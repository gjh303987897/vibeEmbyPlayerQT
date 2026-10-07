#include "cli/CliEngine.h"
#include "cli/CliRuntime.h"
#include "services/management/WorkflowSettings.h"
#include "services/encryptedhls/EncryptedHlsBatchPackager.h"
#include "services/encryptedhls/EncryptedHlsRestorer.h"
#include "services/encryptedhls/EncryptedHlsSourcePlanner.h"
#include "services/ffmpeg/FfmpegCapability.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QtConcurrentRun>

Cli::Result Cli::Engine::video(const Invocation& invocation, SessionRepository& repository)
{
    auto settings = WorkflowSettings::snapshot();
    const QMap<QString,QString> overrides {
        {"container","m3u8s/containerFormat"}, {"video","m3u8s/videoEncoding"},
        {"audio","m3u8s/audioEncoding"}, {"quality","m3u8s/videoQuality"}, {"jobs","m3u8s/parallelJobs"},
        {"ffmpeg","m3u8s/ffmpegExecutable"}, {"temp","m3u8s/temporaryDirectory"},
        {"auto-codecs","m3u8s/autoVideoCodecs"}, {"auto-target","m3u8s/autoVideoTarget"},
        {"format","encryptedHlsRestore/format"},
    };
    for (auto it = overrides.begin(); it != overrides.end(); ++it) if (invocation.has(it.key())) {
        QVariant value(invocation.option(it.key()));
        if (auto valid = WorkflowSettings::validate(it.value(), value); !valid) return failure(ExitCode::Usage, valid.error());
        settings[it.value()] = value;
    }
    if (invocation.has("ffmpeg") && !QFileInfo(settings.value("m3u8s/ffmpegExecutable").toString()).isFile())
        return failure(ExitCode::NotFound, "The explicitly requested FFmpeg executable does not exist");
    EncryptedHlsPackager::setConfiguredExecutablePath(settings.value("m3u8s/ffmpegExecutable").toString());
    const auto capability = awaitFuture(QtConcurrent::run([] {
        return FfmpegCapabilityProbe::run(EncryptedHlsPackager::locateFfmpegExecutable());
    }));
    if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled, "Canceled");
    if (!capability.usable()) return failure(ExitCode::Failed, capability.detail);
    TsslStore store(storagePath());
    if (invocation.command == "video restore") {
        QStringList sources;
        std::unique_ptr<QTemporaryDir> remoteStaging;
        if (invocation.has("service")) {
            const auto temp = settings.value("m3u8s/temporaryDirectory").toString();
            remoteStaging = std::make_unique<QTemporaryDir>(QDir(temp.isEmpty() ? QDir::tempPath() : temp).filePath("vibe-restore-XXXXXX"));
            if (!remoteStaging->isValid()) return failure(ExitCode::Failed,"Cannot create remote restore staging directory");
            int index = 0;
            for (const auto& source : invocation.arguments) {
                auto download = invocation;
                download.command = "webdav download";
                download.flags.remove("overwrite");
                const auto destination = QDir(remoteStaging->path()).filePath(QString::number(index++));
                const auto parent = source.endsWith(".m3u8s",Qt::CaseInsensitive) ? QFileInfo(source).path() : source;
                download.arguments = {invocation.option("service"),parent,destination};
                auto result = webdav(download,repository);
                if (result.code != ExitCode::Success) return result;
                sources += source.endsWith(".m3u8s",Qt::CaseInsensitive)
                    ? QDir(destination).filePath(QFileInfo(source).fileName()) : destination;
            }
        } else sources = invocation.arguments;
        QStringList packages;
        for (const auto& source : sources) {
            if (!QFileInfo(source).isDir()) { packages += source; continue; }
            QDirIterator iterator(source,{"*.m3u8s","*.m3u8sp"},QDir::Files | QDir::NoSymLinks,QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
                packages += iterator.next();
            }
        }
        packages.removeDuplicates();
        if (packages.isEmpty()) return failure(ExitCode::NotFound,"No M3U8S/M3U8SP packages found");
        const auto output = invocation.option("output", settings.value("encryptedHlsRestore/outputDirectory").toString());
        if (output.isEmpty()) return failure(ExitCode::Usage, "Supply --output or configure the restore output directory");
        if (!QDir().mkpath(output)) return failure(ExitCode::Failed, "Unable to create output directory");
        EncryptedHlsRestorer restorer(store);
        QObject::connect(&restorer,&EncryptedHlsRestorer::stateChanged,&restorer,
            [&,last = -1]() mutable {
                const auto percent = static_cast<int>(restorer.progress() * 100);
                if (percent != last) { last = percent; CliConsole::progress(QStringLiteral("Video restore: %1%").arg(percent)); }
            });
        QJsonArray rows;
        int succeeded = 0, failed = 0;
        QObject::connect(&restorer, &EncryptedHlsRestorer::itemChanged, &restorer,
            [&](int index, const QString& state, const QString& path, const QString& error, bool legacy) {
                if (state == "completed" || state == "failed" || state == "canceled") {
                    if (state == "completed") ++succeeded;
                    else if (state == "failed") ++failed;
                    rows += QJsonObject {{"index",index}, {"state",state}, {"output",path},
                        {"error",error}, {"legacyName",legacy}};
                    CliConsole::progress(QStringLiteral("Video restore: %1 / %2").arg(rows.size()).arg(packages.size()));
                }
            });
        auto finished = await<std::expected<bool,QString>>([&](auto done) {
            QObject::connect(&restorer, &EncryptedHlsRestorer::finished, &restorer, [done](bool canceled) { done(canceled); });
            const auto started = restorer.start({packages, output, settings.value("encryptedHlsRestore/format").toString()});
            if (!started) done(std::unexpected(started.error()));
        }, [&] { restorer.cancel(); });
        const QJsonObject data {{"succeeded",succeeded}, {"failed",failed}, {"items",rows}};
        if (!finished) return failure(ExitCode::Failed, finished.error(), data);
        if (*finished || CliConsole::pollCancellation()) return failure(ExitCode::Canceled, "Canceled", data);
        return failed ? failure(succeeded ? ExitCode::Partial : ExitCode::Failed, "Videos could not be restored; inspect the item results", data) : success(data);
    }
    const bool remote = invocation.has("webdav") || (!invocation.has("output") && settings.value("m3u8s/outputMode") == "webdav");
    std::optional<ServiceCard> destination;
    QString credential;
    QUrl remoteRoot;
    if (remote) {
        auto card = resolve(repository, invocation.option("webdav", settings.value("m3u8s/webDavServiceId").toString()), true);
        if (!card) return failure(card.error());
        auto secret = password(invocation, card->server);
        if (!secret) return failure(secret.error());
        auto url = remoteUrl(card->server, invocation.option("path", settings.value("m3u8s/webDavPath").toString()));
        if (!url) return failure(url.error());
        destination = *card; credential = *secret; remoteRoot = *url;
        // Create the configured output ancestry, without overwriting a resource.
        auto parent = QUrl(card->server.baseUrl);
        auto basePath = parent.path();
        if (!basePath.endsWith('/')) basePath += '/';
        const auto parts = remoteRoot.path().mid(basePath.size()).split('/', Qt::SkipEmptyParts);
        for (const auto& part : parts) {
            auto child = WebDavClient::childDirectoryUrl(parent, part);
            if (!child) return failure(ExitCode::Usage, child.error());
            WebDavClient client;
            auto info = await<WebDavItemResult>([&](auto done) { client.statItem(card->server, credential, *child, done); },
                                               [&] { client.cancelRequests(); });
            if (info && !info->directory) return failure(ExitCode::Conflict, "A video output parent is a file");
            if (!info) {
                if (info.error().httpStatus != 404) return networkFailure(info.error());
                auto created = await<WebDavVoidResult>([&](auto done) { client.createDirectory(card->server, credential, *child, done); },
                                                      [&] { client.cancelRequests(); });
                if (!created) return networkFailure(created.error());
            }
            parent = *child;
        }
    }
    auto root = invocation.option("output", settings.value(remote ? "m3u8s/fallbackDirectory" : "m3u8s/outputDirectory").toString());
    if (root.isEmpty()) root = WorkflowSettings::defaultOutputDirectory();
    root = QFileInfo(root).absoluteFilePath();
    if (!QDir().mkpath(root)) return failure(ExitCode::Failed, "Unable to create packaging output directory");
    std::unique_ptr<QTemporaryDir> staging;
    const bool keep = invocation.has("keep-local") || settings.value("m3u8s/keepSuccessfulLocal").toBool();
    if (remote) {
        staging = std::make_unique<QTemporaryDir>(QDir(root).filePath("vibe-cli-XXXXXX"));
        if (!staging->isValid()) return failure(ExitCode::Failed, "Unable to create packaging staging directory");
        root = staging->path();
    }
    auto plan = awaitFuture(QtConcurrent::run([sources = invocation.arguments, root] {
        return EncryptedHlsSourcePlanner::plan(sources, root, CliConsole::canceled);
    }));
    if (!plan) return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Failed, plan.error());
    EncryptedHlsBatchRequest request;
    request.parallelJobs = settings.value("m3u8s/parallelJobs").toInt();
    for (const auto& source : plan->sources) {
        EncryptedHlsPackageRequest package;
        package.sourcePath = source.sourcePath;
        package.outputDirectory = source.outputDirectory;
        WorkflowSettings::applyPackaging(settings, package);
        request.packages += package;
    }
    EncryptedHlsBatchPackager packager(store);
    QObject::connect(&packager,&EncryptedHlsBatchPackager::progressChanged,&packager,
        [&,last = -1]() mutable {
            const auto percent = static_cast<int>(packager.progress() * 100);
            if (percent != last) { last = percent; CliConsole::progress(QStringLiteral("Video pack: %1%").arg(percent)); }
        });
    QObject::connect(&packager, &EncryptedHlsBatchPackager::itemCompleted, &packager,
        [&](const EncryptedHlsPackageResult&) {
            CliConsole::progress(QStringLiteral("Video pack: %1 / %2").arg(packager.processedCount()).arg(packager.totalCount()));
        });
    auto result = await<std::expected<EncryptedHlsBatchResult,QString>>([&](auto done) {
        QObject::connect(&packager, &EncryptedHlsBatchPackager::completed, &packager, [done](const auto& value) { done(value); });
        QObject::connect(&packager, &EncryptedHlsBatchPackager::canceled, &packager, [done](const auto& value) { done(value); });
        const auto started = packager.start(request);
        if (!started) done(std::unexpected(started.error()));
    }, [&] { packager.cancel(); });
    if (!result) return failure(ExitCode::Failed, result.error());
    QJsonArray items;
    int failed = result->failures.size(), uploaded = 0;
    for (const auto& package : result->packages) {
        QJsonObject item {{"output",package.manifestPath}, {"digest",QString::fromLatin1(package.rootManifestDigest.toHex())}};
        if (remote && !CliConsole::pollCancellation()) {
            // The source plan preserves folder hierarchy underneath the selected output root.
            const auto local = settings.value("m3u8s/containerFormat") == "m3u8s"
                ? QFileInfo(package.manifestPath).absolutePath() : package.manifestPath;
            const auto relative = QDir(root).relativeFilePath(local);
            // A folder source can add further parents below the output root.
            auto parent = remoteRoot;
            const auto relativeParts = relative.split('/');
            bool parentsReady = true;
            for (const auto& part : relativeParts.mid(0, relativeParts.size() - 1)) {
                auto child = WebDavClient::childDirectoryUrl(parent, part);
                if (!child) { item["error"] = child.error(); parentsReady = false; break; }
                WebDavClient client;
                auto info = await<WebDavItemResult>([&](auto done) { client.statItem(destination->server, credential, *child, done); },
                                                   [&] { client.cancelRequests(); });
                if (!info && info.error().httpStatus == 404) {
                    auto created = await<WebDavVoidResult>([&](auto done) { client.createDirectory(destination->server, credential, *child, done); },
                                                          [&] { client.cancelRequests(); });
                    if (!created) { item["error"] = networkFailure(created.error()).message; parentsReady = false; break; }
                } else if (!info || !info->directory) {
                    item["error"] = info ? "Output parent is a file" : networkFailure(info.error()).message;
                    parentsReady = false; break;
                }
                parent = *child;
            }
            if (!parentsReady) { ++failed; item["uploaded"] = false; items += item; continue; }
            auto url = remoteRoot;
            auto encoded = url.path(QUrl::FullyEncoded);
            if (!encoded.endsWith('/')) encoded += '/';
            for (const auto& part : relative.split('/')) encoded += QString::fromLatin1(QUrl::toPercentEncoding(part)) + '/';
            if (QFileInfo(local).isFile()) encoded.chop(1);
            url.setPath(encoded, QUrl::StrictMode);
            const auto transfer = transferUpload(invocation, destination->server, credential, local, url);
            if (transfer.code == ExitCode::Success) { ++uploaded; item["uploaded"] = true; }
            else { ++failed; item["uploaded"] = false; item["error"] = transfer.message; }
        }
        items += item;
    }
    for (const auto& problem : result->failures) items += QJsonObject {{"source",problem.sourcePath}, {"error",problem.error}};
    const bool canceled = CliConsole::pollCancellation();
    if (staging && (keep || failed || canceled)) staging->setAutoRemove(false);
    const QJsonObject data {{"succeeded",result->packages.size()}, {"failed",failed}, {"uploaded",uploaded},
        {"items",items}, {"localRetained", !remote || keep || failed || canceled},
        {"localRoot", !remote || keep || failed || canceled ? root : QString()}};
    if (canceled) return failure(ExitCode::Canceled, "Canceled", data);
    return failed ? failure(result->packages.isEmpty() ? ExitCode::Failed : ExitCode::Partial,
        "Packaging or upload operations failed; local results were retained", data) : success(data);
}
