#include "services/local/LocalMediaService.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QThreadPool>

#include <algorithm>
#include <utility>

LocalMediaService::LocalMediaService(QObject* parent)
    : QObject(parent)
{
}

bool LocalMediaService::isSupportedVideoFile(const QString& path)
{
    static const QSet<QString> extensions {
        QStringLiteral("3g2"), QStringLiteral("3gp"), QStringLiteral("asf"),
        QStringLiteral("avi"), QStringLiteral("flv"), QStringLiteral("m2ts"),
        QStringLiteral("m4v"), QStringLiteral("mkv"), QStringLiteral("mov"),
        QStringLiteral("mp4"), QStringLiteral("mpeg"), QStringLiteral("mpg"),
        QStringLiteral("mts"), QStringLiteral("ogm"), QStringLiteral("ogv"),
        QStringLiteral("rm"), QStringLiteral("rmvb"), QStringLiteral("ts"),
        QStringLiteral("vob"), QStringLiteral("webm"), QStringLiteral("wmv"),
        QStringLiteral("m3u8s"),
    };
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

bool LocalMediaService::isEncryptedHlsManifest(const QString& path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("m3u8s"), Qt::CaseInsensitive) == 0;
}

LocalMediaService::VideoFileResult LocalMediaService::resolveVideoFile(const QUrl& url)
{
    if (!url.isLocalFile()) {
        return std::unexpected(QStringLiteral("Only local video files can be opened"));
    }

    const QFileInfo fileInfo(url.toLocalFile());
    const auto canonicalPath = fileInfo.canonicalFilePath();
    if (canonicalPath.isEmpty() || !fileInfo.isFile() || !fileInfo.isReadable()) {
        return std::unexpected(QStringLiteral("The local video file is unavailable or unreadable"));
    }
    if (!isSupportedVideoFile(canonicalPath)) {
        return std::unexpected(QStringLiteral("The dropped file is not a supported video"));
    }
    return QDir::cleanPath(canonicalPath);
}

LocalMediaService::BrowseResult LocalMediaService::browseDirectory(const QString& path)
{
    const QFileInfo directoryInfo(path);
    if (!directoryInfo.exists() || !directoryInfo.isDir()) {
        return std::unexpected(QStringLiteral("The local media folder does not exist"));
    }
    if (!directoryInfo.isReadable()) {
        return std::unexpected(QStringLiteral("The local media folder is not readable"));
    }

    const QDir directory(directoryInfo.absoluteFilePath());
    const auto entries = directory.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    const auto containsEncryptedHlsPackage = std::ranges::any_of(entries, [](const QFileInfo& entry) {
        return entry.isFile() && LocalMediaService::isEncryptedHlsManifest(entry.fileName());
    });
    static const QRegularExpression generatedSegmentPattern(
        QStringLiteral("^segment_\\d{6}\\.ts$"),
        QRegularExpression::CaseInsensitiveOption);

    std::vector<LocalMediaItem> items;
    items.reserve(static_cast<size_t>(entries.size()));
    for (const auto& entry : entries) {
        if (entry.isSymLink()) {
            continue;
        }
        const bool directoryEntry = entry.isDir();
        if (!directoryEntry && !isSupportedVideoFile(entry.fileName())) {
            continue;
        }
        if (!directoryEntry && containsEncryptedHlsPackage &&
            generatedSegmentPattern.match(entry.fileName()).hasMatch()) {
            continue;
        }
        items.push_back(LocalMediaItem {
            .name = entry.fileName(),
            .path = QDir::cleanPath(entry.absoluteFilePath()),
            .lastModified = entry.lastModified(),
            .size = directoryEntry ? 0 : entry.size(),
            .directory = directoryEntry,
        });
    }

    std::ranges::stable_sort(items, [](const LocalMediaItem& left, const LocalMediaItem& right) {
        if (left.directory != right.directory) {
            return left.directory;
        }
        return QString::localeAwareCompare(left.name, right.name) < 0;
    });
    return items;
}

void LocalMediaService::browseDirectoryAsync(QString path, BrowseCallback callback)
{
    const QPointer<LocalMediaService> owner(this);
    QThreadPool::globalInstance()->start([owner, path = std::move(path), callback = std::move(callback)]() mutable {
        auto result = browseDirectory(path);
        if (!owner) {
            return;
        }
        QMetaObject::invokeMethod(owner, [owner, callback = std::move(callback), result = std::move(result)]() mutable {
            if (owner) {
                callback(std::move(result));
            }
        }, Qt::QueuedConnection);
    });
}
