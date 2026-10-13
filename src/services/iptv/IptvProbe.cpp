#include "services/iptv/IptvProbe.h"
#include "services/iptv/IptvStreamSelector.h"
#include "services/encryptedhls/EncryptedHlsPackager.h"
#include "utils/AppLogger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <algorithm>
#include <limits>
#include <utility>

namespace {
constexpr qint64 lightBytes = 128 * 1024;
constexpr qint64 sampleBytes = 512 * 1024;
constexpr qint64 routeBytes = 2 * 1024 * 1024;
bool httpUrl(const QUrl& url)
{
    return url.isValid() && !url.host().isEmpty() &&
        (url.scheme() == "http" || url.scheme() == "https");
}
QString attribute(const QString& line, const QString& key)
{
    const auto match = QRegularExpression("(?:^|,)" + key + "=(?:\"([^\"]*)\"|([^,]*))")
        .match(line.mid(line.indexOf(':') + 1));
    return match.captured(1).isNull() ? match.captured(2) : match.captured(1);
}
std::vector<IptvStreamVariant> variants(const QByteArray& payload, const QUrl& base, bool& external)
{
    std::vector<IptvStreamVariant> result;
    const auto lines = QString::fromUtf8(payload).split('\n');
    for (int i = 0; i + 1 < lines.size(); ++i) {
        const auto line = lines[i].trimmed();
        if (!line.startsWith("#EXT-X-STREAM-INF:")) continue;
        external |= !attribute(line, "AUDIO").isEmpty() || !attribute(line, "SUBTITLES").isEmpty();
        int next = i + 1;
        while (next < lines.size() && lines[next].trimmed().isEmpty()) ++next;
        if (next == lines.size() || lines[next].trimmed().startsWith('#')) continue;
        const auto url = base.resolved(QUrl(lines[next].trimmed()));
        if (!httpUrl(url)) continue;
        const auto resolution = attribute(line, "RESOLUTION").split('x');
        auto bitrate = attribute(line, "AVERAGE-BANDWIDTH").toLongLong();
        if (bitrate <= 0) bitrate = attribute(line, "BANDWIDTH").toLongLong();
        result.push_back({url.toString(), resolution.value(0).toInt(), resolution.value(1).toInt(),
            bitrate, attribute(line, "CODECS")});
        if (result.size() == 64) break; // Bound untrusted master playlists.
    }
    return result;
}
IptvStreamInfo preferred(const std::vector<IptvStreamVariant>& entries, const QString& preference)
{
    IptvChannel channel {.streamUrls = {"master"}};
    channel.streamInfo.push_back({.url = "master", .available = true,
        .checkedAt = QDateTime::currentDateTimeUtc(), .variants = entries});
    return IptvStreamSelector::select(channel, preference).info;
}
bool byteRange(const QString& value, qint64& offset, qint64& length)
{
    if (value.isEmpty()) { offset = 0; length = -1; return true; }
    const auto parts = value.split('@');
    bool validLength = false, validOffset = true;
    length = parts.value(0).toLongLong(&validLength);
    offset = parts.size() > 1 ? parts[1].toLongLong(&validOffset) : 0;
    return validLength && validOffset && length > 0 && offset >= 0
        && offset <= std::numeric_limits<qint64>::max() - sampleBytes;
}
}

IptvProbe::IptvProbe(QObject* parent)
    : QObject(parent), m_manager(new QNetworkAccessManager(this)), m_deadline(new QTimer(this))
{
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this] { finishRoute("timeout"); });
}
IptvProbe::~IptvProbe() { cancel(); }

void IptvProbe::probe(const QString& channelId, const QUrl& url, const QString& preference, bool detailed)
{
    cancel();
    m_channelId = channelId;
    m_url = url;
    m_preference = preference;
    m_detailed = detailed;
    m_deadline->start(detailed ? 12000 : 8000);
    m_targets.push_back({url});
    QTimer::singleShot(0, this, [this, generation = m_generation] {
        if (generation == m_generation) nextTarget();
    });
}

