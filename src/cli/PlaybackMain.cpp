#include "cli/CliIpc.h"
#include "cli/CliRuntime.h"
#include "cli/CliIptvPlayback.h"
#include "player/PlayerController.h"
#include "database/SessionRepository.h"
#include "services/management/ServiceOperations.h"
#include "services/management/WorkflowSettings.h"
#include "services/emby/EmbyClient.h"
#include "services/jellyfin/JellyfinClient.h"
#include "services/webdav/EncryptedHlsPlaybackProxy.h"
#include "services/webdav/WebDavPlaybackProxy.h"
#include "utils/ErrorPresentation.h"
#include "utils/AppLogger.h"
#include <QGuiApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QPointer>
#include <QWindow>
#include <QLockFile>
#include <QStandardPaths>
#include <QUuid>
#include <QElapsedTimer>

namespace Cli { int runScheduleHost(const QString& profile); }

int main(int argc, char* argv[])
{
    QGuiApplication app(argc,argv);
    app.setOrganizationName("vibePlayerQT"); app.setApplicationName("vibePlayerQT");
    AppLogger::setSanitizer([](const QString& message) { return ErrorPresentation::sanitizeDetails(message); });
    const auto args = app.arguments();
    const auto profile = args.contains("--data-dir") ? args.value(args.indexOf("--data-dir") + 1) : QString();
    if (!profile.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,QDir(profile).absolutePath());
    }
    if (args.contains("--schedule-host")) return Cli::runScheduleHost(profile);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    const auto root = profile.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) : QDir(profile).absolutePath();
    if (!QDir().mkpath(root)) return 1;
    QLockFile hostLock(QDir(root).filePath("cli-player.lock"));
    hostLock.setStaleLockTime(0);
    if (!hostLock.tryLock(0)) return 5;
    const auto databasePath = profile.isEmpty() ? QString() : QDir(profile).filePath("vibeplayer.sqlite3");
    SessionRepository repository("cli-player",databasePath);
    if (!repository.initialize()) return 1;
    QWindow window; window.resize(1100,650); window.setTitle("vibePlayerQT");
    PlayerController player;
    TsslStore store(profile.isEmpty() ? QString() : QDir(profile).filePath("tssl"));
    EncryptedHlsPlaybackProxy proxy(store);
    WebDavPlaybackProxy webdavProxy;
    NetworkClient network; EmbyClient emby(network); JellyfinClient jellyfin(network);
    std::optional<UserSession> session;
    PlaybackReport report;
    bool privatePlayback = false, preparing = false, preparationCanceled = false;
    bool reportStarted = false, ended = false, failed = false;
    QString encryptedSession, webdavSession, lastError, title;
    Cli::IptvPlayback routes(repository,databasePath);
    QString historyId;
    QString currentServiceId;
    bool historyCompleted = false;
    QJsonObject historyRequest;
    qint64 networkBytes = 0;
    QElapsedTimer usageElapsed; usageElapsed.start();
    auto saveProgress = [&](bool completed = false) {
        if (historyId.isEmpty()) return;
        historyCompleted |= completed;
        const auto saved = repository.updatePlaybackHistoryProgress(historyId,
            static_cast<qint64>(player.position()),static_cast<qint64>(player.duration()),historyCompleted,QDateTime::currentDateTimeUtc());
        if (!saved) AppLogger::warning("global-history","Unable to update CLI playback progress");
        if (historyRequest.value("historySource").toString() == "Local") {
            usageElapsed.restart(); networkBytes = 0; return;
        }
        ServerConfig source;
        source.id = historyRequest.value("serviceId").toString("builtin-link-playback");
        source.name = historyRequest.value("serviceName").toString(historyRequest.value("historySource").toString());
        source.serviceType = serviceTypeFromString(historyRequest.value("historySource").toString());
        const auto seconds = usageElapsed.restart() / 1000;
        const auto usage = repository.addDailyUsage(source,privatePlayback,player.paused() ? 0 : seconds,networkBytes,0,0,0);
        if (!usage) AppLogger::warning("global-history","Unable to update CLI playback statistics");
        networkBytes = 0;
    };
    auto client = [&]() -> MediaServiceClient* {
        return !session ? nullptr : session->server.serviceType == ServiceType::Emby
            ? static_cast<MediaServiceClient*>(&emby) : static_cast<MediaServiceClient*>(&jellyfin);
    };
    auto stopped = [&] {
        saveProgress();
        // Stop/reset and final application cleanup must not replace the last
        // real position with PlayerController's subsequent zeroed state.
        historyId.clear();
        if (auto* media = client(); media && reportStarted) {
            report.positionTicks = static_cast<qint64>(player.position() * 10000000);
            media->reportPlaybackStopped(*session,report);
        }
        reportStarted = false;
        if (!encryptedSession.isEmpty()) { proxy.revoke(encryptedSession); encryptedSession.clear(); }
        if (!webdavSession.isEmpty()) { webdavProxy.revoke(webdavSession); webdavSession.clear(); }
    };
    QObject::connect(&player,&PlayerController::errorOccurred,&app,[&](const QString& message) {
        lastError = ErrorPresentation::sanitizeDetails(message);
    });
    QObject::connect(&player,&PlayerController::playbackRestarted,&app,[&] {
        routes.started();
        if (historyId.isEmpty()) {
            PlaybackHistoryItem item;
            item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            item.source = playbackHistorySourceFromString(historyRequest.value("historySource").toString());
            item.serviceId = historyRequest.value("serviceId").toString();
            item.serviceName = historyRequest.value("serviceName").toString();
            item.replayTarget = historyRequest.value("historyTarget").toString(); item.title = title;
            item.privacyMode = privatePlayback; item.playedAt = item.updatedAt = QDateTime::currentDateTimeUtc();
            item.playedDate = item.playedAt.toLocalTime().date();
            item.positionSeconds = static_cast<qint64>(player.position()); item.durationSeconds = static_cast<qint64>(player.duration());
            if (item.source != PlaybackHistorySource::Unknown && !item.replayTarget.isEmpty()) {
                const auto saved = repository.savePlaybackHistory(item);
                if (saved) historyId = item.id;
                else AppLogger::warning("global-history","Unable to save CLI playback history");
            }
            usageElapsed.restart(); networkBytes = 0;
        }
        if (auto* media = client(); media && !reportStarted) {
            report.positionTicks = static_cast<qint64>(player.position() * 10000000);
            media->reportPlaybackStart(*session,report); reportStarted = true;
        }
    });
    QObject::connect(&player,&PlayerController::videoInfoChanged,&app,[&] {
        routes.update(player.videoResolution(),player.videoCodec(),player.videoBitrate());
    });
    QObject::connect(&player,&PlayerController::playbackNetworkBytes,&app,[&](qint64 bytes) { networkBytes += std::max<qint64>(0,bytes); });
    QObject::connect(&player,&PlayerController::playbackEnded,&app,[&](double,bool reachedEnd,bool failure) {
        if (routes.retry(failure)) { lastError.clear(); return; }
        ended = true; failed = failure; saveProgress(reachedEnd && !failure); stopped();
    });
    routes.timeoutExhausted = [&] { routes.stop(); stopped(); player.stop(); ended = failed = true; lastError = "All IPTV streams timed out"; };
    QTimer progress;
    QObject::connect(&progress,&QTimer::timeout,&app,[&] {
        saveProgress();
        if (auto* media = client(); media && reportStarted) {
            report.positionTicks = static_cast<qint64>(player.position() * 10000000); report.paused = player.paused();
            media->reportPlaybackProgress(*session,report);
        }
    });
    progress.start(10000);
    auto state = [&] {
        auto tracks = [](TrackListModel* model) {
            QJsonArray rows;
            for (int i = 0; i < model->count(); ++i)
                if (const auto* track = model->trackAt(i)) rows += QJsonObject {
                    {"row",i},{"id",track->id},{"title",track->title},{"language",track->language},{"selected",track->selected}};
            return rows;
        };
        return QJsonObject {{"title",title},{"paused",player.paused()},{"loading",player.loading()},
            {"ended",ended},{"failed",failed},
            {"position",player.position()},{"duration",player.duration()},{"volume",player.volume()},
            {"speed",player.speed()},{"error",lastError},{"audio",tracks(player.audioTracks())},
            {"subtitles",tracks(player.subtitleTracks())},{"resolution",player.videoResolution()},
            {"codec",player.videoCodec()},{"bitrate",player.videoBitrate()},{"iptv",routes.active()}};
    };
    auto handle = [&](const QJsonObject& request) -> Cli::Result {
        repository.refreshSettings();
        bool sourcePrivate = request.value("private").toBool();
        const auto serviceId = request.value("serviceId").toString(request.value("action").toString() == "play" ? QString() : currentServiceId);
        if (!serviceId.isEmpty()) {
            auto cards = repository.loadServiceCards(true);
            if (!cards) return Cli::failure(Cli::ExitCode::Failed,"Unable to read playback service");
            bool found = false;
            for (const auto& card : *cards) if (card.server.id == serviceId) { sourcePrivate |= card.server.privateMode; found = true; break; }
            if (!found && request.value("action").toString() != "player stop") return Cli::failure(Cli::ExitCode::NotFound,"Playback service no longer exists");
        }
        if ((privatePlayback || sourcePrivate) &&
            !ServiceOperations::verifyPrivacyPin(repository,request.value("pin").toString()))
            return Cli::failure(Cli::ExitCode::Authentication,"Privacy PIN is required for this playback session");
        const auto action = request.value("action").toString();
        const auto arguments = request.value("arguments").toArray();
        const auto argument = arguments.isEmpty() ? QString() : arguments[0].toString();
        if (preparing) {
            if (action == "player stop") {
                preparationCanceled = true;
                proxy.cancelNetworkRequests();
                return Cli::success(QStringLiteral("Playback preparation canceled"));
            }
            return Cli::failure(Cli::ExitCode::Conflict,"A playback source is being prepared");
        }
        if (action == "play") {
            preparing = true;
            preparationCanceled = false;
            struct Reset { bool& value; ~Reset() { value = false; } } reset {preparing};
            routes.stop(); stopped(); historyId.clear(); historyCompleted = false; historyRequest = request;
            player.stop(); player.shutdown();
            privatePlayback = sourcePrivate; session.reset();
            currentServiceId = serviceId;
            title = request.value("title").toString("vibePlayerQT"); lastError.clear();
            ended = false; failed = false;
            if (!request.value("itemId").toString().isEmpty()) {
                auto saved = repository.loadSession(request.value("serviceId").toString());
                if (!saved || !*saved) return Cli::failure(Cli::ExitCode::Authentication,"Saved playback session is unavailable");
                session = **saved;
                report = {request.value("itemId").toString(),request.value("mediaSourceId").toString(),
                          request.value("playSessionId").toString()};
            }
            const bool headless = request.value("headless").toBool();
            player.setLiveStream(!request.value("channelId").toString().isEmpty());
            window.setVisible(!headless); window.setTitle(title);
            if (!(headless ? player.initializeHeadless() : player.initialize(window.winId())))
                return Cli::failure(Cli::ExitCode::Failed,"Unable to initialize libmpv");
            auto url = QUrl(request.value("url").toString());
            const bool encrypted = url.path().endsWith(".m3u8s",Qt::CaseInsensitive) ||
                                   url.path().endsWith(".m3u8sp",Qt::CaseInsensitive);
            if (encrypted) {
                auto prepared = Cli::await<EncryptedHlsPrepareResult>([&](auto done) {
                    if (url.isLocalFile()) proxy.prepareLocalStream(url.toLocalFile(),done);
                    else {
                        ServerConfig source; source.id = request.value("serviceId").toString(); source.name = title;
                        source.serviceType = ServiceType::WebDAV; source.baseUrl = url.adjusted(QUrl::RemoveFilename).toString();
                        source.username = request.value("username").toString();
                        source.trustSelfSignedCertificate = request.value("insecure").toBool();
                        proxy.prepareStream(source,request.value("password").toString(),url,done);
                    }
                },[] {});
                if (preparationCanceled) {
                    if (prepared) proxy.revoke(prepared->sessionId);
                    player.shutdown(); window.hide();
                    QTimer::singleShot(100,&app,&QCoreApplication::quit);
                    return Cli::failure(Cli::ExitCode::Canceled,"Playback preparation canceled");
                }
                if (!prepared) return Cli::failure(Cli::ExitCode::Failed,prepared.error());
                encryptedSession = prepared->sessionId; url = prepared->url;
            } else if (request.value("webdav").toBool() ||
                       (session && session->server.trustSelfSignedCertificate && url.scheme() == QStringLiteral("https"))) {
                ServerConfig source; source.id = request.value("serviceId").toString(); source.name = title;
                source.serviceType = ServiceType::WebDAV; source.baseUrl = url.adjusted(QUrl::RemoveFilename).toString();
                source.username = request.value("webdav").toBool() ? request.value("username").toString() : QString();
                source.trustSelfSignedCertificate = session ? session->server.trustSelfSignedCertificate
                                                          : request.value("insecure").toBool();
                const auto local = webdavProxy.streamUrlFor(source,request.value("password").toString(),url);
                if (local == url) return Cli::failure(Cli::ExitCode::Failed,"Unable to start the WebDAV playback proxy");
                webdavSession = local.path().section('/',1,1); url = local;
            }
            const auto settings = WorkflowSettings::snapshot();
            player.setVolume(settings.value("player/volume").toInt()); player.setSpeed(settings.value("player/speed").toDouble());
            if (!request.value("channelId").toString().isEmpty()) {
                const auto started = routes.start(serviceId,request.value("channelId").toString(),
                    request.value("preference").toString("quality"),request.value("streamId").toString(),
                    [&](const QString& source) { ended = failed = false; lastError.clear(); player.playUrl(source); });
                if (!started) return Cli::failure(Cli::ExitCode::NotFound,started.error());
            } else player.playUrl(url.toString(),request.value("start").toDouble(),{}, {},false,
                                  request.value("subtitleStreamIndex").toInt(-1),encrypted);
            const auto subtitle = settings.value("player/subtitleFile").toString();
            if (!subtitle.isEmpty()) player.loadExternalSubtitle(subtitle);
            return Cli::success(state());
        }
        if (action == "player streams") {
            if (!routes.active()) return Cli::failure(Cli::ExitCode::Usage,"Current playback is not IPTV");
            return Cli::success(routes.streams(request.value("probe").toBool(),request.value("refresh").toBool()));
        }
        if (action == "player stream") {
            if (!routes.switchStream(argument)) return Cli::failure(Cli::ExitCode::NotFound,"Playback stream not found");
        } else if (action == "player pause") player.pause();
        else if (action == "player resume") player.resume();
        else if (action == "player stop") {
            routes.stop(); stopped(); player.stop(); window.hide(); QTimer::singleShot(300,&app,&QCoreApplication::quit);
        } else if (action == "player seek") player.seekAbsolute(argument.toDouble());
        else if (action == "player volume") player.setVolume(argument.toInt());
        else if (action == "player speed") player.setSpeed(argument.toDouble());
        else if (action == "player subtitle") {
            bool row = false; const int index = argument.toInt(&row);
            if (row) {
                if (index < -1 || index >= player.subtitleTracks()->count()) return Cli::failure(Cli::ExitCode::Usage,"Subtitle track row not found");
                player.selectSubtitleTrack(index);
            } else if (QFileInfo(argument).isFile()) player.loadExternalSubtitle(QFileInfo(argument).absoluteFilePath());
            else return Cli::failure(Cli::ExitCode::NotFound,"Subtitle file not found");
        } else if (action == "player audio") {
            bool ok = false; const int row = argument.toInt(&ok);
            if (!ok || row < 0 || row >= player.audioTracks()->count()) return Cli::failure(Cli::ExitCode::Usage,"Audio track row not found");
            player.selectAudioTrack(row);
        } else if (action != "player status") return Cli::failure(Cli::ExitCode::Usage,"Unknown playback control");
        return Cli::success(state());
    };
    Cli::serveRequests(server,handle);
    if (!server.listen(Cli::socketName("player",profile))) return 5;
    QObject::connect(&window,&QWindow::visibleChanged,&app,[&](bool visible) {
        if (!visible && !preparing && !ended) { routes.stop(); stopped(); player.stop(); }
    });
    const auto result = app.exec(); stopped(); player.shutdown(); return result;
}
