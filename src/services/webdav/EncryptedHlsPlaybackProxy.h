#pragma once

#include "models/ServerConfig.h"
#include "services/webdav/TsslStore.h"
#include "services/encryptedhls/EncryptedHlsTarContainer.h"
#include "utils/NetworkTrafficCoalescer.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTcpServer>
#include <QUrl>

#include <deque>
#include <expected>
#include <functional>
#include <optional>

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
                                  std::function<void(EncryptedHlsIdentifierPreviewResult)> callback) {
        resolveIdentifierPreview(server, password, rootManifestUrl, QString {}, std::move(callback));
    }
    // Listing rows call this for every encrypted package in a directory, so
    // requests are queued with a small concurrency cap and results are cached
    // per URL + revision (size/last-modified fingerprint supplied by the
    // caller; an empty revision disables caching).
    void resolveIdentifierPreview(const ServerConfig& server,
                                  const QString& password,
                                  const QUrl& rootManifestUrl,
                                  const QString& revision,
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
        std::optional<EncryptedHlsTarIndex> containerIndex;
    };

    struct RemoteRangeResult {
        QByteArray bytes;
        QByteArray etag;
        qint64 totalLength { 0 };
    };

    struct PreviewJob final {
        ServerConfig server;
        QString password;
        QUrl url;
        QString revision;
        std::function<void(EncryptedHlsIdentifierPreviewResult)> callback;
        int attempt { 0 }; // transient-failure retries already spent
    };

    struct PreviewCacheEntry final {
        QDateTime expiresAt;
        std::optional<EncryptedHlsIdentifierPreview> value; // successes only; failures are never cached
    };

    void startNextPreviewJobs();
    void runPreviewJob(PreviewJob job);
    void resolvePreviewFromFullManifest(const ServerConfig& server,
                                        const QString& password,
                                        const QUrl& rootManifestUrl,
                                        std::function<void(EncryptedHlsIdentifierPreviewResult)> done);
    void resolvePreviewFromContainerHead(const ServerConfig& server,
                                         const QString& password,
                                         const QUrl& containerUrl,
                                         std::function<void(EncryptedHlsIdentifierPreviewResult)> done);
    std::optional<EncryptedHlsIdentifierPreviewResult> cachedPreviewResult(const QUrl& url,
                                                                           const QString& revision) const;
    void cachePreviewResult(const QUrl& url,
                            const QString& revision,
                            const EncryptedHlsIdentifierPreviewResult& result);
    // Source names for the manifest-head path, resolved locally: a head
    // window never yields a root digest, so identifier -> name comes from the
    // TSSL store instead of the manifest. Backed by a lazily built index;
    // misses rescan once so packages imported during the session appear.
    const QHash<QByteArray, QString>& sourceFileNameIndex(bool forceRefresh = false);

    struct Session {
        bool localSource { false };
        bool containerSource { false };
        ServerConfig server;
        QString password;
        QUrl remoteDirectoryUrl;
        QString localDirectoryPath;
        QString localManifestName;
        QString localContainerPath;
        QUrl remoteContainerUrl;
        QByteArray remoteContainerEtag;
        std::optional<EncryptedHlsTarIndex> containerIndex;
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
                                    const QByteArray& range,
                                    std::function<void(std::expected<QByteArray, QString>)> callback);
    QNetworkReply* fetchRemoteRange(const ServerConfig& server,
                                    const QString& password,
                                    const QUrl& url,
                                    qint64 start,
                                    qint64 end,
                                    qint64 maximumBytes,
                                    const QByteArray& expectedEtag,
                                    std::function<void(std::expected<RemoteRangeResult, QString>)> callback);
    void fetchLocalBytes(const QString& path,
                         qint64 maximumBytes,
                         std::function<void(std::expected<QByteArray, QString>)> callback);
    QNetworkReply* fetchSessionBytes(const Session& session,
                                     const QUrl& sourceUrl,
                                     const QString& relativePath,
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
    NetworkTrafficCoalescer m_traffic;
    QHash<QString, Session> m_sessions;
    std::deque<PreviewJob> m_previewQueue;
    int m_activePreviewJobs { 0 };
    struct PreviewCacheKey {
        QUrl url;
        QString revision;
    };
    QHash<QUrl, QPair<QString, PreviewCacheEntry>> m_previewCache;
    std::optional<QHash<QByteArray, QString>> m_sourceFileNameIndex;
};
