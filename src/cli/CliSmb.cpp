#include "cli/CliEngine.h"
#include "cli/CliRuntime.h"
#include "cli/CliCheckpoint.h"
#include "services/smb/SmbClient.h"
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QThread>
#include <QtConcurrentRun>

namespace {
bool canceled()
{
    while (CliConsole::transferPaused.load() && !CliConsole::canceled.load()) QThread::msleep(20);
    return CliConsole::canceled.load();
}
Cli::Result copyTree(const QString& source, const QString& destination, bool overwrite, const QString& checkpoints = {})
{
    if (!QFileInfo::exists(source)) return Cli::failure(Cli::ExitCode::NotFound,"Source not found");
    if (SmbClient::hasLinkedAncestor(source) || SmbClient::hasLinkedAncestor(destination))
        return Cli::failure(Cli::ExitCode::Usage,"Symbolic links are not followed");
    const auto root = QFileInfo(source).absoluteFilePath();
    const auto target = QFileInfo(destination).absoluteFilePath();
    if (root.compare(target,Qt::CaseInsensitive) == 0 ||
        target.startsWith(root + '/',Qt::CaseInsensitive))
        return Cli::failure(Cli::ExitCode::Usage,"Cannot copy a resource into itself");
    const auto rootRecord = Cli::checkpoint(checkpoints,"root");
    const bool recovery = rootRecord.value("source").toString() == root && rootRecord.value("target").toString() == target;
    if (QFileInfo::exists(target) && !overwrite && !recovery) return Cli::failure(Cli::ExitCode::Conflict,"Target already exists");
    if (!Cli::saveCheckpoint(checkpoints,"root",QJsonObject {{"source",root},{"target",target}}))
        return Cli::failure(Cli::ExitCode::Failed,"Cannot save SMB task checkpoint");
    QStringList files;
    if (QFileInfo(root).isFile()) files += root;
    else {
        if (!QDir().mkpath(target)) return Cli::failure(Cli::ExitCode::Failed,"Cannot create destination");
        QDirIterator iterator(root,QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (canceled()) return Cli::failure(Cli::ExitCode::Canceled,"Canceled");
            iterator.next();
            if (iterator.fileInfo().isSymLink() || iterator.fileInfo().isJunction())
                return Cli::failure(Cli::ExitCode::Usage,"Symbolic links and junctions are not followed");
            const auto relative = QDir(root).relativeFilePath(iterator.filePath());
            if (iterator.fileInfo().isDir()) {
                if (SmbClient::hasLinkedAncestor(QDir(target).filePath(relative)))
                    return Cli::failure(Cli::ExitCode::Usage,"Destination contains a link or junction");
                if (!QDir().mkpath(QDir(target).filePath(relative))) return Cli::failure(Cli::ExitCode::Failed,"Cannot create directory");
            } else files += iterator.filePath();
        }
    }
    int completed = 0;
    for (const auto& file : files) {
        if (canceled()) return Cli::failure(Cli::ExitCode::Canceled,"Canceled",QJsonObject {{"completed",completed}});
        const auto output = QFileInfo(root).isFile() ? target : QDir(target).filePath(QDir(root).relativeFilePath(file));
        const auto record = Cli::checkpoint(checkpoints,output);
        if (!record.value("hash").toString().isEmpty() && record.value("hash").toString() == Cli::fileHash(file) &&
            record.value("hash").toString() == Cli::fileHash(output)) { ++completed; continue; }
        if (SmbClient::hasLinkedAncestor(output) || (QFileInfo::exists(output) && !overwrite))
            return Cli::failure(completed ? Cli::ExitCode::Partial : Cli::ExitCode::Conflict,"Destination exists",QJsonObject {{"completed",completed}});
        QFile input(file);
        QTemporaryFile temporary(QDir(QFileInfo(output).absolutePath()).filePath(".vibe-smb-XXXXXX"));
        if (!overwrite && !temporary.open()) return Cli::failure(Cli::ExitCode::Failed,"Cannot stage transfer");
        const auto staged = temporary.fileName(); temporary.close();
        QSaveFile sink(overwrite ? output : staged);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!input.open(QIODevice::ReadOnly) || !sink.open(QIODevice::WriteOnly))
            return Cli::failure(completed ? Cli::ExitCode::Partial : Cli::ExitCode::Failed,"Cannot open transfer file");
        while (!input.atEnd()) {
            if (canceled()) return Cli::failure(Cli::ExitCode::Canceled,"Canceled",QJsonObject {{"completed",completed}});
            auto bytes = input.read(1024 * 1024);
            if (bytes.isEmpty() || sink.write(bytes) != bytes.size()) return Cli::failure(Cli::ExitCode::Failed,"File transfer failed");
            hash.addData(bytes);
        }
        // A new-only target is published by rename below, retaining conflict protection.
        if (!overwrite && QFileInfo::exists(output)) return Cli::failure(Cli::ExitCode::Conflict,"Destination appeared during transfer");
        if (!sink.commit()) return Cli::failure(Cli::ExitCode::Failed,"Cannot publish transfer file");
        if (!overwrite && !QFile::rename(staged,output)) return Cli::failure(Cli::ExitCode::Conflict,"Destination exists or cannot be published");
        if (!Cli::saveCheckpoint(checkpoints,output,QJsonObject {{"hash",QString::fromLatin1(hash.result().toHex())}}))
            return Cli::failure(Cli::ExitCode::Failed,"Cannot save completed SMB task checkpoint");
        ++completed;
        CliConsole::progress(QStringLiteral("SMB transfer: %1 / %2").arg(completed).arg(files.size()));
    }
    return Cli::success(QJsonObject {{"completed",completed},{"target",target}});
}
}