void IptvProbe::cancel()
{
    ++m_generation;
    m_deadline->stop();
    if (auto* reply = std::exchange(m_activeReply, nullptr)) {
        reply->abort();
        reply->deleteLater();
    }
    if (auto* process = std::exchange(m_process, nullptr)) {
        process->disconnect(this);
        if (auto* sample = std::exchange(m_sample, nullptr)) sample->setParent(process);
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, &QObject::deleteLater);
        process->kill();
        if (process->state() == QProcess::NotRunning) process->deleteLater();
    }
    delete std::exchange(m_sample, nullptr);
    m_channelId.clear();
    m_url = QUrl {};
    m_payload.clear();
    m_initialization.clear();
    m_targets.clear();
    m_result = {};
    m_requests = 0;
    m_bytes = 0;
    m_depth = 0;
    m_variant = -1;
}

void IptvProbe::nextTarget()
{
    if (m_targets.empty()) { finishRoute(); return; }
    const auto target = m_targets.front();
    m_targets.pop_front();
    m_variant = target.variant;
    m_depth = 0;
    m_initialization.clear();
    fetch(target.url);
}

void IptvProbe::fetch(const QUrl& url, FetchKind kind, qint64 offset, qint64 length, bool range)
{
    if (!httpUrl(url)) { targetFinished({.state = "unsupported"}); return; }
    if (++m_requests > 12 || m_bytes >= routeBytes) { finishRoute("probe-limit"); return; }
    if (kind == FetchKind::Document && ++m_depth > 4) { targetFinished({.state = "probe-limit"}); return; }
    const auto cap = std::min({m_detailed ? sampleBytes : lightBytes, routeBytes - m_bytes,
        length > 0 ? length : sampleBytes});
    m_payload.clear();
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT IPTV probe"));
    if (range) request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + "-" + QByteArray::number(offset + cap - 1));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(4);
    auto* reply = m_manager->get(request);
    m_activeReply = reply;
    reply->setReadBufferSize(cap);
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply] {
        reply->setProperty("probeTimedOut", true);
        reply->abort();
    });
    timeout->start(m_detailed ? 4000 : 6000);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, cap] {
        if (reply != m_activeReply) return;
        const auto bytes = reply->read(cap - m_payload.size());
        m_bytes += bytes.size();
        m_payload += bytes;
        if (m_payload.size() >= cap) {
            reply->setProperty("probeLimitReached", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, timeout, cap, url, kind, offset, length, range] {
        timeout->stop();
        if (reply != m_activeReply) { reply->deleteLater(); return; }
        m_activeReply = nullptr;
        reply->deleteLater();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->isOpen()) {
            const auto bytes = reply->read(cap - m_payload.size());
            m_bytes += bytes.size();
            m_payload += bytes;
        }
        // Some live endpoints reject Range. The bounded reader also works with GET.
        if (range && offset == 0 && (status == 400 || status == 416)) {
            if (kind == FetchKind::Document) --m_depth; // Retry the same document, not a child playlist.
            fetch(url, kind, offset, length, false);
            return;
        }
        if (offset > 0 && (status != 206 ||
            !reply->rawHeader("Content-Range").startsWith("bytes " + QByteArray::number(offset) + "-"))) {
            targetFinished({.state = "range-unsupported"});
            return;
        }
        if (status < 200 || status >= 300 || m_payload.isEmpty() ||
            (reply->error() != QNetworkReply::NoError && !reply->property("probeLimitReached").toBool() &&
             !(reply->property("probeTimedOut").toBool() && !m_payload.isEmpty()))) {
            targetFinished({.state = reply->property("probeTimedOut").toBool() ? "timeout" : "unavailable"});
            return;
        }
        const auto payload = std::exchange(m_payload, {});
        consume(payload, reply->url(), kind);
    });
}

