#pragma once

#include "models/ServerConfig.h"
#include "models/WebDavItem.h"
#include "network/NetworkError.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSslError>
#include <QUrl>

#include <expected>
#include <functional>
#include <memory>
#include <vector>

using WebDavListResult = std::expected<std::vector<WebDavItem>, NetworkError>;
using WebDavItemResult = std::expected<WebDavItem, NetworkError>;
using WebDavVoidResult = std::expected<void, NetworkError>;

class WebDavClient final : public QObject {
    Q_OBJECT

public:
    explicit WebDavClient(QObject* parent = nullptr);

    void listDirectory(const ServerConfig& server,
                       const QString& password,
                       const QUrl& directoryUrl,
                       std::function<void(WebDavListResult)> callback);

    void statItem(const ServerConfig& server,
                  const QString& password,
                  const QUrl& itemUrl,
                  std::function<void(WebDavItemResult)> callback);

    void createDirectory(const ServerConfig& server,
                         const QString& password,
                         const QUrl& directoryUrl,
                         std::function<void(WebDavVoidResult)> callback);

signals:
    void certificateConfirmationRequired(const QString& host, const QList<QSslError>& errors, std::function<void(bool)> reply);
    void networkTrafficSample(const QString& serviceId,
                              const QString& serviceName,
                              const QString& serviceType,
                              qint64 bytesReceived,
                              qint64 bytesSent);

private:
    void configureRequest(QNetworkRequest& request, const ServerConfig& server) const;
    void wireReply(QNetworkReply* reply, const ServerConfig& server);
    static NetworkError replyError(QNetworkReply* reply);

    QNetworkAccessManager m_manager;
};
