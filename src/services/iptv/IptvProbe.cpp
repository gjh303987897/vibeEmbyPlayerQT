#include "services/iptv/IptvProbe.h"
#include "services/iptv/IptvStreamSelector.h"
#include "services/encryptedhls/EncryptedHlsPackager.h"

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
#include <utility>

namespace {
constexpr qint64 maximumPlaylistBytes = 128 * 1024;
constexpr int requestTimeoutMs = 6'000;
bool httpUrl(const QUrl& url)
{
    return url.isValid() && !url.host().isEmpty() &&
        (url.scheme() == "http" || url.scheme() == "https");
}
}

IptvProbe::IptvProbe(QObject* parent)
    : QObject(parent), m_manager(new QNetworkAccessManager(this))
{
}

IptvProbe::~IptvProbe() { cancel(); }

void IptvProbe::probe(const QString& channelId, const QUrl& url, const QString& preference)
{
    cancel();
    m_channelId = channelId;
    m_url = url;
    m_preference = preference;
    if (!httpUrl(url)) {
        QTimer::singleShot(0, this, [this, generation = m_generation] {
            if (generation == m_generation) complete({.state = "unsupported"});
        });
        return;
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT IPTV probe"));
    request.setRawHeader("Range", "bytes=0-131071");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = m_manager->get(request);
    m_activeReply = reply;
    reply->setReadBufferSize(maximumPlaylistBytes);
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply] {
        reply->setProperty("probeTimedOut", true);
        reply->abort();
    });
    timeout->start(requestTimeoutMs);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply != m_activeReply) return;
        m_payload.append(reply->read(maximumPlaylistBytes - m_payload.size()));
        if (m_payload.size() >= maximumPlaylistBytes) {
            reply->setProperty("probeLimitReached", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, timeout] {
        timeout->stop();
        if (reply != m_activeReply) { reply->deleteLater(); return; }
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->isOpen()) m_payload.append(reply->read(maximumPlaylistBytes - m_payload.size()));
        IptvProbeResult result;
        if (status >= 400 || (reply->error() != QNetworkReply::NoError &&
            !reply->property("probeLimitReached").toBool() &&
            !(reply->property("probeTimedOut").toBool() && status >= 200 && !m_payload.isEmpty()))) {
            result.state = reply->property("probeTimedOut").toBool() ? "timeout" : "unavailable";
            result.error = status >= 400 ? QString("HTTP %1").arg(status) : QString("Network probe failed");
        } else {
            result = parse(m_payload, status, m_preference, reply->url());
        }
        finish(reply, result);
    });
}

void IptvProbe::cancel()
{
    ++m_generation;
    // abort() emits finished synchronously; detach identity first.
    if (auto* reply = std::exchange(m_activeReply, nullptr)) {
        reply->abort();
        reply->deleteLater();
    }
    if (auto* process = std::exchange(m_process, nullptr)) {
        process->disconnect(this);
        // Keep the sample alive until FFprobe has released it (Windows file sharing).
        if (auto* sample = std::exchange(m_sample, nullptr)) sample->setParent(process);
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                process, &QObject::deleteLater);
        process->kill();
        if (process->state() == QProcess::NotRunning) process->deleteLater();
    }
    delete std::exchange(m_sample, nullptr);
    m_channelId.clear();
    m_url = QUrl {};
    m_payload.clear();
}

void IptvProbe::finish(QNetworkReply* reply, const IptvProbeResult& result)
{
    if (reply != m_activeReply) { reply->deleteLater(); return; }
    m_activeReply = nullptr;
    reply->deleteLater();
    if (result.available && result.variants.empty() && !m_payload.trimmed().startsWith("#EXTM3U"))
        probePrefix(m_payload, result);
    else complete(result);
}

void IptvProbe::complete(const IptvProbeResult& result)
{
    const auto channel = std::exchange(m_channelId, QString {});
    const auto url = std::exchange(m_url, QUrl {});
    m_payload.clear();
    delete std::exchange(m_sample, nullptr);
    emit finished(channel, url, result);
}

