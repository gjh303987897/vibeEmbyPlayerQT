#include "services/webdav/WebDavUploadPlanner.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <algorithm>

namespace WebDavUploadPlanner {
std::expected<Requests, QString> plan(const QString& source, const QUrl& target,
                                     const std::atomic_bool& canceled)
{
    const QFileInfo root(source);
    if (!root.exists() || !root.isReadable() || root.isSymLink())
        return std::unexpected(QStringLiteral("The upload source must be a readable file or folder, not a symbolic link"));
    Requests requests;
    if (root.isFile()) {
        requests.push_back({ .remoteUrl = target, .localPath = root.absoluteFilePath(),
                             .totalBytes = root.size(), .title = root.fileName() });
        return requests;
    }
    if (!root.isDir()) return std::unexpected(QStringLiteral("Unsupported upload source"));
    auto rootUrl = target;
    auto base = rootUrl.path(QUrl::FullyEncoded);
    if (!base.endsWith('/')) base += '/';
    rootUrl.setPath(base, QUrl::StrictMode);
    requests.push_back({ .remoteUrl = rootUrl, .title = root.fileName() + '/', .directory = true });
    Requests directories, files;
    QDirIterator it(root.absoluteFilePath(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                    QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (canceled.load()) return std::unexpected(QStringLiteral("Upload planning canceled"));
        it.next();
        const auto info = it.fileInfo();
        if (!info.isReadable()) return std::unexpected(QStringLiteral("An upload entry is not readable"));
        const auto relative = QDir(root.absoluteFilePath()).relativeFilePath(info.absoluteFilePath());
        QString encoded;
        for (const auto& segment : relative.split('/')) {
            if (!encoded.isEmpty()) encoded += '/';
            encoded += QString::fromLatin1(QUrl::toPercentEncoding(segment));
        }
        auto url = rootUrl;
        url.setPath(base + encoded + (info.isDir() ? QStringLiteral("/") : QString()), QUrl::StrictMode);
        if (info.isDir()) directories.push_back({ .remoteUrl = url, .title = relative + '/', .directory = true });
        else files.push_back({ .remoteUrl = url, .localPath = info.absoluteFilePath(),
                              .totalBytes = info.size(), .title = relative });
    }
    std::ranges::sort(directories, {}, &TransferManager::TaskRequest::title);
    requests.insert(requests.end(), std::make_move_iterator(directories.begin()), std::make_move_iterator(directories.end()));
    requests.insert(requests.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
    return requests;
}
}
