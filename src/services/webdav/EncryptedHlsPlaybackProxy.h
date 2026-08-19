#pragma once

#include "models/ServerConfig.h"
#include "services/webdav/TsslStore.h"

#include <QByteArray>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTcpServer>
#include <QUrl>

#include <expected>
#include <functional>

class QNetworkReply;
class QTcpSocket;

struct EncryptedHlsPreparedStream final {
    QUrl url;
    QString sessionId;
    QString displayName;
};

using EncryptedHlsPrepareResult = std::expected<EncryptedHlsPreparedStream, QString>;
using EncryptedHlsDigestResult = std::expected<QByteArray, QString>;
struct EncryptedHlsIdentifierPreview final {
    QString identifier;
    QString sourceFileName;
};

using EncryptedHlsIdentifierPreviewResult = std::expected<EncryptedHlsIdentifierPreview, QString>;

class EncryptedHlsPlaybackProxy final : public QObject {
    Q_OBJECT

public:
    explicit EncryptedHlsPlaybackProxy(TsslStore& store, QObject* parent = nullptr);

    void prepareStream(const ServerConfig& server,
                       const QString& password,
                       const QUrl& rootManifestUrl,
                       std::function<void(EncryptedHlsPrepareResult)> callback);
    void prepareLocalStream(const QString& rootManifestPath,
                            std::function<void(EncryptedHlsPrepareResult)> callback);
    void resolveRootDigest(const ServerConfig& server,
                           const QString& password,
                           const QUrl& rootManifestUrl,
                           std::function<void(EncryptedHlsDigestResult)> callback);
    void resolveIdentifierPreview(const ServerConfig& server,
                                  const QString& password,
                                  const QUrl& rootManifestUrl,
                                  std::function<void(EncryptedHlsIdentifierPreviewResult)> callback);
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
        QString sourceFileName;
    };

    struct Session {
        bool localSource { false };
        ServerConfig server;
        QString password;
        QUrl remoteDirectoryUrl;
        QString localDirectoryPath;
        QString localManifestName;
        QByteArray rootManifest;
        TsslPackage package;
    };

    void resolvePackage(const ServerConfig& server,
                        const QString& password,
                        const QUrl& rootManifestUrl,
                        std::function<void(std::expected<ResolvedPackage, QString>)> callback);
    static std::expected<ResolvedPackage, QString> resolvePackageBytes(QByteArray manifest,
                                                                       const TsslStore& store);
    void finishPreparingStream(ResolvedPackage resolved,
                               Session session,
                               QString fallbackDisplayName,
                               std::function<void(EncryptedHlsPrepareResult)> callback);
    QNetworkReply* fetchRemoteBytes(const ServerConfig& server,
                                    const QString& password,
                                    const QUrl& url,
                                    qint64 maximumBytes,
                                    std::function<void(std::expected<QByteArray, QString>)> callback);
    void fetchLocalBytes(const QString& path,
                         qint64 maximumBytes,
                         std::function<void(std::expected<QByteArray, QString>)> callback);
    QNetworkReply* fetchSessionBytes(const Session& session,
                                     const QUrl& sourceUrl,
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
