#include "cli/CliEngine.h"
#include "cli/CliRuntime.h"
#include "cli/CliIpc.h"
#include "services/management/WorkflowSettings.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

Cli::Result Cli::Engine::tasks(const Invocation& invocation, SessionRepository& repository)
{
    QJsonObject request {{"action",invocation.command},{"id",invocation.arguments.value(0)},
                         {"pin",m_private ? m_pin : QString()}};
    if (invocation.command == "task remove" && !confirm(invocation,"Delete this task record, saved credentials and partial staging files?"))
        return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Usage,"Removal requires --yes or confirmation");
    if (!invocation.has("background")) return rpc("tasks",m_profile,request,true);
    static const QStringList allowed {"webdav upload","webdav download","smb upload","smb download","video pack","video restore","tssl backup","tssl restore"};
    if (!allowed.contains(invocation.command)) return failure(ExitCode::Usage,"This command cannot run in the background");
    auto job = invocation;
    job.flags.remove("background"); job.flags.remove("json"); job.options.remove("pin-env"); job.options.remove("data-dir");
    // Only actually private destinations retain the private-access request.
    job.flags.remove("private");
    QJsonObject secrets;
    for (const auto& option : {"password-env","s3-secret-env","s3-session-token-env"})
        if (job.has(option)) {
            const auto name = job.option(option);
            if (!qEnvironmentVariableIsSet(name.toUtf8().constData())) return failure(ExitCode::Authentication,"A task credential variable is not set");
            secrets[name] = qEnvironmentVariable(name.toUtf8().constData());
        }
    if (job.option("s3-credentials") == "env")
        for (const auto& name : {"AWS_ACCESS_KEY_ID","AWS_SECRET_ACCESS_KEY","AWS_SESSION_TOKEN"})
            if (qEnvironmentVariableIsSet(name)) secrets[name] = qEnvironmentVariable(name);
    const auto settings = WorkflowSettings::snapshot();
    QString service;
    if (job.command.startsWith("webdav ") || job.command.startsWith("smb ")) service = job.arguments[0];
    else if (job.command == "video restore" && job.has("service")) service = job.option("service");
    else if (job.command == "video pack" && (job.has("webdav") || (!job.has("output") && settings.value("m3u8s/outputMode") == "webdav")))
        service = job.option("webdav",settings.value("m3u8s/webDavServiceId").toString());
    else if (job.command.startsWith("tssl ") && job.option("target",settings.value("tsslBackup/target").toString()) == "webdav")
        service = job.option("service",settings.value("tsslBackup/webDavServiceId").toString());
    bool privateJob = false;
    if (!service.isEmpty()) {
        auto card = resolve(repository,service,!job.command.startsWith("smb "));
        if (!card) return failure(card.error());
        privateJob = card->server.privateMode;
        auto credential = password(job,card->server);
        if (!credential) return failure(credential.error());
        job.options["password-env"] = "VIBE_CLI_TASK_PASSWORD";
        secrets["VIBE_CLI_TASK_PASSWORD"] = *credential;
        if (job.command.startsWith("webdav ") || job.command.startsWith("smb ")) job.arguments[0] = card->server.id;
    }
    if (job.command == "webdav upload" || job.command == "smb upload") job.arguments[1] = QFileInfo(job.arguments[1]).absoluteFilePath();
    if (job.command == "webdav download" || job.command == "smb download") job.arguments[2] = QFileInfo(job.arguments[2]).absoluteFilePath();
    if (job.command == "video pack" || (job.command == "video restore" && !job.has("service")))
        for (auto& source : job.arguments) source = QFileInfo(source).absoluteFilePath();
    for (const auto& option : {"output","local-path","temp","ffmpeg"})
        if (job.has(option)) job.options[option] = QFileInfo(job.option(option)).absoluteFilePath();
    QJsonObject options;
    for (auto it = job.options.begin(); it != job.options.end(); ++it) options[it.key()] = it.value();
    request["action"] = "submit";
    request["private"] = privateJob;
    request["secrets"] = secrets;
    request["invocation"] = QJsonObject {{"command",job.command},{"arguments",QJsonArray::fromStringList(job.arguments)},
        {"options",options},{"flags",QJsonArray::fromStringList(job.flags.values())}};
    return rpc("tasks",m_profile,request,true);
}
