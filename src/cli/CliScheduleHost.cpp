#include "cli/CliIpc.h"
#include "cli/CliRuntime.h"
#include "services/scheduler/ScheduledPlaybackManager.h"
#include "services/management/ServiceOperations.h"
#include "utils/ErrorPresentation.h"
#include "utils/AppLogger.h"
#include <QLocalServer>
#include <QLockFile>
#include <QStandardPaths>
#include <QDir>
#include <QJsonArray>
#include <algorithm>
#include <utility>

namespace Cli {
int runScheduleHost(const QString& profile)
{
    const auto root = profile.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) : QDir(profile).absolutePath();
    if (!QDir().mkpath(root)) return 1;
    QLockFile lock(QDir(root).filePath("cli-schedule.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return 5;
    SessionRepository repository("cli-schedule",profile.isEmpty() ? QString() : QDir(profile).filePath("vibeplayer.sqlite3"));
    if (!repository.initialize()) return 1;
    NetworkClient network; EmbyClient emby(network);
    ScheduledPlaybackManager manager(emby,repository);
    QLocalServer server; server.setSocketOptions(QLocalServer::UserAccessOption);
    bool watching = false, privateAccess = false;
    QString pin;
    QHash<QString,std::pair<ServerConfig,qint64>> traffic;
    QObject::connect(&manager,&ScheduledPlaybackManager::networkTrafficSample,&server,[&](const auto& source,qint64 bytes) {
        if (bytes <= 0) return;
        auto& pending = traffic[source.id]; pending.first = source; pending.second += bytes;
    });
    auto flushTraffic = [&] {
        for (const auto& entry : std::as_const(traffic)) {
            const auto saved = repository.addDailyUsage(entry.first,entry.first.privateMode,0,0,0,entry.second,0);
            if (!saved) AppLogger::warning("scheduled-playback","Unable to persist CLI keep-alive traffic");
        }
        traffic.clear();
    };
    auto refresh = [&]() -> Result {
        repository.refreshSettings();
        if (privateAccess && !ServiceOperations::verifyPrivacyPin(repository,pin)) {
            manager.stop(); manager.setTasks({},false); privateAccess = false; pin.clear();
        }
        auto tasks = repository.loadScheduledPlaybackTasks(privateAccess);
        if (!tasks) return failure(ExitCode::Failed,"Unable to load scheduled playback tasks");
        manager.setTasks(watching ? *tasks : std::vector<ScheduledPlaybackTask> {},privateAccess);
        return success(true);
    };
    auto state = [&]() -> Result {
        return success(QJsonObject {{"watching",watching},{"state",manager.status()},{"active",manager.active()},
            {"waiting",manager.waiting()},{"serviceName",manager.currentServerName()},{"mediaName",manager.currentMediaName()},
            {"elapsedSeconds",manager.elapsedSeconds()},{"targetSeconds",manager.targetSeconds()},
            {"missedCount",manager.missedTaskCount()},{"missedServices",QJsonArray::fromStringList(manager.missedTaskServerNames())},
            {"error",ErrorPresentation::sanitizeDetails(manager.errorMessage())}});
    };
    auto enforceVisibility = [&] {
        const auto current = manager.currentServerId();
        if (current.isEmpty()) return true;
        const auto tasks = repository.loadScheduledPlaybackTasks(privateAccess);
        if (!tasks || std::ranges::none_of(*tasks,[&](const auto& task) { return task.id == manager.currentTaskId(); })) {
            manager.stop(); refresh(); return true;
        }
        const auto visible = repository.loadServiceCards(privateAccess);
        if (visible && std::ranges::any_of(*visible,[&](const auto& card) { return card.server.id == current; })) return true;
        manager.stop(); refresh(); return false;
    };
    QTimer timer; timer.setInterval(1000);
    int seconds = 0;
    QObject::connect(&timer,&QTimer::timeout,&server,[&] {
        repository.refreshSettings();
        if (privateAccess && !ServiceOperations::verifyPrivacyPin(repository,pin)) { manager.stop(); privateAccess = false; pin.clear(); refresh(); }
        enforceVisibility();
        ++seconds;
        if (watching && seconds % 10 == 0) refresh();
        if (seconds % 5 == 0) flushTraffic();
        if (watching || manager.active() || manager.waiting()) {
            const auto foreground = rpc("player",profile,QJsonObject {{"action","player status"},{"pin",pin}});
            manager.setForegroundPlaybackActive(foreground.code == ExitCode::Authentication ||
                (foreground.code == ExitCode::Success && !foreground.data.toObject().value("ended").toBool()));
        }
    });
    timer.start();
    serveRequests(server,[&](const QJsonObject& request) -> Result {
        repository.refreshSettings();
        const auto action = request.value("action").toString();
        const auto requestPin = request.value("pin").toString();
        const bool unlocked = ServiceOperations::verifyPrivacyPin(repository,requestPin);
        if ((privateAccess || request.value("private").toBool()) && !unlocked) return failure(ExitCode::Authentication,"Privacy PIN is required");
        if (!enforceVisibility()) return failure(ExitCode::Authentication,"Scheduled source is no longer accessible");
        if (action == "schedule status") return state();
        if (action == "schedule stop" || action == "schedule unwatch") {
            manager.stop();
            flushTraffic();
            if (action == "schedule unwatch") {
                watching = false; manager.setTasks({},false);
                QTimer::singleShot(100,&server,&QCoreApplication::quit);
            }
            return state();
        }
        if (action == "schedule resolve") {
            const auto run = request.value("run").toString();
            if (run != "true" && run != "false") return failure(ExitCode::Usage,"--run must be true or false");
            manager.resolveMissedTasks(run == "true"); return state();
        }
        if (action != "schedule watch" && action != "schedule run") return failure(ExitCode::Usage,"Unknown schedule command");
        if (privateAccess != request.value("private").toBool()) manager.stop();
        privateAccess = request.value("private").toBool(); pin = privateAccess ? requestPin : QString();
        if (action == "schedule watch") { watching = true; const auto result = refresh(); return result.code == ExitCode::Success ? state() : result; }
        if (manager.active() || manager.waiting()) return failure(ExitCode::Conflict,"Another scheduled task is active");
        const auto tasks = repository.loadScheduledPlaybackTasks(privateAccess);
        if (!tasks) return failure(ExitCode::Failed,"Unable to load scheduled tasks");
        const auto task = std::ranges::find(*tasks,request.value("id").toString(),&ScheduledPlaybackTask::id);
        if (task == tasks->end()) return failure(ExitCode::NotFound,"Scheduled task not found");
        manager.runNow(*task);
        return manager.status() == "error" ? failure(ExitCode::Failed,ErrorPresentation::sanitizeDetails(manager.errorMessage())) : state();
    });
    if (!server.listen(socketName("schedule",profile))) return 5;
    // This entry runs before PlaybackMain enters the application's main loop.
    // QCoreApplication::quit must target that loop, not a standalone nested loop.
    const auto result = QCoreApplication::exec();
    manager.stop(); flushTraffic(); return result;
}
}
