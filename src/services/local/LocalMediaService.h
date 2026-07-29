#pragma once

#include "models/LocalMediaItem.h"

#include <QObject>
#include <QString>

#include <expected>
#include <functional>
#include <vector>

class LocalMediaService final : public QObject {
    Q_OBJECT

public:
    using BrowseResult = std::expected<std::vector<LocalMediaItem>, QString>;
    using BrowseCallback = std::function<void(BrowseResult)>;

    explicit LocalMediaService(QObject* parent = nullptr);

    static BrowseResult browseDirectory(const QString& path);
    void browseDirectoryAsync(QString path, BrowseCallback callback);
    static bool isSupportedVideoFile(const QString& path);
};
