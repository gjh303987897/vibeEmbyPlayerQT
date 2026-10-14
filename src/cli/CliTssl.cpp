#include "cli/CliEngine.h"
#include "cli/CliRuntime.h"
#include "services/backup/TsslBackupService.h"
#include "services/backup/TsslBackupArchive.h"
#include "services/backup/S3CredentialProvider.h"
#include "services/backup/TsslImportWorkflow.h"
#include "services/management/WorkflowSettings.h"
#include "services/credentials/CredentialStore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtConcurrentRun>
#include <algorithm>

namespace {
std::expected<QByteArray, Cli::Error> digest(const QString& text)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-fA-F]{64}$"));
    if (!expression.match(text).hasMatch())
        return std::unexpected(Cli::Error {Cli::ExitCode::Usage, QStringLiteral("Expected a complete 64-character root digest")});
    return QByteArray::fromHex(text.toLatin1());
}
Cli::Result importFiles(const TsslStore& store, const QStringList& files)
{
    const auto summary = Cli::awaitFuture(QtConcurrent::run([store, files] {
        return TsslImportWorkflow::run(store, files, &CliConsole::canceled, {}, [](const QString& stage, int done,int total) {
            CliConsole::progress(QStringLiteral("TSSL %1: %2 / %3").arg(stage).arg(done).arg(total));
        });
    }));
    const QJsonObject data {{"imported",summary.restored}, {"duplicates",summary.alreadyExists}, {"failed",summary.failed}};
    if (summary.canceled || CliConsole::pollCancellation()) return Cli::failure(Cli::ExitCode::Canceled, "Canceled", data);
    if (summary.failed) return Cli::failure(summary.restored || summary.alreadyExists ? Cli::ExitCode::Partial : Cli::ExitCode::Failed,
                                           summary.firstError, data);
    return Cli::success(data);
}
Cli::Result exportPackages(const TsslStore& store, const std::vector<QByteArray>& digests,
                          const QString& destination, const QString& countKey = "exported")
{
    return Cli::awaitFuture(QtConcurrent::run([store,digests,destination,countKey] {
        int exported = 0, failed = 0;
        QString firstError;
        for (const auto& digest : digests) {
            if (CliConsole::canceled.load()) break;
            const auto target = QDir(destination).filePath(QString::fromLatin1(digest.toHex()) + ".tssl");
            auto result = store.exportByRootDigest(digest,target);
            if (result) ++exported;
            else { ++failed; if (firstError.isEmpty()) firstError = result.error(); }
            CliConsole::progress(QStringLiteral("TSSL export: %1 / %2").arg(exported + failed).arg(digests.size()));
        }
        const QJsonObject data {{countKey,exported},{"failed",failed},{"target",destination}};
        if (CliConsole::canceled.load()) return Cli::failure(Cli::ExitCode::Canceled,"Canceled",data);
        return failed ? Cli::failure(exported ? Cli::ExitCode::Partial : Cli::ExitCode::Failed,firstError,data)
                      : Cli::success(data);
    }));
}
}

