#pragma once

#include "models/ServerConfig.h"
#include "models/WebDavItem.h"
#include "network/NetworkError.h"
#include "services/webdav/WebDavClient.h"

#include <QString>
#include <QUrl>

#include <expected>
#include <functional>
#include <vector>

struct WebDavDownloadEntry {
    QUrl remoteUrl;
    QString localPath;
    qint64 bytesTotal { -1 };
};

struct WebDavDownloadPlan {
    std::vector<WebDavDownloadEntry> files;
    std::vector<QString> directories;
    qint64 bytesTotal { 0 };
    bool sizeComplete { true };
};

using WebDavDownloadPlanResult = std::expected<WebDavDownloadPlan, NetworkError>;

class WebDavDownloadPlanner final {
public:
    explicit WebDavDownloadPlanner(WebDavClient& client);

    void buildPlan(const ServerConfig& server,
                   const QString& password,
                   const WebDavItem& rootItem,
                   const QString& targetPath,
                   std::function<void(WebDavDownloadPlanResult)> callback);

private:
    WebDavClient& m_client;
};
