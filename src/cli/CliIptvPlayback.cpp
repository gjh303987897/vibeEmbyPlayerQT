#include "cli/CliIptvPlayback.h"
#include "services/iptv/IptvParser.h"
#include "utils/AppLogger.h"
#include <QtConcurrentRun>
#include <QUuid>
#include <algorithm>
#include <utility>

Cli::IptvPlayback::IptvPlayback(SessionRepository& repository, QString path, QObject* parent)
    : QObject(parent),m_repository(repository),m_databasePath(std::move(path))
{
    m_startup.setSingleShot(true); m_startup.setInterval(20000);
    connect(&m_startup,&QTimer::timeout,this,[this] { if (!retry(true) && timeoutExhausted) timeoutExhausted(); });
    m_flushTimer.setSingleShot(true); m_flushTimer.setInterval(2000);
    connect(&m_flushTimer,&QTimer::timeout,this,&IptvPlayback::flush);
    connect(&m_writer,&QFutureWatcherBase::finished,this,[this] {
        const auto result = m_writer.result();
        if (!result) AppLogger::warning("iptv","Unable to persist CLI stream metadata");
        if (!m_dirty.isEmpty()) flush();
    });
    connect(&m_probe,&IptvProbe::finished,this,[this](const auto& id,const QUrl& url,const auto& result) {
        if (!m_channel || m_channel->id != id) return;
        IptvStreamWorkflow::applyProbe(*m_channel,url.toString(),result);
        m_dirty[m_channel->id] = *m_channel; m_flushTimer.start();
        const auto generation = m_generation;
        QTimer::singleShot(150,this,[this,generation] { if (generation == m_generation) nextProbe(); });
    });
}
Cli::IptvPlayback::~IptvPlayback()
{
    stop();
    m_writer.waitForFinished();
    while (!m_dirty.isEmpty()) { flush(); m_writer.waitForFinished(); }
}
std::expected<void,QString> Cli::IptvPlayback::start(const QString& service, const QString& id,
        const QString& preference, const QString& stream, std::function<void(const QString&)> load)
{
    stop();
    auto channels = m_repository.loadIptvChannels(service);
    if (!channels) return std::unexpected(channels.error());
    *channels = IptvParser::mergeChannels(std::move(*channels));
    const auto channel = std::ranges::find(*channels,id,&IptvChannel::id);
    if (channel == channels->end()) return std::unexpected(QStringLiteral("IPTV channel no longer exists"));
    m_channel = *channel; m_preference = preference; m_load = std::move(load);
    if (!next(stream)) { stop(); return std::unexpected(QStringLiteral("Playback stream not found")); }
    scheduleProbe(false,false);
    return {};
}
void Cli::IptvPlayback::stop()
{
    ++m_generation; m_probe.cancel(); m_queue.clear(); m_startup.stop();
    flush(); m_channel.reset(); m_tried.clear(); m_retrying = m_playing = false; m_load = {};
}
bool Cli::IptvPlayback::next(const QString& id)
{
    if (!m_channel) return false;
    if (!id.isEmpty() && id == IptvStreamSelector::selectionId(m_selected) && m_channel->streamUrls.contains(m_selected.sourceUrl)) {
        m_tried.insert(m_selected.playbackUrl); m_retrying = m_playing = false;
        m_startup.start(); m_load(m_selected.playbackUrl); return true;
    }
    const auto candidates = IptvStreamSelector::candidates(*m_channel,m_preference);
    for (const auto& candidate : candidates) {
        if (id.isEmpty() ? m_tried.contains(candidate.playbackUrl) : IptvStreamSelector::selectionId(candidate) != id) continue;
        m_selected = candidate; m_tried.insert(candidate.playbackUrl); m_retrying = m_playing = false;
        m_startup.start(); m_load(candidate.playbackUrl); return true;
    }
    return false;
}
bool Cli::IptvPlayback::retry(bool failed)
{
    if (!m_channel || !failed) return false;
    m_playing = false;
    m_startup.stop();
    IptvStreamWorkflow::markFailed(*m_channel,m_selected); m_dirty[m_channel->id] = *m_channel; m_flushTimer.start();
    const auto candidates = IptvStreamSelector::candidates(*m_channel,m_preference);
    if (std::ranges::none_of(candidates,[this](const auto& candidate) { return !m_tried.contains(candidate.playbackUrl); })) return false;
    if (m_retrying) return true;
    m_retrying = true;
    const auto generation = m_generation;
    QTimer::singleShot(0,this,[this,generation] { if (generation == m_generation) next(); });
    return true;
}
void Cli::IptvPlayback::started() { m_startup.stop(); m_retrying = false; m_playing = true; }
bool Cli::IptvPlayback::switchStream(const QString& id)
{
    if (!m_channel) return false;
    const bool current = IptvStreamSelector::selectionId(m_selected) == id;
    if (current && m_playing) return true;
    const auto candidates = IptvStreamSelector::candidates(*m_channel,m_preference);
    if (!current && std::ranges::none_of(candidates,[&](const auto& item) { return IptvStreamSelector::selectionId(item) == id; })) return false;
    ++m_generation; m_probe.cancel(); m_queue.clear(); m_tried.clear();
    const auto selected = next(id); scheduleProbe(false,false); return selected;
}
QJsonArray Cli::IptvPlayback::streams(bool probe, bool refresh)
{
    if (!m_channel) return {};
    if (probe || refresh) scheduleProbe(true,refresh);
    return IptvStreamWorkflow::rows(*m_channel,m_preference,IptvStreamSelector::selectionId(m_selected),&m_selected);
}
void Cli::IptvPlayback::scheduleProbe(bool detailed, bool refresh)
{
    m_probe.cancel(); m_queue.clear();
    if (!m_channel) return;
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto& source : m_channel->streamUrls) {
        const auto cached = std::ranges::find(m_channel->streamInfo,source,&IptvStreamInfo::url);
        if (!refresh && cached != m_channel->streamInfo.end() && IptvStreamSelector::fresh(*cached)) {
            if (!detailed || (cached->width > 0 && cached->height > 0) ||
                (cached->qualityCheckedAt.isValid() && cached->qualityCheckedAt.secsTo(now) < 60)) continue;
        }
        if (!detailed && m_queue.size() >= 36) break;
        m_queue.append({source,detailed});
    }
    nextProbe();
}
void Cli::IptvPlayback::nextProbe()
{
    if (!m_channel || m_queue.isEmpty()) return;
    const auto target = m_queue.takeFirst();
    m_probe.probe(m_channel->id,QUrl(target.first),m_preference,target.second);
}
void Cli::IptvPlayback::update(const QString& resolution, const QString& codec, const QString& bitrate)
{
    if (!m_channel || m_retrying) return;
    if (IptvStreamWorkflow::applyPlayback(*m_channel,m_selected.sourceUrl,m_selected.playbackUrl,resolution,codec,bitrate)) {
        m_dirty[m_channel->id] = *m_channel;
        if (!m_flushTimer.isActive()) m_flushTimer.start();
    }
}
void Cli::IptvPlayback::flush()
{
    if (m_dirty.isEmpty() || m_writer.isRunning()) return;
    const auto snapshot = *m_dirty.cbegin();
    m_dirty.remove(m_dirty.firstKey());
    m_writer.setFuture(QtConcurrent::run([channel = snapshot,path = m_databasePath] -> std::expected<void,QString> {
        SessionRepository writer("cli-iptv-" + QUuid::createUuid().toString(QUuid::WithoutBraces),path);
        // The playback host already performed the schema migration.
        return writer.updateIptvChannelProbe(channel.playlistId,channel.id,channel.width,channel.height,
            channel.bitrate,channel.codec,channel.probeState,channel.streamInfo);
    }));
}
