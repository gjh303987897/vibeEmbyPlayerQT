#pragma once

#include "models/ServerConfig.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSslError>
#include <QTcpServer>
#include <QUrl>

#include <functional>

class QNetworkReply;
class QTcpSocket;

class WebDavPlaybackProxy final : public QObject {
    Q_OBJECT

public:
    explicit WebDavPlaybackProxy(QObject* parent = nullptr);

    QUrl streamUrlFor(const ServerConfig& server, const QString& password, const QUrl& remoteUrl);
    void revoke(const QString& streamId);

signals:
    void certificateConfirmationRequired(const QString& host, const QList<QSslError>& errors, std::function<void(bool)> reply);
    void networkTrafficSample(const QString& serviceId,
                              const QString& serviceName,
                              const QString& serviceType,
                              qint64 bytesReceived,
                              qint64 bytesSent);

private:
    struct Stream {
        ServerConfig server;
        QString password;
        QUrl remoteUrl;
    };

    void ensureListening();
    void handlePendingConnection();
    void handleRequest(QTcpSocket* socket, const QByteArray& requestBytes);
    void proxyRemoteRequest(QTcpSocket* socket,
                            const Stream& stream,
                            const QByteArray& method,
                            const QHash<QByteArray, QByteArray>& requestHeaders);
    void wireReply(QNetworkReply* reply, const ServerConfig& server);
    QByteArray responseReason(int statusCode) const;

    QTcpServer m_server;
    QNetworkAccessManager m_manager;
    QHash<QString, Stream> m_streams;
};
