#pragma once

#include "models/ServerConfig.h"
#include "services/webdav/TsslStore.h"

#include <QByteArray>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSslError>
#include <QTcpServer>
#include <QUrl>

#include <expected>
#include <functional>

class QNetworkReply;
class QTcpSocket;

struct EncryptedHlsPreparedStream final {
    QUrl url;
    QString sessionId;
};

using EncryptedHlsPrepareResult = std::expected<EncryptedHlsPreparedStream, QString>;
using EncryptedHlsDigestResult = std::expected<QByteArray, QString>;

class EncryptedHlsPlaybackProxy final : public QObject {
    Q_OBJECT

public:
    explicit EncryptedHlsPlaybackProxy(TsslStore& store, QObject* parent = nullptr);

    void prepareStream(const ServerConfig& server,
                       const QString& password,
                       const QUrl& rootManifestUrl,
                       std::function<void(EncryptedHlsPrepareResult)> callback);
    void resolveRootDigest(const ServerConfig& server,
                           const QString& password,
                           const QUrl& rootManifestUrl,
                           std::function<void(EncryptedHlsDigestResult)> callback);
    void revoke(const QString& sessionId);

signals:
    void networkTrafficSample(const QString& serviceId,
                              const QString& serviceName,
                              const QString& serviceType,
                              qint64 bytesReceived,
                              qint64 bytesSent);

private:
    struct ResolvedPackage {
        QByteArray rootManifest;
        TsslPackage package;
    };

    struct Session {
        ServerConfig server;
        QString password;
        QUrl remoteDirectoryUrl;
        QString localManifestName;
        QByteArray rootManifest;
        TsslPackage package;
    };

    void resolvePackage(const ServerConfig& server,
                        const QString& password,
                        const QUrl& rootManifestUrl,
                        std::function<void(std::expected<ResolvedPackage, QString>)> callback);
    QNetworkReply* fetchRemoteBytes(const ServerConfig& server,
                                    const QString& password,
                                    const QUrl& url,
                                    qint64 maximumBytes,
                                    std::function<void(std::expected<QByteArray, QString>)> callback);
    bool ensureListening();
    void handlePendingConnection();
    void handleRequest(QTcpSocket* socket, const QByteArray& requestBytes);
    void serveManifest(QTcpSocket* socket,
                       const QString& sessionId,
                       const Session& session,
                       const QString& relativePath,
                       const QUrl& remoteUrl,
                       const QByteArray& method,
                       const QByteArray& expectedDigest);
    void serveSegment(QTcpSocket* socket,
                      const QString& sessionId,
                      const Session& session,
                      const QString& relativePath,
                      const QUrl& remoteUrl,
                      const QByteArray& method,
                      const QHash<QByteArray, QByteArray>& requestHeaders,
                      const QByteArray& key);
    void serveResource(QTcpSocket* socket,
                       const QString& sessionId,
                       const Session& session,
                       const QString& relativePath,
                       const QUrl& remoteUrl,
                       const QByteArray& method,
                       const QHash<QByteArray, QByteArray>& requestHeaders,
                       const QByteArray& expectedDigest);
    void wireReply(QNetworkReply* reply, const ServerConfig& server);

    TsslStore& m_store;
    QTcpServer m_server;
    QNetworkAccessManager m_manager;
    QHash<QString, Session> m_sessions;
};
