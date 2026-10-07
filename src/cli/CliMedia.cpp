#include "cli/CliEngine.h"
#include "cli/CliRuntime.h"
#include "cli/CliIpc.h"
#include "services/emby/EmbyClient.h"
#include "services/jellyfin/JellyfinClient.h"
#include "services/smb/SmbClient.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <QtConcurrentRun>
#include <cmath>

namespace {
QJsonObject itemJson(const MediaItem& item)
{
    return {{"id",item.id},{"name",item.name},{"type",item.itemType},{"directory",item.folder},
        {"parentId",item.parentId},{"duration",item.runTimeTicks / 10000000.0},
        {"resumeAt",item.playbackPositionTicks / 10000000.0},{"overview",item.overview}};
}
}
Cli::Result Cli::Engine::startPlayback(const Invocation& invocation, const QJsonObject& request)
{
    QJsonObject status {{"action","player status"},{"pin",request.value("pin")}};
    auto cancelPlayback = [&]() {
        status["action"] = "player stop";
        // Forward the stop even when cancellation interrupted a status RPC.
        CliConsole::canceled.store(false);
        static_cast<void>(rpc("player",m_profile,status));
        CliConsole::canceled.store(true);
        return failure(ExitCode::Canceled,"Canceled");
    };
    auto result = rpc("player",m_profile,request,true);
    if (result.code == ExitCode::Canceled) return cancelPlayback();
    if (result.code != ExitCode::Success || !invocation.has("wait")) return result;
    while (true) {
        if (CliConsole::pollCancellation()) return cancelPlayback();
        result = rpc("player",m_profile,status);
        if (result.code == ExitCode::Canceled) return cancelPlayback();
        if (result.code != ExitCode::Success) return result;
        const auto state = result.data.toObject();
        if (state.value("ended").toBool())
            return state.value("failed").toBool() ? failure(ExitCode::Failed,state.value("error").toString("Playback failed"),state)
                                                  : success(state);
        QThread::msleep(100);
    }
}
Cli::Result Cli::Engine::media(const Invocation& invocation, SessionRepository& repository)
{
    QJsonObject request {{"action",invocation.command},{"pin",m_private ? m_pin : QString()},
                         {"arguments",QJsonArray::fromStringList(invocation.arguments)}};
    if (invocation.command.startsWith("player ")) {
        if (QStringList {"player seek","player volume","player speed"}.contains(invocation.command)) {
            bool ok = false; const auto value = invocation.arguments[0].toDouble(&ok);
            if (!ok || !std::isfinite(value) || value < 0 ||
                (invocation.command == "player volume" && (value > 100 || std::floor(value) != value)) ||
                (invocation.command == "player speed" && (value < 0.5 || value > 5)))
                return failure(ExitCode::Usage,"Playback control value is outside its supported range");
        }
        return rpc("player",m_profile,request);
    }
    bool startOk = false;
    const auto start = invocation.option("start","0").toDouble(&startOk);
    if (!startOk || !std::isfinite(start) || start < 0) return failure(ExitCode::Usage,"--start must be a nonnegative number");
    request["start"] = start; request["headless"] = invocation.has("headless");
    request["private"] = false;
    if (invocation.command == "play" && invocation.arguments.size() == 1) {
        const auto source = invocation.arguments[0];
        QUrl url(source);
        if (QFileInfo(source).isFile()) url = QUrl::fromLocalFile(QFileInfo(source).absoluteFilePath());
        if (!url.isValid() || (!url.isLocalFile() && url.scheme() != "http" && url.scheme() != "https") ||
            !url.userInfo().isEmpty() || (url.isLocalFile() && !QFileInfo(url.toLocalFile()).isFile()))
            return failure(ExitCode::NotFound,"Expected an existing media file or an HTTP(S) URL without embedded credentials");
        request["url"] = url.toString();
        return startPlayback(invocation,request);
    }
    auto card = resolve(repository,invocation.arguments[0]);
    if (!card) return failure(card.error());
    const auto server = card->server;
    request["private"] = server.privateMode;
    request["serviceId"] = server.id;
    request["title"] = server.name;
    if (server.serviceType == ServiceType::IPTV) {
        auto channels = repository.loadIptvChannels(server.id);
        if (!channels) return failure(ExitCode::Failed,channels.error());
        QJsonArray rows;
        std::optional<IptvChannel> selected;
        const auto target = invocation.arguments.value(1);
        for (const auto& channel : *channels) {
            rows += QJsonObject {{"id",channel.id},{"name",channel.name},{"group",channel.groupName}};
            if (channel.id == target) { selected = channel; break; }
        }
        if (invocation.command == "media channels" || invocation.command == "media ls") return success(rows);
        if (invocation.command != "play") return failure(ExitCode::Usage,"IPTV supports media channels and play");
        if (!selected) for (const auto& channel : *channels) if (channel.name == target) {
            if (selected) return failure(ExitCode::Usage,"Channel name is ambiguous; specify its ID");
            selected = channel;
        }
        if (!selected) return failure(ExitCode::NotFound,"IPTV channel not found");
        request["url"] = selected->streamUrl; request["title"] = selected->name;
        return startPlayback(invocation,request);
    }
    if (server.serviceType == ServiceType::WebDAV || server.serviceType == ServiceType::SMB) {
        if (invocation.command != "play") return failure(ExitCode::Usage,"Use the webdav or smb file commands for this service");
        auto credential = password(invocation,server);
        if (!credential) return failure(credential.error());
        request["username"] = server.username; request["password"] = *credential;
        request["insecure"] = server.trustSelfSignedCertificate;
        if (server.serviceType == ServiceType::WebDAV) {
            request["webdav"] = true;
            auto url = remoteUrl(server,invocation.arguments[1]);
            if (!url) return failure(url.error());
            request["url"] = url->toString();
        } else {
            auto connected = awaitFuture(QtConcurrent::run([server,credential = *credential] { return SmbClient::connect(server,credential); }));
            if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
            if (!connected) return failure(ExitCode::Authentication,connected.error());
            auto path = SmbClient::path(server,invocation.arguments[1]);
            if (!path) return failure(ExitCode::Usage,path.error());
            request["url"] = QUrl::fromLocalFile(*path).toString();
        }
        return startPlayback(invocation,request);
    }
    if (server.serviceType != ServiceType::Emby && server.serviceType != ServiceType::Jellyfin)
        return failure(ExitCode::Usage,"Unsupported media service");
    const auto saved = repository.loadSession(server.id);
    if (!saved) return failure(ExitCode::Failed,saved.error());
    if (!*saved) return failure(ExitCode::Authentication,"No saved session; run service login first");
    const auto session = **saved;
    NetworkClient network;
    EmbyClient emby(network); JellyfinClient jellyfin(network);
    MediaServiceClient& client = server.serviceType == ServiceType::Emby
        ? static_cast<MediaServiceClient&>(emby) : static_cast<MediaServiceClient&>(jellyfin);
    if (invocation.command == "media libraries" || (invocation.command == "media ls" && invocation.arguments.size() == 1)) {
        auto libraries = await<LibraryResult>([&](auto done) { client.fetchLibraries(session,done); },[&] { network.cancelRequests(); });
        if (!libraries) return networkFailure(libraries.error());
        QJsonArray rows;
        for (const auto& library : *libraries) rows += QJsonObject {{"id",library.id},{"name",library.name},
            {"type",library.collectionType},{"children",library.childCount}};
        return success(rows);
    }
    if (invocation.command == "media ls" || invocation.command == "media search") {
        bool offsetOk = false, limitOk = false;
        const auto offset = invocation.option("offset","0").toInt(&offsetOk), limit = invocation.option("limit","100").toInt(&limitOk);
        if (!offsetOk || !limitOk || offset < 0 || limit < 1 || limit > 1000)
            return failure(ExitCode::Usage,"Offset must be nonnegative and limit must be 1..1000");
        auto items = await<ItemResult>([&](auto done) {
            if (invocation.command == "media search") client.searchVideoItems(session,invocation.arguments[1],offset,limit,done);
            else {
                MediaLibrary library; library.id = invocation.arguments[1];
                client.fetchLibraryItems(session,library,library.id,offset,limit,done);
            }
        },[&] { network.cancelRequests(); });
        if (!items) return networkFailure(items.error());
        QJsonArray rows; for (const auto& item : *items) rows += itemJson(item);
        return success(rows);
    }
    if (invocation.command == "media channels") return failure(ExitCode::Usage,"media channels requires IPTV");
    auto item = await<std::expected<MediaItem,NetworkError>>([&](auto done) {
        client.fetchItemDetails(session,invocation.arguments[1],done);
    },[&] { network.cancelRequests(); });
    if (!item) return networkFailure(item.error());
    if (invocation.command == "media show") return success(itemJson(*item));
    if (item->folder) return failure(ExitCode::Usage,"A folder cannot be played");
    auto playback = await<PlaybackUrlResult>([&](auto done) { client.fetchPlaybackUrl(session,*item,done); },[&] { network.cancelRequests(); });
    if (!playback) return networkFailure(playback.error());
    request["url"] = playback->url.toString();
    request["start"] = invocation.has("start") ? start : playback->startSeconds;
    request["title"] = item->name; request["itemId"] = item->id;
    request["mediaSourceId"] = playback->mediaSourceId; request["playSessionId"] = playback->playSessionId;
    request["subtitleStreamIndex"] = playback->subtitleStreamIndex;
    return startPlayback(invocation,request);
}
