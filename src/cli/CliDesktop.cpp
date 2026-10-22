#include "cli/CliEngine.h"
#include "cli/CliRuntime.h"
#include "cli/CliIpc.h"
#include "services/local/LocalMediaService.h"
#include "services/scheduler/ScheduledPlaybackSchedule.h"
#include "services/iptv/IptvParser.h"
#include "services/iptv/IptvStreamWorkflow.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QTime>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>

namespace {
QJsonObject historyJson(const PlaybackHistoryItem& item)
{
    return {{"id",item.id},{"source",playbackHistorySourceToString(item.source)},{"serviceId",item.serviceId},
        {"serviceName",item.serviceName},{"title",item.title},{"date",item.playedDate.toString(Qt::ISODate)},
        {"playedAt",item.playedAt.toString(Qt::ISODate)},{"position",item.positionSeconds},
        {"duration",item.durationSeconds},{"completed",item.completed},{"private",item.privacyMode}};
}
QJsonObject scheduleJson(const ScheduledPlaybackTask& task)
{
    const auto next = ScheduledPlaybackSchedule::nextOccurrence(task,QDateTime::currentDateTime());
    return {{"id",task.id},{"serviceId",task.serverId},{"serviceName",task.serverName},
        {"scheduleType",task.scheduleType},{"time",task.startTime},{"days",task.scheduleDays},
        {"durationMinutes",task.durationMinutes},{"enabled",task.enabled},{"private",task.privateMode},
        {"lastRunDate",task.lastRunDate},{"nextRun",next ? next->toString(Qt::ISODate) : QString()}};
}
template<typename T, typename Key>
std::expected<T,Cli::Error> select(const std::vector<T>& values, const QString& value, Key name)
{
    for (const auto& item : values) if (item.id == value) return item;
    std::optional<T> selected;
    for (const auto& item : values) if (std::invoke(name,item) == value) {
        if (selected) return std::unexpected(Cli::Error {Cli::ExitCode::Usage,"Name is ambiguous; use the complete ID"});
        selected = item;
    }
    if (selected) return *selected;
    return std::unexpected(Cli::Error {Cli::ExitCode::NotFound,"Entry not found"});
}
}