void IptvProbe::consume(const QByteArray& payload, const QUrl& url, FetchKind kind)
{
    if (kind == FetchKind::Initialization) {
        m_initialization = payload;
        fetch(m_segmentUrl, FetchKind::Segment, m_segmentOffset, m_segmentLength);
        return;
    }
    if (kind == FetchKind::Segment || !payload.trimmed().startsWith("#EXTM3U")) {
        probePrefix(m_initialization + payload, {.available = true, .state = "sample-incomplete"});
        return;
    }
    auto parsed = parse(payload, 200, m_preference, url);
    if (!m_detailed) { targetFinished(parsed); return; }
    bool external = false;
    const auto master = variants(payload, url, external);
    if (!master.empty()) {
        if (m_variant == -1 && m_depth == 1 && !external) {
            m_result = parsed;
            for (int i = 0; i < static_cast<int>(master.size()); ++i) {
                if (master[i].width <= 0 || master[i].height <= 0)
                    m_targets.push_back({QUrl(master[i].url), i});
            }
            nextTarget();
        } else {
            // Keep playback on masters with separate audio/subtitle groups.
            // Sampling one video child must not remove those playback tracks.
            if (parsed.width > 0 && parsed.height > 0) { targetFinished(parsed); return; }
            fetch(QUrl(master.front().url));
        }
        return;
    }
    QUrl initializationUrl;
    qint64 initOffset = 0, initLength = -1;
    QString segmentRange;
    bool encrypted = false;
    m_segmentUrl = QUrl {};
    for (const auto& raw : QString::fromUtf8(payload).split('\n')) {
        const auto line = raw.trimmed();
        if (line.startsWith("#EXT-X-KEY:"))
            encrypted = attribute(line, "METHOD") != "NONE";
        else if (line.startsWith("#EXT-X-MAP:")) {
            initializationUrl = url.resolved(QUrl(attribute(line, "URI")));
            if (!byteRange(attribute(line, "BYTERANGE"), initOffset, initLength)) {
                targetFinished({.available = true, .state = "range-unsupported"}); return;
            }
        } else if (line.startsWith("#EXT-X-BYTERANGE:")) segmentRange = line.mid(17);
        else if (!line.isEmpty() && !line.startsWith('#')) {
            m_segmentUrl = url.resolved(QUrl(line));
            break; // One published segment is enough; never follow the live playlist.
        }
    }
    if (encrypted) { targetFinished({.available = true, .state = "encrypted"}); return; }
    if (m_segmentUrl.isEmpty()) { targetFinished({.available = true, .state = "sample-incomplete"}); return; }
    if (!byteRange(segmentRange, m_segmentOffset, m_segmentLength)) {
        targetFinished({.available = true, .state = "range-unsupported"}); return;
    }
    if (!initializationUrl.isEmpty()) fetch(initializationUrl, FetchKind::Initialization, initOffset, initLength);
    else fetch(m_segmentUrl, FetchKind::Segment, m_segmentOffset, m_segmentLength);
}

void IptvProbe::targetFinished(const IptvProbeResult& result)
{
    if (m_variant >= 0 && m_variant < static_cast<int>(m_result.variants.size())) {
        auto& variant = m_result.variants[m_variant];
        variant.width = result.width;
        variant.height = result.height;
        if (result.bitrate > 0) variant.bitrate = result.bitrate;
        if (!result.codec.isEmpty()) variant.codec = result.codec;
        variant.probeState = result.state;
    } else m_result = result;
    QTimer::singleShot(0, this, [this, generation = m_generation] {
        if (generation == m_generation) nextTarget();
    });
}

void IptvProbe::finishRoute(const QString& incompleteState)
{
    if (!incompleteState.isEmpty()) {
        if (m_result.width <= 0 || m_result.height <= 0) m_result.state = incompleteState;
        for (auto& variant : m_result.variants)
            if (variant.width <= 0 || variant.height <= 0) variant.probeState = incompleteState;
    }
    if (!m_result.variants.empty()) {
        const auto selected = preferred(m_result.variants, m_preference);
        m_result.width = selected.width;
        m_result.height = selected.height;
        m_result.bitrate = selected.bitrate;
        m_result.codec = selected.codec;
    }
    complete(m_result);
}

