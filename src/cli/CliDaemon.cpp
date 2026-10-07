#include "cli/CliDaemon.h"
#include "cli/CliEngine.h"
#include "cli/CliIpc.h"
#include "cli/CliRuntime.h"
#include "services/credentials/CredentialStore.h"
#include "services/management/ServiceOperations.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QLockFile>

int Cli::runDaemon(const QString& profile)
{
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    const auto root = profile.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) : QDir(profile).absolutePath();
    if (!QDir().mkpath(root)) return 1;
    QLockFile hostLock(QDir(root).filePath("cli-tasks.lock"));
    hostLock.setStaleLockTime(0);
    if (!hostLock.tryLock(0)) return 5;
    auto database = QSqlDatabase::addDatabase("QSQLITE","cli-tasks");
    database.setDatabaseName(QDir(root).filePath("cli_tasks.sqlite3"));
    database.setConnectOptions("QSQLITE_BUSY_TIMEOUT=3000");
    if (!database.open()) return 1;
    QSqlQuery schema(database);
    if (!schema.exec("CREATE TABLE IF NOT EXISTS tasks (id TEXT PRIMARY KEY, body TEXT NOT NULL)")) return 1;
    QMap<QString,QJsonObject> jobs;
    QMap<QString,QString> pins;
    QString activeId;
    std::unique_ptr<Engine> engine;
    bool stopping = false;
    auto save = [&](const QString& id) {
        QSqlQuery query(database);
        query.prepare("INSERT OR REPLACE INTO tasks (id,body) VALUES (?,?)");
        query.addBindValue(id); query.addBindValue(QString::fromUtf8(QJsonDocument(jobs[id]).toJson(QJsonDocument::Compact)));
        return query.exec();
    };
    QSqlQuery query(database);
    query.exec("SELECT id,body FROM tasks");
    while (query.next()) {
        const auto id = query.value(0).toString();
        jobs[id] = QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object();
        if (QStringList {"running","paused","queued"}.contains(jobs[id].value("state").toString())) {
            jobs[id]["state"] = "interrupted"; save(id);
        }
    }
    SessionRepository repository("cli-task-privacy",QDir(root).filePath("vibeplayer.sqlite3"),SessionRepository::OpenMode::ReadOnly);
    auto authorized = [&](const QJsonObject& request) {
        repository.refreshSettings();
        return ServiceOperations::verifyPrivacyPin(repository,request.value("pin").toString());
    };
    auto publicJob = [&](const QString& id) {
        auto object = jobs[id];
        object.remove("invocation"); object.remove("credentialKey");
        return object;
    };
    std::function<void()> next;
    next = [&] {
        if (!activeId.isEmpty() || stopping) return;
        QString selected;
        for (auto it = jobs.begin(); it != jobs.end(); ++it)
            if (it.value().value("state") == "queued") { selected = it.key(); break; }
        if (selected.isEmpty()) return;
        activeId = selected;
        auto& job = jobs[selected];
        job["state"] = "running"; save(selected);
        const auto stored = job.value("invocation").toObject();
        Invocation invocation;
        invocation.command = stored.value("command").toString();
        for (const auto& arg : stored.value("arguments").toArray()) invocation.arguments += arg.toString();
        const auto options = stored.value("options").toObject();
        for (auto it = options.begin(); it != options.end(); ++it) invocation.options[it.key()] = it.value().toString();
        for (const auto& flag : stored.value("flags").toArray()) invocation.flags.insert(flag.toString());
        QMap<QByteArray,std::optional<QByteArray>> previous;
        auto setEnv = [&](const QString& name, const QString& value) {
            const auto key = name.toUtf8();
            if (!previous.contains(key)) previous[key] = qEnvironmentVariableIsSet(key.constData())
                ? std::optional<QByteArray>(qgetenv(key.constData())) : std::nullopt;
            qputenv(key.constData(),value.toUtf8());
        };
        if (CredentialStore::isAvailable()) {
            auto saved = CredentialStore::loadSecret("CliTask/" + selected);
            if (saved && *saved) {
                const auto secrets = QJsonDocument::fromJson((**saved).toUtf8()).object();
                for (auto it = secrets.begin(); it != secrets.end(); ++it) setEnv(it.key(),it.value().toString());
            }
        }
        if (job.value("private").toBool()) {
            setEnv("VIBE_CLI_TASK_PIN",pins.value(selected));
            invocation.flags.insert("private"); invocation.options["pin-env"] = "VIBE_CLI_TASK_PIN";
        }
        engine = std::make_unique<Engine>(profile);
        engine->setCheckpointDirectory(QDir(root).filePath("cli-task-checkpoints/" + selected));
        engine->setProgressObserver([&,selected](qint64 done,qint64 total) {
            jobs[selected]["bytesDone"] = static_cast<double>(done);
            jobs[selected]["bytesTotal"] = static_cast<double>(total);
        });
        const auto result = engine->execute(invocation);
        engine.reset();
        CliConsole::canceled.store(false);
        for (auto it = previous.begin(); it != previous.end(); ++it)
            if (it.value()) qputenv(it.key().constData(),*it.value()); else qunsetenv(it.key().constData());
        job["state"] = stopping ? "interrupted" : result.code == ExitCode::Success ? "completed" :
            result.code == ExitCode::Canceled ? "canceled" : "failed";
        job["result"] = resultObject(result);
        job["finishedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (result.code == ExitCode::Success && CredentialStore::isAvailable())
            static_cast<void>(CredentialStore::deleteSecret("CliTask/" + selected));
        save(selected); activeId.clear(); pins.remove(selected);
        if (stopping) QCoreApplication::quit();
        else QTimer::singleShot(0,&server,next);
    };
    auto handle = [&](const QJsonObject& request) -> Result {
        const auto action = request.value("action").toString();
        if (action == "submit") {
            const bool privateJob = request.value("private").toBool();
            if (privateJob && !authorized(request)) return failure(ExitCode::Authentication,"Privacy PIN is required");
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto secrets = request.value("secrets").toObject();
            if (!secrets.isEmpty()) {
                if (!CredentialStore::isAvailable()) return failure(ExitCode::Authentication,"Persistent tasks need a secure credential backend");
                auto saved = CredentialStore::saveSecret("CliTask/" + id,QString::fromUtf8(QJsonDocument(secrets).toJson(QJsonDocument::Compact)));
                if (!saved) return failure(ExitCode::Authentication,"Unable to securely save task credentials");
            }
            jobs[id] = QJsonObject {{"id",id},{"command",request.value("invocation").toObject().value("command")},
                {"state","queued"},{"private",privateJob},{"createdAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                {"invocation",request.value("invocation")}};
            if (privateJob) pins[id] = request.value("pin").toString();
            if (!save(id)) {
                jobs.remove(id); pins.remove(id);
                if (CredentialStore::isAvailable()) static_cast<void>(CredentialStore::deleteSecret("CliTask/" + id));
                return failure(ExitCode::Failed,"Unable to save task");
            }
            QTimer::singleShot(0,&server,next);
            return success(publicJob(id));
        }
        if (action == "task list") {
            QJsonArray rows;
            const bool privateAccess = authorized(request);
            for (auto it = jobs.begin(); it != jobs.end(); ++it)
                if (!it.value().value("private").toBool() || privateAccess) rows += publicJob(it.key());
            return success(rows);
        }
        if (action == "daemon stop") {
            if (!activeId.isEmpty() && jobs[activeId].value("private").toBool() && !authorized(request))
                return failure(ExitCode::Authentication,"Privacy PIN is required to stop this worker");
            stopping = true;
            if (engine) engine->transferControl("cancel");
            else QTimer::singleShot(100,&server,&QCoreApplication::quit);
            return success(QStringLiteral("Worker shutdown requested; tasks can be resumed after restart"));
        }
        const auto id = request.value("id").toString();
        if (!jobs.contains(id) || (jobs[id].value("private").toBool() && !authorized(request)))
            return failure(ExitCode::NotFound,"Task not found in the accessible tasks");
        auto& job = jobs[id];
        if (action == "task show") return success(publicJob(id));
        if (action == "task remove") {
            if (id == activeId || job.value("state") == "queued") return failure(ExitCode::Conflict,"Cancel or finish the task before removing it");
            const auto stagingRoot = QDir(root).filePath("cli-task-checkpoints");
            const auto staging = QDir(stagingRoot).absoluteFilePath(id);
            if (QUuid(id).toString(QUuid::WithoutBraces) != id ||
                !staging.startsWith(QDir(stagingRoot).absolutePath() + '/'))
                return failure(ExitCode::Failed,"Invalid task checkpoint location");
            if (QDir(staging).exists() && !QDir(staging).removeRecursively())
                return failure(ExitCode::Failed,"Unable to remove task checkpoints");
            if (CredentialStore::isAvailable()) {
                auto removed = CredentialStore::deleteSecret("CliTask/" + id);
                if (!removed) return failure(ExitCode::Failed,"Unable to remove saved task credentials");
            }
            QSqlQuery query(database); query.prepare("DELETE FROM tasks WHERE id = ?"); query.addBindValue(id);
            if (!query.exec()) return failure(ExitCode::Failed,"Unable to remove task record");
            jobs.remove(id); pins.remove(id);
            return success(QJsonObject {{"removed",id}});
        }
        if (action == "task cancel") {
            if (QStringList {"completed","failed","canceled"}.contains(job.value("state").toString()))
                return failure(ExitCode::Conflict,"This task has already finished");
            if (id == activeId && engine) engine->transferControl("cancel");
            else job["state"] = "canceled";
        } else if (action == "task pause") {
            if (id == activeId && engine) {
                auto result = engine->transferControl("pause");
                if (result.code != ExitCode::Success) return result;
            } else if (job.value("state") != "queued") return failure(ExitCode::Conflict,"Only queued or running tasks can be paused");
            job["state"] = "paused";
        } else if (action == "task resume" || action == "task retry") {
            if (id == activeId && engine) {
                auto result = engine->transferControl("resume");
                if (result.code != ExitCode::Success) return result;
                job["state"] = "running";
            } else {
                if (job.value("state") == "completed" || job.value("state") == "running")
                    return failure(ExitCode::Conflict,"The task is completed or already running");
                job["state"] = "queued";
                if (job.value("private").toBool()) pins[id] = request.value("pin").toString();
                QTimer::singleShot(0,&server,next);
            }
        } else return failure(ExitCode::Usage,"Unknown task command");
        if (!save(id)) return failure(ExitCode::Failed,"Unable to save task state");
        return success(publicJob(id));
    };
    serveRequests(server,handle);
    // Publish the endpoint only after state and request handlers are ready.
    if (!server.listen(socketName("tasks",profile))) return 5;
    QTimer::singleShot(0,&server,next);
    QTimer interruptTimer;
    QObject::connect(&interruptTimer,&QTimer::timeout,&server,[&] {
        if (engine || !CliConsole::pollCancellation() || stopping) return;
        stopping = true;
        if (engine) engine->transferControl("cancel");
        else QCoreApplication::quit();
    });
    interruptTimer.start(50);
    QEventLoop loop;
    QObject::connect(QCoreApplication::instance(),&QCoreApplication::aboutToQuit,&loop,&QEventLoop::quit);
    loop.exec();
    database.close();
    return 0;
}