Cli::Result Cli::Engine::desktop(const Invocation& invocation, SessionRepository& repository)
{
    const auto& command = invocation.command;
    if (!repository.databaseExists() && (command == "local list" || command == "schedule list")) return success(QJsonArray {});
    if (command.startsWith("local ")) {
        if (!repository.databaseExists()) return failure(ExitCode::NotFound,"Local folder not found");
        auto roots = repository.loadLocalMediaRoots();
        if (!roots) return failure(ExitCode::Failed,roots.error());
        if (command == "local list") {
            QJsonArray rows;
            for (const auto& root : *roots) rows += QJsonObject {{"id",root.id},{"name",root.name},{"path",root.path}};
            return success(rows);
        }
        if (command == "local add") {
            const auto input = invocation.arguments[0];
            auto listing = awaitFuture(QtConcurrent::run([input] { return LocalMediaService::browseDirectoryWithinRoot(input,input); }));
            if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
            if (!listing) return failure(ExitCode::NotFound,listing.error());
            for (const auto& root : *roots) if (root.path == listing->path) return failure(ExitCode::Conflict,"Folder is already configured");
            LocalMediaRoot root {.id = QUuid::createUuid().toString(QUuid::WithoutBraces),
                .name = invocation.option("name",QFileInfo(listing->path).fileName()).trimmed(),
                .path = listing->path,.sortOrder = static_cast<int>(roots->size()),.available = true};
            if (root.name.isEmpty()) root.name = root.path;
            const auto result = repository.saveLocalMediaRoot(root);
            return result ? success(QJsonObject {{"id",root.id},{"name",root.name},{"path",root.path}}) : failure(ExitCode::Failed,result.error());
        }
        auto root = select(*roots,invocation.arguments[0],&LocalMediaRoot::name);
        if (!root) return failure(root.error());
        if (command == "local remove") {
            if (!confirm(invocation,"Remove this configured folder? Files will be retained.")) return failure(ExitCode::Usage,"Removal requires --yes or confirmation");
            const auto result = repository.deleteLocalMediaRoot(root->id);
            return result ? success(QJsonObject {{"removed",root->id}}) : failure(ExitCode::Failed,result.error());
        }
        const auto target = invocation.arguments.value(1,".");
        const auto path = QDir(root->path).absoluteFilePath(target);
        const auto directory = command == "local play" ? QFileInfo(path).absolutePath() : path;
        auto listing = awaitFuture(QtConcurrent::run([root = root->path,directory] { return LocalMediaService::browseDirectoryWithinRoot(root,directory); }));
        if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
        if (!listing) return failure(ExitCode::Usage,listing.error());
        if (command == "local play") {
            const auto canonical = QFileInfo(path).canonicalFilePath();
            if (std::ranges::none_of(listing->items,[&](const auto& item) { return !item.directory && QFileInfo(item.path).canonicalFilePath() == canonical; }))
                return failure(ExitCode::NotFound,"Choose a supported video inside the configured folder");
            auto play = invocation; play.command = "play"; play.arguments = {canonical};
            return media(play,repository);
        }
        QJsonArray rows;
        for (const auto& item : listing->items) rows += QJsonObject {{"name",item.name},{"path",item.path},
            {"directory",item.directory},{"size",item.size},{"modifiedAt",item.lastModified.toString(Qt::ISODate)}};
        return success(rows);
    }
    if (command.startsWith("playback-history ")) {
        if (command == "playback-history stats") {
            if (!repository.databaseExists()) return success(QJsonArray {});
            const auto stats = repository.loadDailyUsageStats(m_private);
            if (!stats) return failure(ExitCode::Failed,stats.error());
            QJsonArray rows;
            for (const auto& stat : *stats) rows += QJsonObject {{"date",stat.date},{"serviceId",stat.serviceId},
                {"serviceName",stat.serviceName},{"source",stat.serviceType},{"watchSeconds",stat.watchSeconds},
                {"bytesIn",stat.networkBytesIn},{"bytesOut",stat.networkBytesOut},
                {"keepAliveBytesIn",stat.keepAliveNetworkBytesIn},{"keepAliveBytesOut",stat.keepAliveNetworkBytesOut}};
            return success(rows);
        }
        const auto date = QDate::fromString(invocation.option("date"),Qt::ISODate);
        if (invocation.has("date") && (!date.isValid() || date.toString(Qt::ISODate) != invocation.option("date"))) return failure(ExitCode::Usage,"Expected date YYYY-MM-DD");
        if (command == "playback-history clear") {
            if (!date.isValid()) return failure(ExitCode::Usage,"--date is required");
            if (!confirm(invocation,"Delete playback history for this date?")) return failure(ExitCode::Usage,"Deletion requires --yes or confirmation");
            const auto result = repository.deletePlaybackHistoryForDate(m_private,date);
            return result ? success(QJsonObject {{"removedDate",date.toString(Qt::ISODate)}}) : failure(ExitCode::Failed,result.error());
        }
        bool offsetOk = true, limitOk = true;
        const auto offset = invocation.option("offset","0").toInt(&offsetOk), limit = invocation.option("limit","100").toInt(&limitOk);
        const auto source = playbackHistorySourceFromString(invocation.option("source"));
        if (!offsetOk || !limitOk || offset < 0 || limit < 1 || limit > 1000 ||
            (invocation.has("source") && source == PlaybackHistorySource::Unknown)) return failure(ExitCode::Usage,"Invalid source or pagination; limit must be 1..1000");
        if (!repository.databaseExists()) return command == "playback-history list" ? success(QJsonArray {}) : failure(ExitCode::NotFound,"Playback record not found");
        std::vector<PlaybackHistoryItem> entries;
        if (date.isValid()) {
            auto loaded = repository.loadPlaybackHistoryForDate(m_private,date);
            if (!loaded) return failure(ExitCode::Failed,loaded.error());
            for (auto& item : *loaded) if (source == PlaybackHistorySource::Unknown || item.source == source) entries.push_back(std::move(item));
        } else {
            const bool lookup = command != "playback-history list";
            for (int start = lookup ? 0 : offset;;start += 1000) {
                auto loaded = repository.loadPlaybackHistory(m_private,start,lookup ? 1000 : limit,source);
                if (!loaded) return failure(ExitCode::Failed,loaded.error());
                const auto count = loaded->size();
                entries.insert(entries.end(),loaded->begin(),loaded->end());
                if (!lookup || count < 1000 || std::ranges::any_of(entries,[&](const auto& item) { return item.id == invocation.arguments.value(0); })) break;
                if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
            }
        }
        if (command == "playback-history list") {
            QJsonArray rows;
            const size_t begin = date.isValid() ? std::min<size_t>(offset,entries.size()) : 0;
            for (size_t index = begin; index < entries.size() && rows.size() < limit; ++index) rows += historyJson(entries[index]);
            return success(rows);
        }
        const auto item = std::ranges::find(entries,invocation.arguments[0],&PlaybackHistoryItem::id);
        if (item == entries.end()) return failure(ExitCode::NotFound,"Playback record not found");
        if (command == "playback-history show") return success(historyJson(*item));
        if (command == "playback-history remove") {
            if (!confirm(invocation,"Delete this playback record?")) return failure(ExitCode::Usage,"Deletion requires --yes or confirmation");
            const auto result = repository.deletePlaybackHistory(item->id);
            return result ? success(QJsonObject {{"removed",item->id}}) : failure(ExitCode::Failed,result.error());
        }
        auto play = invocation; play.command = "play";
        play.arguments = item->serviceId.isEmpty() || item->source == PlaybackHistorySource::Local || item->source == PlaybackHistorySource::Link
            ? QStringList {item->replayTarget} : QStringList {item->serviceId,item->replayTarget};
        if (item->source == PlaybackHistorySource::WebDav) {
            const QUrl target(item->replayTarget);
            if (target.scheme() == "http" || target.scheme() == "https") {
                auto service = resolve(repository,item->serviceId,true);
                if (!service) return failure(service.error());
                const QUrl root(service->server.baseUrl);
                auto prefix = root.path(); if (!prefix.endsWith('/')) prefix += '/';
                if (target.scheme() != root.scheme() || target.host() != root.host() || target.port() != root.port() || !target.path().startsWith(prefix))
                    return failure(ExitCode::Usage,"History target is outside its saved service");
                play.arguments[1] = '/' + target.path().mid(prefix.size());
            }
        }
        if (!play.has("start") && item->source != PlaybackHistorySource::Iptv) play.options["start"] = QString::number(item->completed ? 0 : item->positionSeconds);
        return media(play,repository);
    }
    if (command == "iptv streams") {
        auto card = resolve(repository,invocation.arguments[0]);
        if (!card) return failure(card.error());
        if (card->server.serviceType != ServiceType::IPTV) return failure(ExitCode::Usage,"This command requires IPTV");
        auto channels = repository.loadIptvChannels(card->server.id);
        if (!channels) return failure(ExitCode::Failed,channels.error());
        auto channel = select(IptvParser::mergeChannels(std::move(*channels)),invocation.arguments[1],&IptvChannel::name);
        if (!channel) return failure(channel.error());
        if (invocation.has("probe") || invocation.has("refresh")) {
            IptvProbe probe;
            for (const auto& source : channel->streamUrls) {
                const auto cached = std::ranges::find(channel->streamInfo,source,&IptvStreamInfo::url);
                if (!invocation.has("refresh") && cached != channel->streamInfo.end() && IptvStreamSelector::fresh(*cached) &&
                    ((cached->width > 0 && cached->height > 0) ||
                    (cached->qualityCheckedAt.isValid() && cached->qualityCheckedAt.secsTo(QDateTime::currentDateTimeUtc()) < 60))) continue;
                std::function<void(IptvProbeResult)> complete;
                const auto connection = QObject::connect(&probe,&IptvProbe::finished,&probe,[&](const auto&,const auto&,const IptvProbeResult& value) { if (complete) complete(value); });
                const auto result = await<IptvProbeResult>([&](auto done) {
                    complete = done; probe.probe(channel->id,QUrl(source),repository.iptvStreamPreference(),true);
                },[&] { probe.cancel(); if (complete) complete(IptvProbeResult {.state = "canceled"}); });
                QObject::disconnect(connection);
                if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
                IptvStreamWorkflow::applyProbe(*channel,source,result);
                CliConsole::progress(QStringLiteral("IPTV probe: %1 / %2").arg(channel->streamUrls.indexOf(source) + 1).arg(channel->streamUrls.size()));
            }
            const auto selected = IptvStreamSelector::select(*channel,repository.iptvStreamPreference());
            if (selected.info.available) {
                channel->width = selected.info.width; channel->height = selected.info.height;
                channel->bitrate = selected.info.bitrate; channel->codec = selected.info.codec; channel->probeState = selected.info.state;
            }
            const auto result = repository.updateIptvChannelProbe(channel->playlistId,channel->id,channel->width,channel->height,
                channel->bitrate,channel->codec,channel->probeState,channel->streamInfo);
            if (!result) return failure(ExitCode::Failed,result.error());
        }
        return success(IptvStreamWorkflow::rows(*channel,repository.iptvStreamPreference()));
    }
    if (command == "schedule watch" || command == "schedule run" || command == "schedule status" || command == "schedule stop" || command == "schedule unwatch" || command == "schedule resolve") {
        return rpc("schedule",m_profile,QJsonObject {{"action",command},{"id",invocation.arguments.value(0)},
            {"private",m_private},{"pin",m_private ? m_pin : QString()},{"run",invocation.option("run")}},
            command == "schedule watch" || command == "schedule run");
    }
    if (!repository.databaseExists()) return failure(ExitCode::NotFound,"Scheduled task not found");
    auto tasks = repository.loadScheduledPlaybackTasks(m_private);
    if (!tasks) return failure(ExitCode::Failed,tasks.error());
    if (command == "schedule list") {
        QJsonArray rows; for (const auto& task : *tasks) rows += scheduleJson(task); return success(rows);
    }
    ScheduledPlaybackTask task;
    if (command != "schedule add") {
        const auto it = std::ranges::find(*tasks,invocation.arguments[0],&ScheduledPlaybackTask::id);
        if (it == tasks->end()) return failure(ExitCode::NotFound,"Scheduled task not found");
        task = *it;
        if (command == "schedule show") return success(scheduleJson(task));
        if (command == "schedule remove") {
            if (!confirm(invocation,"Delete this scheduled playback task?")) return failure(ExitCode::Usage,"Deletion requires --yes or confirmation");
            const auto result = repository.deleteScheduledPlaybackTask(task.id);
            return result ? success(QJsonObject {{"removed",task.id}}) : failure(ExitCode::Failed,result.error());
        }
    } else { task.id = QUuid::createUuid().toString(QUuid::WithoutBraces); task.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }
    const auto previousService = task.serverId;
    auto service = resolve(repository,invocation.option("service",task.serverId));
    if (!service) return failure(service.error());
    if (service->server.serviceType != ServiceType::Emby) return failure(ExitCode::Usage,"Scheduled playback requires an Emby service");
    const auto session = repository.loadSession(service->server.id);
    if (!session) return failure(ExitCode::Failed,session.error());
    if (!*session) return failure(ExitCode::Authentication,"Log in to the Emby service first");
    task.serverId = service->server.id; task.serverName = service->server.name; task.username = service->server.username; task.privateMode = service->server.privateMode;
    const auto oldSchedule = task.scheduleType + task.startTime + task.scheduleDays;
    task.scheduleType = invocation.option("schedule-type",task.scheduleType);
    if (!ScheduledPlaybackSchedule::isSupportedType(task.scheduleType)) return failure(ExitCode::Usage,"Invalid schedule type");
    task.startTime = invocation.option("time",task.startTime);
    task.scheduleDays = invocation.option("days",task.scheduleDays);
    if (task.scheduleType == "manual") { task.startTime = "manual"; task.scheduleDays.clear(); }
    else {
        const auto time = QTime::fromString(task.startTime,"HH:mm");
        if (!time.isValid() || time.toString("HH:mm") != task.startTime) return failure(ExitCode::Usage,"Automatic schedules require --time HH:mm");
        if (task.scheduleType == "daily") task.scheduleDays.clear();
        else {
            const auto maximum = task.scheduleType == "weekly" ? 7 : 31;
            for (const auto& day : task.scheduleDays.split(',')) {
                bool ok = false; const auto number = day.toInt(&ok);
                if (!ok || number < 1 || number > maximum) return failure(ExitCode::Usage,"Invalid scheduled days");
            }
            const auto days = ScheduledPlaybackSchedule::parseDays(task.scheduleDays,1,maximum);
            if (days.isEmpty() || ((task.scheduleType == "weekly" || task.scheduleType == "monthly") && days.size() != 1)) return failure(ExitCode::Usage,"This schedule requires one day");
            task.scheduleDays = ScheduledPlaybackSchedule::serializeDays(days,1,maximum);
        }
    }
    bool durationOk = false; task.durationMinutes = invocation.option("duration",QString::number(task.durationMinutes)).toInt(&durationOk);
    if (!durationOk || task.durationMinutes < 5 || task.durationMinutes > 720) return failure(ExitCode::Usage,"Duration must be 5..720 minutes");
    if (invocation.has("enabled")) {
        if (invocation.option("enabled") != "true" && invocation.option("enabled") != "false") return failure(ExitCode::Usage,"Enabled must be true or false");
        task.enabled = invocation.option("enabled") == "true";
    }
    if (previousService != task.serverId || oldSchedule != task.scheduleType + task.startTime + task.scheduleDays) task.lastRunDate.clear();
    for (const auto& existing : *tasks) if (existing.id != task.id && existing.serverId == task.serverId &&
        existing.scheduleType == task.scheduleType && existing.startTime == task.startTime && existing.scheduleDays == task.scheduleDays)
        return failure(ExitCode::Conflict,"An equivalent scheduled playback task already exists");
    const auto result = repository.saveScheduledPlaybackTask(task);
    return result ? success(scheduleJson(task)) : failure(ExitCode::Failed,result.error());
}