void IptvProbe::probePrefix(const QByteArray& payload, IptvProbeResult result)
{
#ifdef Q_OS_WIN
    const auto name = QStringLiteral("ffprobe.exe");
#else
    const auto name = QStringLiteral("ffprobe");
#endif
    auto executable = QDir(QFileInfo(EncryptedHlsPackager::locateFfmpegExecutable()).absolutePath()).filePath(name);
    if (!QFileInfo(executable).isExecutable()) executable = QStandardPaths::findExecutable(name);
    if (executable.isEmpty() || payload.isEmpty()) { complete(result); return; }
    // Probe only the bounded downloaded sample, never let FFprobe fetch a live stream.
    m_sample = new QTemporaryFile(this);
    if (!m_sample->open() || m_sample->write(payload) != payload.size() || !m_sample->flush()) {
        complete(result); return;
    }
    auto* process = new QProcess(this);
    m_process = process;
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this, process, timeout, result](int code, QProcess::ExitStatus status) mutable {
            timeout->stop();
            if (process != m_process) return;
            m_process = nullptr;
            if (code == 0 && status == QProcess::NormalExit) {
                const auto streams = QJsonDocument::fromJson(process->readAllStandardOutput()).object().value("streams").toArray();
                for (const auto& value : streams) {
                    const auto video = value.toObject();
                    if (video.value("codec_type").toString() != "video") continue;
                    result.width = video.value("width").toInt();
                    result.height = video.value("height").toInt();
                    result.bitrate = video.value("bit_rate").toString().toLongLong();
                    result.codec = video.value("codec_name").toString();
                    break;
                }
            }
            process->deleteLater();
            complete(result);
        });
    connect(process, &QProcess::errorOccurred, this, [this, process, result](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && process == m_process) {
            m_process = nullptr;
            process->deleteLater();
            complete(result);
        }
    });
    process->start(executable, {"-v", "quiet", "-protocol_whitelist", "file,pipe", "-probesize", "131072",
        "-analyzeduration", "1000000", "-show_entries", "stream=codec_type,codec_name,width,height,bit_rate",
        "-of", "json", "-i", m_sample->fileName()}, QIODevice::ReadOnly);
    timeout->start(2000);
}

IptvProbeResult IptvProbe::parse(const QByteArray& payload, int statusCode,
                                const QString& preference, const QUrl& baseUrl)
{
    IptvProbeResult result {.available = statusCode >= 200 && statusCode < 300 && !payload.isEmpty(), .state = "available"};
    if (!result.available) { result.state = "unavailable"; return result; }
    const auto lines = QString::fromUtf8(payload).split(QRegularExpression("\r\n|\n|\r"));
    if (std::ranges::any_of(lines, [](const QString& line) {
            return line.startsWith("#EXT-X-STREAM-INF:") &&
                (line.contains("AUDIO=") || line.contains("SUBTITLES="));
        })) return result;
    for (int index = 0; index + 1 < lines.size(); ++index) {
        const auto line = lines.at(index).trimmed();
        if (!line.startsWith("#EXT-X-STREAM-INF:")) continue;
        const auto resolution = QRegularExpression(R"((?:^|,)RESOLUTION=(\d+)x(\d+))").match(line.mid(18));
        const auto bandwidth = QRegularExpression(R"((?:^|,)(?:AVERAGE-)?BANDWIDTH=(\d+))").match(line.mid(18));
        const auto codec = QRegularExpression("CODECS=\"([^\"]+)\"").match(line);
        int uriIndex = index + 1;
        while (uriIndex < lines.size() && lines.at(uriIndex).trimmed().isEmpty()) ++uriIndex;
        if (uriIndex >= lines.size() || lines.at(uriIndex).trimmed().startsWith('#')) continue;
        const auto variantUrl = baseUrl.resolved(QUrl(lines.at(uriIndex).trimmed()));
        if (!httpUrl(variantUrl)) continue;
        result.variants.push_back({variantUrl.toString(), resolution.captured(1).toInt(),
            resolution.captured(2).toInt(), bandwidth.captured(1).toLongLong(), codec.captured(1)});
    }
    if (!result.variants.empty()) {
        IptvChannel channel {.streamUrl = baseUrl.toString(), .streamUrls = {baseUrl.toString()}};
        channel.streamInfo.push_back({.url = baseUrl.toString(), .available = true,
            .checkedAt = QDateTime::currentDateTimeUtc(), .variants = result.variants});
        const auto selected = IptvStreamSelector::select(channel, preference).info;
        result.width = selected.width;
        result.height = selected.height;
        result.bitrate = selected.bitrate;
        result.codec = selected.codec;
    }
    return result;
}