Cli::Result Cli::Engine::tssl(const Invocation& invocation, SessionRepository& repository)
{
    TsslStore store(storagePath());
    if (invocation.command == "tssl list") {
        const auto result = awaitFuture(QtConcurrent::run([store] { return store.listPackageSummaries(); }));
        if (!result) return failure(ExitCode::Failed, result.error());
        QJsonArray data;
        for (const auto& entry : *result) data += QJsonObject {
            {"digest",QString::fromLatin1(entry.rootManifestDigest.toHex())}, {"file",entry.filePath},
            {"size",static_cast<double>(entry.fileSize)}, {"modifiedAt",entry.modifiedAt.toString(Qt::ISODate)},
        };
        return success(data);
    }
    if (invocation.command == "tssl import") return importFiles(store, invocation.arguments);
    if (invocation.command == "tssl export" && invocation.has("output")) {
        const auto destination = QFileInfo(invocation.option("output")).absoluteFilePath();
        if (!QDir().mkpath(destination)) return failure(ExitCode::Failed, "Unable to create export directory");
        const auto result = awaitFuture(QtConcurrent::run([store, invocation]() -> std::expected<std::vector<QByteArray>,QString> {
            std::vector<QByteArray> digests;
            if (!invocation.has("all")) {
                for (const auto& text : invocation.arguments) {
                    auto value = digest(text);
                    if (!value) return std::unexpected(value.error().message);
                    if (std::ranges::find(digests,*value) == digests.end()) digests.push_back(*value);
                }
                return digests;
            }
            const auto summaries = store.listPackageSummaries();
            if (!summaries) return std::unexpected(summaries.error());
            for (const auto& item : *summaries) digests.push_back(item.rootManifestDigest);
            return digests;
        }));
        return result ? exportPackages(store,*result,destination) : failure(invocation.has("all") ? ExitCode::Failed : ExitCode::Usage, result.error());
    }
    if (invocation.command == "tssl remove") {
        std::vector<QByteArray> selected;
        for (const auto& text : invocation.arguments) {
            auto value = digest(text);
            if (!value) return failure(value.error());
            if (std::ranges::find(selected,*value) == selected.end()) selected.push_back(*value);
        }
        if (!confirm(invocation,"Delete the selected TSSL packages?"))
            return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Usage,"Deletion requires confirmation");
        int removed = 0, failed = 0;
        auto errorCode = ExitCode::NotFound;
        QString firstError;
        for (const auto& root : selected) {
            if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
            auto package = store.packageForRootDigest(root);
            if (!package || !*package) { ++failed; firstError = package ? "TSSL package not found" : package.error(); if (!package) errorCode = ExitCode::Failed; continue; }
            auto result = store.deleteByRootDigest(root);
            if (result) ++removed; else { ++failed; firstError = result.error(); errorCode = ExitCode::Failed; }
        }
        const QJsonObject data {{"removed",removed},{"failed",failed}};
        return failed ? failure(removed ? ExitCode::Partial : errorCode,firstError,data) : success(data);
    }
    if (invocation.command == "tssl show" || invocation.command == "tssl export") {
        const auto root = digest(invocation.arguments[0]);
        if (!root) return failure(root.error());
        const auto package = awaitFuture(QtConcurrent::run([store, root = *root] { return store.packageForRootDigest(root); }));
        if (!package) return failure(ExitCode::Failed, package.error());
        if (!*package) return failure(ExitCode::NotFound, "TSSL package not found");
        if (invocation.command == "tssl show") {
            auto name = (**package).decryptedSourceFileName();
            return success(QJsonObject {{"digest",invocation.arguments[0]}, {"version",(**package).version},
                {"container",(**package).containerFormat}, {"manifests",(**package).manifestDigests.size()},
                {"segments",(**package).segmentKeys.size()}, {"resources",(**package).resourceDigests.size()},
                {"originalName", name && *name ? **name : QString()}});
        }
        const auto destination = QFileInfo(invocation.arguments[1]).absoluteFilePath();
        if (QFileInfo::exists(destination)) return failure(ExitCode::Conflict, "The export target already exists");
        const auto exported = store.exportByRootDigest(*root, destination);
        return exported ? success(QJsonObject {{"exported",destination}}) : failure(ExitCode::Failed, exported.error());
    }
    const auto settings = WorkflowSettings::snapshot();
    const auto targetType = invocation.option("target", settings.value("tsslBackup/target").toString());
    const bool restore = invocation.command == "tssl restore";
    const auto formatName = invocation.option("format", settings.value("tsslBackup/format").toString());
    if (!restore && formatName != "zip" && formatName != "files")
        return failure(ExitCode::Usage, "Select a backup format: zip or files");
    const auto format = formatName == "files" ? TsslBackupTarget::Format::Files : TsslBackupTarget::Format::Zip;
    if (targetType == "local") {
        const auto path = invocation.option("local-path", settings.value("tsslBackup/localPath").toString());
        if (path.isEmpty()) return failure(ExitCode::Usage, "Configure a local backup path or supply --local-path");
        if (restore) {
            const auto files = awaitFuture(QtConcurrent::run([path]() -> std::expected<QStringList,Error> {
                const QFileInfo directory(path);
                if (!directory.isDir()) return std::unexpected(Error {ExitCode::NotFound, "The backup directory does not exist"});
                if (!directory.isReadable()) return std::unexpected(Error {ExitCode::Failed, "The backup directory is not readable"});
                return TsslBackupArchive::discover(path);
            }));
            if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled, "Canceled");
            if (!files) return failure(files.error());
            if (files->isEmpty()) return failure(ExitCode::NotFound, "No TSSL backups found");
            return importFiles(store, *files);
        }
        // Preserve the legacy CLI's per-package partial-success report.
        if (format == TsslBackupTarget::Format::Files) {
            if (!QDir().mkpath(path)) return failure(ExitCode::Failed, "Unable to create local backup directory");
            const auto selected = awaitFuture(QtConcurrent::run([store]() -> std::expected<std::vector<QByteArray>,QString> {
                const auto summaries = store.listPackageSummaries();
                if (!summaries) return std::unexpected(summaries.error());
                std::vector<QByteArray> digests;
                for (const auto& package : *summaries) digests.push_back(package.rootManifestDigest);
                return digests;
            }));
            if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled, "Canceled");
            if (!selected) return failure(ExitCode::Failed, selected.error());
            if (selected->empty()) return failure(ExitCode::NotFound, "No valid TSSL packages to back up");
            auto result = exportPackages(store, *selected, path, QStringLiteral("backedUp"));
            auto data = result.data.toObject();
            data.insert(QStringLiteral("format"), formatName);
            result.data = data;
            return result;
        }
        const auto result = awaitFuture(TsslBackupService::backupLocal(store, QFileInfo(path).absoluteFilePath(),format,
            &CliConsole::canceled, [](const QString& stage,int done,int total) {
                CliConsole::progress(QStringLiteral("TSSL %1: %2 / %3").arg(stage).arg(done).arg(total));
            }));
        if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled, "Canceled");
        return result ? success(QJsonObject {{"backedUp",*result},{"target",QFileInfo(path).absoluteFilePath()},{"format",formatName}})
                      : failure(ExitCode::Failed, result.error());
    }
    TsslBackupTarget target;
    target.format = format;
    if (targetType == "webdav") {
        const auto service = invocation.option("service", settings.value("tsslBackup/webDavServiceId").toString());
        auto card = resolve(repository, service, true);
        if (!card) return failure(card.error());
        auto credential = password(invocation, card->server);
        if (!credential) return failure(credential.error());
        target.type = TsslBackupTarget::Type::WebDav;
        target.webDavServer = card->server;
        target.webDavPassword = *credential;
        target.webDavPath = invocation.option("path", settings.value("tsslBackup/webDavPath").toString());
        // Validate against the configured root using the same virtual path rules as file commands.
        auto url = remoteUrl(card->server, target.webDavPath);
        if (!url) return failure(url.error());
        auto base = QUrl(card->server.baseUrl).path();
        if (!base.endsWith('/')) base += '/';
        target.webDavPath = url->path().mid(base.size());
    } else if (targetType == "s3") {
        target.type = TsslBackupTarget::Type::S3;
        target.s3Endpoint = QUrl(invocation.option("s3-endpoint", settings.value("tsslBackup/s3Endpoint").toString()));
        target.s3Bucket = invocation.option("s3-bucket", settings.value("tsslBackup/s3Bucket").toString());
        target.s3Region = invocation.option("s3-region", settings.value("tsslBackup/s3Region").toString());
        target.s3Prefix = invocation.option("s3-prefix", settings.value("tsslBackup/s3Prefix").toString());
        target.s3AccessKey = invocation.option("s3-access-key", settings.value("tsslBackup/s3AccessKey").toString());
        if (invocation.has("s3-credentials")) {
            auto credentials = S3CredentialProvider::load(invocation.option("s3-credentials"),invocation.option("s3-profile"),&CliConsole::canceled);
            if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
            if (!credentials) return failure(ExitCode::Authentication,credentials.error());
            target.s3AccessKey = credentials->accessKey; target.s3SecretKey = credentials->secretKey;
            target.s3SessionToken = credentials->sessionToken;
        } else if (invocation.has("s3-secret-env")) {
            auto credential = secret(invocation, "s3-secret-env", "S3 secret: ");
            if (!credential) return failure(credential.error());
            target.s3SecretKey = *credential;
        } else if (CredentialStore::isAvailable()) {
            auto saved = CredentialStore::loadSecret("tsslBackupS3Secret");
            if (!saved) return failure(ExitCode::Authentication, saved.error());
            if (*saved) target.s3SecretKey = **saved;
        }
        if (target.s3SecretKey.isEmpty()) {
            auto credential = secret(invocation, "s3-secret-env", "S3 secret: ");
            if (!credential) return failure(credential.error());
            target.s3SecretKey = *credential;
        }
        if (invocation.has("s3-session-token-env")) {
            auto token = secret(invocation,"s3-session-token-env","S3 session token: ");
            if (!token) return failure(token.error());
            target.s3SessionToken = *token;
        }
        if (target.s3Endpoint.host().isEmpty() || !target.s3Endpoint.userInfo().isEmpty() ||
            (target.s3Endpoint.scheme() != "http" && target.s3Endpoint.scheme() != "https") ||
            target.s3Bucket.isEmpty() || target.s3AccessKey.isEmpty())
            return failure(ExitCode::Usage, "S3 endpoint, bucket and access key are required");
        for (const auto& value : {target.s3AccessKey,target.s3SecretKey,target.s3SessionToken})
            if (value.contains('\r') || value.contains('\n')) return failure(ExitCode::Usage,"Invalid S3 credential value");
        if (invocation.has("s3-role-arn")) {
            auto assumed = S3CredentialProvider::assumeRole({target.s3AccessKey,target.s3SecretKey,target.s3SessionToken},
                invocation.option("s3-role-arn"),target.s3Region,invocation.option("s3-role-session-name","vibePlayerQT-cli"),
                invocation.option("s3-external-id"),&CliConsole::canceled);
            if (!assumed) return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Authentication,assumed.error());
            target.s3AccessKey = assumed->accessKey; target.s3SecretKey = assumed->secretKey;
            target.s3SessionToken = assumed->sessionToken;
        }
    } else return failure(ExitCode::Usage, "Select a backup target: local, webdav or s3");
    const auto backupError = [](const QString& message) {
        const auto code = CliConsole::pollCancellation() ? ExitCode::Canceled :
            message.contains("HTTP 401") || message.contains("HTTP 403") ? ExitCode::Authentication :
            message.contains("HTTP 404") ? ExitCode::NotFound :
            message.contains("HTTP 409") || message.contains("HTTP 412") ? ExitCode::Conflict : ExitCode::Failed;
        return failure(code,message);
    };
    TsslBackupService backup;
    QObject::connect(&backup, &TsslBackupService::stageChanged, &backup, [](const QString& stage,int done, int total) {
        CliConsole::progress(QStringLiteral("TSSL %1: %2 / %3").arg(stage).arg(done).arg(total));
    });
    if (restore) {
        auto files = await<TsslBackupRestoreResult>([&](auto done) { backup.restore(target, done); }, [&] { backup.cancel(); });
        if (!files) return backupError(files.error());
        if (files->isEmpty()) { backup.cleanupRestoreFiles(); return failure(ExitCode::NotFound, "No remote TSSL backups found"); }
        const auto result = importFiles(store, *files);
        backup.cleanupRestoreFiles();
        return result;
    }
    auto packages = awaitFuture(TsslBackupService::preparePackages(store));
    if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled, "Canceled");
    if (!packages) return failure(ExitCode::Failed, packages.error());
    auto result = await<TsslBackupResult>([&](auto done) { backup.backup(target, packages->files, done); }, [&] { backup.cancel(); });
    return result ? success(QJsonObject {{"backedUp",*result}}) :
        backupError(result.error());
}
