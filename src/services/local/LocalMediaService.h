#pragma once

#include "models/LocalMediaItem.h"

#include <QObject>
#include <QString>
#include <QUrl>

#include <expected>
#include <functional>
#include <vector>

class LocalMediaService final : public QObject {
    Q_OBJECT

public:
    struct DirectoryListing final {
        QString path;
        std::vector<LocalMediaItem> items;
    };

    using BrowseResult = std::expected<std::vector<LocalMediaItem>, QString>;
    using BrowseCallback = std::function<void(BrowseResult)>;
    using DirectoryListingResult = std::expected<DirectoryListing, QString>;
    using DirectoryListingCallback = std::function<void(DirectoryListingResult)>;
    using VideoFileResult = std::expected<QString, QString>;

    explicit LocalMediaService(QObject* parent = nullptr);

    static BrowseResult browseDirectory(const QString& path);
    void browseDirectoryAsync(QString path, BrowseCallback callback);
    static DirectoryListingResult browseDirectoryWithinRoot(const QString& rootPath,
                                                            const QString& path);
    void browseDirectoryWithinRootAsync(QString rootPath,
                                        QString path,
                                        DirectoryListingCallback callback);
    static bool isSupportedVideoFile(const QString& path);
    static bool isEncryptedHlsManifest(const QString& path);
    static VideoFileResult resolveVideoFile(const QUrl& url);
};
