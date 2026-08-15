#include "services/iptv/IptvPlaylistStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
QString safeFileName(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        value = QStringLiteral("playlist");
    }
    value.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|]+)")), QStringLiteral("_"));
    return value.left(80);
}

QString managedDirectory(const QString& storageRoot)
{
    const auto root = storageRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : storageRoot;
    return QDir(root).filePath(QStringLiteral("iptv"));
}
}

std::expected<QString, QString> IptvPlaylistStore::importFile(const QString& sourcePath,
                                                               const QString& serviceId,
                                                               const QString& storageRoot)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return std::unexpected(QStringLiteral("Selected IPTV playlist file does not exist"));
    }

    const auto extension = sourceInfo.suffix().toLower();
    if (extension != QStringLiteral("m3u") && extension != QStringLiteral("m3u8")) {
        return std::unexpected(QStringLiteral("Select an M3U or M3U8 playlist file"));
    }

    const auto directory = managedDirectory(storageRoot);
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        return std::unexpected(QStringLiteral("Unable to create IPTV playlist storage directory"));
    }

    QFile source(sourceInfo.absoluteFilePath());
    if (!source.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Unable to open IPTV playlist file for import"));
    }

    const auto targetName = QStringLiteral("%1.%2").arg(safeFileName(serviceId), extension);
    const auto targetPath = QDir(directory).absoluteFilePath(targetName);
    const QFileInfo targetInfo(targetPath);
    if (targetInfo.exists() && sourceInfo.canonicalFilePath() == targetInfo.canonicalFilePath()) {
        return targetPath;
    }

    QSaveFile target(targetPath);
    if (!target.open(QIODevice::WriteOnly)) {
        return std::unexpected(QStringLiteral("Unable to create managed IPTV playlist copy"));
    }

    constexpr qint64 chunkSize = 64 * 1024;
    while (!source.atEnd()) {
        const auto chunk = source.read(chunkSize);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            target.cancelWriting();
            return std::unexpected(QStringLiteral("Unable to read IPTV playlist file during import"));
        }
        if (target.write(chunk) != chunk.size()) {
            target.cancelWriting();
            return std::unexpected(QStringLiteral("Unable to write managed IPTV playlist copy"));
        }
    }

    if (!target.commit()) {
        return std::unexpected(QStringLiteral("Unable to finish importing IPTV playlist file"));
    }
    return targetPath;
}