Cli::Result Cli::Engine::smb(const Invocation& invocation, SessionRepository& repository)
{
    auto card = resolve(repository,invocation.arguments.front());
    if (!card) return failure(card.error());
    if (card->server.serviceType != ServiceType::SMB) return failure(ExitCode::Usage,"This command requires an SMB service");
    auto credential = password(invocation,card->server);
    if (!credential) return failure(credential.error());
    auto connection = awaitFuture(QtConcurrent::run([server = card->server,credential = *credential] { return SmbClient::connect(server,credential); }));
    if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
    if (!connection) return failure(ExitCode::Authentication,connection.error());
    const auto operation = invocation.command.section(' ',1);
    auto source = SmbClient::path(card->server,operation == "upload" ? QString("/") : invocation.arguments.value(1,"/"));
    if (!source) return failure(ExitCode::Usage,source.error());
    if (operation == "remove" && !confirm(invocation,"Delete this SMB resource and its children?"))
        return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Usage,"Deletion requires --yes or confirmation");
    const auto serviceRoot = SmbClient::path(card->server,"/");
    if ((operation == "remove" || operation == "move") && source == serviceRoot)
        return failure(ExitCode::Usage,"The service root cannot be modified");
    m_smbRunning = operation == "upload" || operation == "download";
    CliConsole::transferPaused.store(false);
    struct Reset { bool& running; ~Reset() { running = false; CliConsole::transferPaused.store(false); } } reset {m_smbRunning};
    return awaitFuture(QtConcurrent::run([invocation,operation,server = card->server,source = *source,checkpoints = m_checkpointDirectory] {
        if (SmbClient::hasLinkedAncestor(source)) return failure(ExitCode::Usage,"Symbolic links and junctions are not followed");
        const QFileInfo info(source);
        auto json = [](const QFileInfo& item) { return QJsonObject {{"name",item.fileName()},{"directory",item.isDir()},
            {"size",static_cast<double>(item.size())},{"modifiedAt",item.lastModified().toString(Qt::ISODate)}}; };
        if (operation == "upload") {
            auto target = SmbClient::path(server,invocation.arguments[2]);
            return target ? copyTree(invocation.arguments[1],*target,invocation.has("overwrite"),checkpoints) : failure(ExitCode::Usage,target.error());
        }
        if (operation == "mkdir") {
            if (info.exists() && !(invocation.has("parents") && info.isDir())) return failure(ExitCode::Conflict,"Directory already exists");
            const auto ok = invocation.has("parents") ? QDir().mkpath(source) : QDir(info.absolutePath()).mkdir(info.fileName());
            return ok ? success(QJsonObject {{"created",invocation.arguments[1]}}) : failure(ExitCode::Failed,"Cannot create SMB directory");
        }
        if (!info.exists()) return failure(ExitCode::NotFound,"SMB resource not found");
        if (operation == "stat") return success(json(info));
        if (operation == "ls") {
            if (!info.isDir()) return failure(ExitCode::Usage,"Path is not a directory");
            QJsonArray entries;
            for (const auto& item : QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,QDir::DirsFirst | QDir::Name)) entries += json(item);
            return success(entries);
        }
        if (operation == "download") return copyTree(source,invocation.arguments[2],invocation.has("overwrite"),checkpoints);
        if (operation == "remove") {
            if (info.isDir()) {
                QDirIterator scan(source,QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,QDirIterator::Subdirectories);
                while (scan.hasNext()) {
                    if (canceled()) return failure(ExitCode::Canceled,"Canceled");
                    scan.next();
                    if (scan.fileInfo().isSymLink() || scan.fileInfo().isJunction())
                        return failure(ExitCode::Usage,"Directory contains a link or junction");
                }
            }
            const bool ok = info.isDir() ? QDir(source).removeRecursively() : QFile::remove(source);
            return ok ? success(QJsonObject {{"removed",invocation.arguments[1]}}) : failure(ExitCode::Failed,"SMB deletion failed");
        }
        if (operation == "move" || operation == "copy") {
            auto target = SmbClient::path(server,invocation.arguments[2]);
            if (!target) return failure(ExitCode::Usage,target.error());
            if (operation == "copy") return copyTree(source,*target,invocation.has("overwrite"));
            if (QFileInfo::exists(*target) && !invocation.has("overwrite")) return failure(ExitCode::Conflict,"Move target already exists");
            auto moved = SmbClient::move(source,*target,invocation.has("overwrite"));
            return moved ? success(QJsonObject {{"target",invocation.arguments[2]}}) : failure(ExitCode::Failed,moved.error());
        }
        return failure(ExitCode::Usage,"Unsupported SMB command");
    }));
}