void IptvProbe::complete(const IptvProbeResult& result)
{
    auto copy = result; // result can refer to m_result, which cancel() clears.
    copy.detailed = m_detailed;
    if (m_detailed) AppLogger::info(QStringLiteral("iptv-probe"),
        QStringLiteral("Quality probe finished: %1, %2x%3, %4 requests, %5 sample bytes")
            .arg(copy.state).arg(copy.width).arg(copy.height).arg(m_requests).arg(m_bytes));
    const auto channel = m_channelId;
    const auto url = m_url;
    cancel();
    emit finished(channel, url, copy);
}

void IptvProbe::probePrefix(const QByteArray& payload, IptvProbeResult result)
{
#ifdef Q_OS_WIN
    const auto name = QStringLiteral("ffprobe.exe");
#else
    const auto name = QStringLiteral("ffprobe");
#endif
    const auto ffmpeg = EncryptedHlsPackager::locateFfmpegExecutable();
    auto executable = ffmpeg.isEmpty() ? QString() : QDir(QFileInfo(ffmpeg).absolutePath()).filePath(name);
    if (executable.isEmpty() || !QFileInfo(executable).isExecutable())
        executable = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    if (!QFileInfo(executable).isExecutable()) executable = QStandardPaths::findExecutable(name);
    if (executable.isEmpty()) { result.state = "tool-missing"; targetFinished(result); return; }
    m_sample = new QTemporaryFile(this);
    if (!m_sample->open() || m_sample->write(payload) != payload.size() || !m_sample->flush()) {
        delete std::exchange(m_sample, nullptr);
        result.state = "tool-error"; targetFinished(result); return;
    }
    auto* process = new QProcess(this);
    m_process = process;
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process] {
        process->setProperty("timedOut", true);
        process->kill();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, process, timeout, result](int code, QProcess::ExitStatus status) mutable {
            timeout->stop();
            if (process != m_process) return;
            m_process = nullptr;
            if (code == 0 && status == QProcess::NormalExit) {
                const auto streams = QJsonDocument::fromJson(process->readAllStandardOutput()).object().value("streams").toArray();
                bool audio = false;
                for (const auto& value : streams) {
                    const auto video = value.toObject();
                    audio |= video.value("codec_type").toString() == "audio";
                    if (video.value("codec_type").toString() != "video") continue;
                    result.width = video.value("width").toInt();
                    result.height = video.value("height").toInt();
                    result.bitrate = video.value("bit_rate").toString().toLongLong();
                    result.codec = video.value("codec_name").toString();
                    if (result.width > 0 && result.height > 0) result.state = "measured";
                    break;
                }
                if (audio && result.codec.isEmpty()) result.state = "audio-only";
            } else if (process->property("timedOut").toBool()) result.state = "timeout";
            process->deleteLater();
            delete std::exchange(m_sample, nullptr);
            targetFinished(result);
        });
    connect(process, &QProcess::errorOccurred, this, [this, process, result](QProcess::ProcessError error) mutable {
        if (error == QProcess::FailedToStart && process == m_process) {
            m_process = nullptr;
            process->deleteLater();
            delete std::exchange(m_sample, nullptr);
            result.state = "tool-error";
            targetFinished(result);
        }
    });
    // Only parse a local bounded sample, with one decoder thread and no network protocols.
    process->start(executable, {"-v", "quiet", "-protocol_whitelist", "file,pipe", "-threads", "1",
        "-probesize", QString::number(payload.size()), "-analyzeduration", "1000000",
        "-show_entries", "stream=codec_type,codec_name,width,height,bit_rate", "-of", "json",
        "-i", m_sample->fileName()}, QIODevice::ReadOnly);
    timeout->start(2000);
}

IptvProbeResult IptvProbe::parse(const QByteArray& payload, int statusCode,
                                const QString& preference, const QUrl& baseUrl)
{
    IptvProbeResult result {.available = statusCode >= 200 && statusCode < 300 && !payload.isEmpty(), .state = "available"};
    if (!result.available) { result.state = "unavailable"; return result; }
    bool external = false;
    result.variants = variants(payload, baseUrl, external);
    if (!result.variants.empty()) {
        const auto selected = preferred(result.variants, preference);
        result.width = selected.width;
        result.height = selected.height;
        result.bitrate = selected.bitrate;
        result.codec = selected.codec;
    }
    if (external) result.variants.clear();
    return result;
}
