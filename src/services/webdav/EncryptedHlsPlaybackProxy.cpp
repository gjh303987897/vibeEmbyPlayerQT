#include "services/webdav/EncryptedHlsPlaybackProxy.h"

#include "services/webdav/AesGcmDecryptor.h"
#include "services/webdav/HlsManifestValidator.h"
#include "utils/AppLogger.h"

#include <QAuthenticator>
#include <QCryptographicHash>
#include <QDir>
#include <QFutureWatcher>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QTemporaryFile>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <memory>

namespace {
constexpr qint64 requestTimeoutMs = 30000;
constexpr qint64 maximumManifestBytes = 4 * 1024 * 1024;
constexpr qint64 maximumEncryptedSegmentBytes = 512 * 1024 * 1024;
constexpr qint64 maximumResourceBytes = 128 * 1024 * 1024;
constexpr qsizetype maximumRequestHeaderBytes = 64 * 1024;

QHash<QByteArray, QByteArray> parseHeaders(const QList<QByteArray>& lines)
{
    QHash<QByteArray, QByteArray> headers;
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const auto colon = lines.at(index).indexOf(':');
        if (colon <= 0) {
            continue;
        }
        headers.insert(lines.at(index).left(colon).trimmed().toLower(),
                       lines.at(index).mid(colon + 1).trimmed());
    }
    return headers;
}

QByteArray reasonPhrase(int statusCode)
{
    switch (statusCode) {
    case 200: return QByteArrayLiteral("OK");
    case 206: return QByteArrayLiteral("Partial Content");
    case 400: return QByteArrayLiteral("Bad Request");
    case 403: return QByteArrayLiteral("Forbidden");
    case 404: return QByteArrayLiteral("Not Found");
    case 405: return QByteArrayLiteral("Method Not Allowed");
    case 410: return QByteArrayLiteral("Gone");
    case 413: return QByteArrayLiteral("Content Too Large");
    case 416: return QByteArrayLiteral("Range Not Satisfiable");
    case 431: return QByteArrayLiteral("Request Header Fields Too Large");
    case 502: return QByteArrayLiteral("Bad Gateway");
    default: return QByteArrayLiteral("Internal Server Error");
    }
}

void writeError(QTcpSocket* socket, int statusCode)
{
    if (!socket || socket->state() == QAbstractSocket::UnconnectedState) {
        return;
    }
    const auto reason = reasonPhrase(statusCode);
    const auto body = QByteArray::number(statusCode) + ' ' + reason + '\n';
    socket->write("HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + reason + "\r\n");
    socket->write("Content-Type: text/plain; charset=utf-8\r\n");
    socket->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
    socket->write("Cache-Control: no-store\r\nConnection: close\r\n\r\n");
    socket->write(body);
    socket->disconnectFromHost();
}

std::expected<QPair<qint64, qint64>, QString> parseRange(const QByteArray& value, qint64 totalBytes)
{
    static const QRegularExpression pattern(QStringLiteral("^bytes=(\\d*)-(\\d*)$"));
    const auto match = pattern.match(QString::fromLatin1(value));
    if (!match.hasMatch() || totalBytes <= 0 ||
        (match.captured(1).isEmpty() && match.captured(2).isEmpty())) {
        return std::unexpected(QStringLiteral("Invalid byte range"));
    }

    bool startOk = false;
    bool endOk = false;
    qint64 start = match.captured(1).toLongLong(&startOk);
    qint64 end = match.captured(2).toLongLong(&endOk);
    if (match.captured(1).isEmpty()) {
        const auto suffixBytes = end;
        if (!endOk || suffixBytes <= 0) {
            return std::unexpected(QStringLiteral("Invalid byte range"));
        }
        start = std::max<qint64>(0, totalBytes - suffixBytes);
        end = totalBytes - 1;
    } else {
        if (!startOk || start < 0 || start >= totalBytes) {
            return std::unexpected(QStringLiteral("Invalid byte range"));
        }
        if (match.captured(2).isEmpty()) {
            end = totalBytes - 1;
        } else if (!endOk || end < start) {
            return std::unexpected(QStringLiteral("Invalid byte range"));
        } else {
            end = std::min(end, totalBytes - 1);
        }
    }
    return QPair<qint64, qint64> { start, end };
}

void writeVerifiedBytes(QTcpSocket* socket,
                        const QByteArray& contents,
                        const QByteArray& method,
                        const QByteArray& contentType,
                        const QHash<QByteArray, QByteArray>& requestHeaders)
{
    if (!socket || socket->state() == QAbstractSocket::UnconnectedState) {
        return;
    }

    qint64 start = 0;
    qint64 end = contents.size() - 1;
    auto statusCode = 200;
    if (requestHeaders.contains(QByteArrayLiteral("range"))) {
        auto range = parseRange(requestHeaders.value(QByteArrayLiteral("range")), contents.size());
        if (!range) {
            socket->write("HTTP/1.1 416 Range Not Satisfiable\r\n");
            socket->write("Content-Range: bytes */" + QByteArray::number(contents.size()) + "\r\n");
            socket->write("Content-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        start = range->first;
        end = range->second;
        statusCode = 206;
    }

    const auto responseBytes = end >= start ? end - start + 1 : 0;
    socket->write("HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + reasonPhrase(statusCode) + "\r\n");
    socket->write("Content-Type: " + contentType + "\r\n");
    socket->write("Content-Length: " + QByteArray::number(responseBytes) + "\r\n");
    socket->write("Accept-Ranges: bytes\r\nCache-Control: no-store\r\n");
    if (statusCode == 206) {
        socket->write("Content-Range: bytes " + QByteArray::number(start) + '-' + QByteArray::number(end) + '/' +
                      QByteArray::number(contents.size()) + "\r\n");
    }
    socket->write("Connection: close\r\n\r\n");
    if (method != QByteArrayLiteral("HEAD") && responseBytes > 0) {
        socket->write(contents.constData() + start, responseBytes);
    }
    socket->disconnectFromHost();
}

QByteArray contentTypeForPath(const QString& path)
{
    const auto suffix = path.section(QLatin1Char('.'), -1).toLower();
    if (suffix == QStringLiteral("vtt") || suffix == QStringLiteral("webvtt")) {
        return QByteArrayLiteral("text/vtt; charset=utf-8");
    }
    if (suffix == QStringLiteral("aac")) {
        return QByteArrayLiteral("audio/aac");
    }
    if (suffix == QStringLiteral("m4s") || suffix == QStringLiteral("mp4")) {
        return QByteArrayLiteral("video/mp4");
    }
    return QByteArrayLiteral("application/octet-stream");
}

std::expected<QString, QString> normalizedRequestPath(const QString& path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\'))) {
        return std::unexpected(QStringLiteral("Invalid package path"));
    }
    const auto normalized = QDir::cleanPath(path);
    if (normalized != path || normalized == QStringLiteral("..") || normalized.startsWith(QStringLiteral("../")) ||
        QDir::isAbsolutePath(normalized)) {
        return std::unexpected(QStringLiteral("Package path escapes the root"));
    }
    return normalized;
}

QUrl remoteDirectoryFor(const QUrl& manifestUrl)
{
    auto directory = manifestUrl;
    auto path = directory.path(QUrl::FullyDecoded);
    path = path.left(path.lastIndexOf(QLatin1Char('/')) + 1);
    directory.setPath(path);
    directory.setQuery(QString {});
    directory.setFragment(QString {});
    return directory;
}

QString localManifestNameFor(const QUrl& manifestUrl)
{
    auto name = QFileInfo(manifestUrl.path(QUrl::FullyDecoded)).fileName();
    if (name.endsWith(QStringLiteral(".m3u8s"), Qt::CaseInsensitive)) {
        name.chop(1);
    }
    if (name.isEmpty() || !name.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
        return QStringLiteral("manifest.m3u8");
    }
    return name;
}

std::expected<QByteArray, QString> readBoundedLocalFile(const QString& path, qint64 maximumBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Unable to open local encrypted HLS object: %1")
                                   .arg(file.errorString()));
    }
    if (file.size() <= 0 || file.size() > maximumBytes) {
        return std::unexpected(QStringLiteral("Local encrypted HLS object is empty or exceeds its size limit"));
    }
    auto bytes = file.readAll();
    if (bytes.size() != file.size()) {
        return std::unexpected(QStringLiteral("Unable to read the complete local encrypted HLS object"));
    }
    return bytes;
}

std::expected<QUrl, QString> localResourceUrl(const QString& canonicalDirectory,
                                              const QString& relativePath)
{
    const QFileInfo resourceInfo(QDir(canonicalDirectory).filePath(relativePath));
    const auto canonicalPath = QDir::cleanPath(resourceInfo.canonicalFilePath());
    if (canonicalPath.isEmpty() || !resourceInfo.isFile() || !resourceInfo.isReadable()) {
        return std::unexpected(QStringLiteral("Local encrypted HLS resource is unavailable"));
    }
    auto root = QDir::fromNativeSeparators(QDir::cleanPath(canonicalDirectory));
    auto resource = QDir::fromNativeSeparators(canonicalPath);
    if (!root.endsWith(QLatin1Char('/'))) {
        root += QLatin1Char('/');
    }
#ifdef Q_OS_WIN
    const auto insideRoot = resource.startsWith(root, Qt::CaseInsensitive);
#else
    const auto insideRoot = resource.startsWith(root);
#endif
    if (!insideRoot) {
        return std::unexpected(QStringLiteral("Local encrypted HLS resource escapes the package directory"));
    }
    return QUrl::fromLocalFile(canonicalPath);
}
}

EncryptedHlsPlaybackProxy::EncryptedHlsPlaybackProxy(TsslStore& store, QObject* parent)
    : QObject(parent)
    , m_store(store)
{
    connect(&m_server, &QTcpServer::newConnection, this, &EncryptedHlsPlaybackProxy::handlePendingConnection);
    connect(&m_manager,
            &QNetworkAccessManager::authenticationRequired,
            this,
            [](QNetworkReply* reply, QAuthenticator* authenticator) {
                authenticator->setUser(reply->property("webdavUsername").toString());
                authenticator->setPassword(reply->property("webdavPassword").toString());
            });
}

void EncryptedHlsPlaybackProxy::prepareStream(const ServerConfig& server,
                                              const QString& password,
                                              const QUrl& rootManifestUrl,
                                              std::function<void(EncryptedHlsPrepareResult)> callback)
{
    if (rootManifestUrl.path(QUrl::FullyDecoded).endsWith(QStringLiteral(".m3u8sp"), Qt::CaseInsensitive)) {
        constexpr qint64 indexPrefixBytes = 16 * 1024 * 1024 + 512;
        fetchRemoteRange(server, password, rootManifestUrl, 0, indexPrefixBytes - 1,
                         indexPrefixBytes, {},
                         [this, server, password, rootManifestUrl, callback = std::move(callback)](std::expected<RemoteRangeResult, QString> prefix) mutable {
            if (!prefix) { callback(std::unexpected(prefix.error())); return; }
            if (prefix->etag.isEmpty()) {
                callback(std::unexpected(QStringLiteral("Remote M3U8SP object has no stable ETag")));
                return;
            }
            auto index = EncryptedHlsTarContainer::readIndexPrefix(prefix->bytes, prefix->totalLength);
            if (!index) { callback(std::unexpected(index.error())); return; }
            const auto* manifestEntry = index->entry(index->manifestPath);
            if (!manifestEntry) { callback(std::unexpected(QStringLiteral("M3U8SP manifest entry is missing"))); return; }
            const auto etag = prefix->etag;
            fetchRemoteRange(server, password, rootManifestUrl, manifestEntry->dataOffset,
                             manifestEntry->dataOffset + manifestEntry->size - 1,
                             maximumManifestBytes, etag,
                             [this, server, password, rootManifestUrl, index = std::move(*index), etag, callback = std::move(callback)](std::expected<RemoteRangeResult, QString> manifest) mutable {
                if (!manifest) { callback(std::unexpected(manifest.error())); return; }
                auto resolved = resolvePackageBytes(std::move(manifest->bytes), m_store);
                if (!resolved) { callback(std::unexpected(resolved.error())); return; }
                if (resolved->package.version != 4 || resolved->package.containerIndexSha256 != index.sha256 ||
                    resolved->package.containerLength != index.containerLength) {
                    callback(std::unexpected(QStringLiteral("M3U8SP container metadata does not match TSSL"))); return;
                }
                Session session {
                    .containerSource = true,
                    .server = server,
                    .password = password,
                    .remoteDirectoryUrl = remoteDirectoryFor(rootManifestUrl),
                    .localManifestName = localManifestNameFor(rootManifestUrl),
                    .remoteContainerUrl = rootManifestUrl,
                    .remoteContainerEtag = etag,
                    .containerIndex = std::move(index),
                };
                finishPreparingStream(std::move(*resolved), std::move(session), QFileInfo(rootManifestUrl.path(QUrl::FullyDecoded)).fileName(), std::move(callback));
            });
        });
        return;
    }
    resolvePackage(server,
                   password,
                   rootManifestUrl,
                   [this, server, password, rootManifestUrl, callback = std::move(callback)](
                       std::expected<ResolvedPackage, QString> resolved) mutable {
        if (!resolved) {
            callback(std::unexpected(resolved.error()));
            return;
        }
        Session session {
            .server = server,
            .password = password,
            .remoteDirectoryUrl = remoteDirectoryFor(rootManifestUrl),
            .localManifestName = localManifestNameFor(rootManifestUrl),
        };
        finishPreparingStream(std::move(*resolved),
                              std::move(session),
                              QFileInfo(rootManifestUrl.path(QUrl::FullyDecoded)).fileName(),
                              std::move(callback));
    });
}

void EncryptedHlsPlaybackProxy::prepareLocalStream(
    const QString& rootManifestPath,
    std::function<void(EncryptedHlsPrepareResult)> callback)
{
    const QFileInfo manifestInfo(rootManifestPath);
    if (manifestInfo.fileName().endsWith(QStringLiteral(".m3u8sp"), Qt::CaseInsensitive)) {
        const auto archivePath = QDir::cleanPath(manifestInfo.canonicalFilePath());
        const auto storeDirectory = m_store.storageDirectory();
        auto* watcher = new QFutureWatcher<std::expected<ResolvedPackage, QString>>(this);
        connect(watcher, &QFutureWatcherBase::finished, watcher,
                [this, watcher, archivePath, callback = std::move(callback)]() mutable {
            auto resolved = watcher->result(); watcher->deleteLater();
            if (!resolved) { callback(std::unexpected(resolved.error())); return; }
            Session session { .localSource = true, .containerSource = true,
                .localManifestName = QStringLiteral("index.m3u8"), .localContainerPath = archivePath,
                .containerIndex = std::move(resolved->containerIndex) };
            finishPreparingStream(std::move(*resolved), std::move(session), QFileInfo(archivePath).fileName(), std::move(callback));
        });
        watcher->setFuture(QtConcurrent::run([archivePath, storeDirectory]() -> std::expected<ResolvedPackage, QString> {
            auto index = EncryptedHlsTarContainer::readIndex(archivePath);
            if (!index) return std::unexpected(index.error());
            auto manifest = EncryptedHlsTarContainer::readEntry(archivePath, *index, index->manifestPath, maximumManifestBytes);
            if (!manifest) return std::unexpected(manifest.error());
            const TsslStore store(storeDirectory);
            auto resolved = resolvePackageBytes(std::move(*manifest), store);
            if (!resolved) return std::unexpected(resolved.error());
            if (resolved->package.version != 4 || resolved->package.containerIndexSha256 != index->sha256 ||
                resolved->package.containerLength != QFileInfo(archivePath).size()) {
                return std::unexpected(QStringLiteral("M3U8SP container metadata does not match TSSL"));
            }
            resolved->containerIndex = std::move(*index);
            return resolved;
        }));
        return;
    }
    const auto canonicalPath = QDir::cleanPath(manifestInfo.canonicalFilePath());
    if (canonicalPath.isEmpty() || !manifestInfo.isFile() || !manifestInfo.isReadable() ||
        !canonicalPath.endsWith(QStringLiteral(".m3u8s"), Qt::CaseInsensitive)) {
        callback(std::unexpected(QStringLiteral("Choose a readable local M3U8S manifest")));
        return;
    }
    const auto canonicalDirectory = QFileInfo(canonicalPath).dir().canonicalPath();
    if (canonicalDirectory.isEmpty()) {
        callback(std::unexpected(QStringLiteral("The local M3U8S package directory is unavailable")));
        return;
    }

    const auto storeDirectory = m_store.storageDirectory();
    auto* watcher = new QFutureWatcher<std::expected<ResolvedPackage, QString>>(this);
    connect(watcher,
            &QFutureWatcherBase::finished,
            watcher,
            [this, watcher, canonicalPath, canonicalDirectory, callback = std::move(callback)]() mutable {
        auto resolved = watcher->result();
        watcher->deleteLater();
        if (!resolved) {
            callback(std::unexpected(resolved.error()));
            return;
        }
        Session session {
            .localSource = true,
            .localDirectoryPath = canonicalDirectory,
            .localManifestName = localManifestNameFor(QUrl::fromLocalFile(canonicalPath)),
        };
        finishPreparingStream(std::move(*resolved),
                              std::move(session),
                              QFileInfo(canonicalPath).fileName(),
                              std::move(callback));
    });
    watcher->setFuture(QtConcurrent::run([canonicalPath, storeDirectory]() -> std::expected<ResolvedPackage, QString> {
        auto manifest = readBoundedLocalFile(canonicalPath, maximumManifestBytes);
        if (!manifest) {
            return std::unexpected(manifest.error());
        }
        const TsslStore store(storeDirectory);
        return resolvePackageBytes(std::move(*manifest), store);
    }));
}

void EncryptedHlsPlaybackProxy::finishPreparingStream(
    ResolvedPackage resolved,
    Session session,
    QString fallbackDisplayName,
    std::function<void(EncryptedHlsPrepareResult)> callback)
{
    if (!ensureListening()) {
        callback(std::unexpected(QStringLiteral("Unable to start the local encrypted HLS proxy")));
        return;
    }
    if (resolved.package.manifestDigests.contains(session.localManifestName) ||
        resolved.package.segmentKeys.contains(session.localManifestName) ||
        resolved.package.resourceDigests.contains(session.localManifestName)) {
        callback(std::unexpected(QStringLiteral("The root manifest name conflicts with a registered package path")));
        return;
    }

    const auto displayName = resolved.sourceFileName.isEmpty()
        ? std::move(fallbackDisplayName)
        : resolved.sourceFileName;
    session.rootManifest = std::move(resolved.rootManifest);
    session.package = std::move(resolved.package);
    const auto localManifestName = session.localManifestName;
    const auto sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_sessions.insert(sessionId, std::move(session));

    QUrl localUrl;
    localUrl.setScheme(QStringLiteral("http"));
    localUrl.setHost(QStringLiteral("127.0.0.1"));
    localUrl.setPort(m_server.serverPort());
    localUrl.setPath(QStringLiteral("/%1/%2").arg(sessionId, localManifestName));
    callback(EncryptedHlsPreparedStream {
        .url = localUrl,
        .sessionId = sessionId,
        .displayName = displayName,
    });
}

void EncryptedHlsPlaybackProxy::resolveRootDigest(const ServerConfig& server,
                                                  const QString& password,
                                                  const QUrl& rootManifestUrl,
                                                  std::function<void(EncryptedHlsDigestResult)> callback)
{
    if (rootManifestUrl.path(QUrl::FullyDecoded).endsWith(QStringLiteral(".m3u8sp"), Qt::CaseInsensitive)) {
        prepareStream(server, password, rootManifestUrl,
                      [this, callback = std::move(callback)](EncryptedHlsPrepareResult prepared) mutable {
            if (!prepared) { callback(std::unexpected(prepared.error())); return; }
            const auto session = m_sessions.constFind(prepared->sessionId);
            if (session == m_sessions.cend()) { callback(std::unexpected(QStringLiteral("M3U8SP session is unavailable"))); return; }
            const auto digest = session->package.rootManifestDigest;
            revoke(prepared->sessionId);
            callback(digest);
        });
        return;
    }
    resolvePackage(server,
                   password,
                   rootManifestUrl,
                   [callback = std::move(callback)](std::expected<ResolvedPackage, QString> resolved) mutable {
        if (!resolved) {
            callback(std::unexpected(resolved.error()));
            return;
        }
        callback(resolved->package.rootManifestDigest);
    });
}

void EncryptedHlsPlaybackProxy::resolveIdentifierPreview(
    const ServerConfig& server,
    const QString& password,
    const QUrl& rootManifestUrl,
    std::function<void(EncryptedHlsIdentifierPreviewResult)> callback)
{
    if (rootManifestUrl.path(QUrl::FullyDecoded).endsWith(QStringLiteral(".m3u8sp"), Qt::CaseInsensitive)) {
        prepareStream(server, password, rootManifestUrl,
                      [this, callback = std::move(callback)](EncryptedHlsPrepareResult prepared) mutable {
            if (!prepared) { callback(std::unexpected(prepared.error())); return; }
            const auto session = m_sessions.constFind(prepared->sessionId);
            if (session == m_sessions.cend()) { callback(std::unexpected(QStringLiteral("M3U8SP session is unavailable"))); return; }
            EncryptedHlsIdentifierPreview result {
                .identifier = QStringLiteral("%1...%2")
                    .arg(QString::fromLatin1(session->package.identifier.first(16)),
                         QString::fromLatin1(session->package.identifier.last(12))),
                .sourceFileName = prepared->displayName,
            };
            revoke(prepared->sessionId);
            callback(std::move(result));
        });
        return;
    }
    fetchRemoteBytes(server,
                     password,
                     rootManifestUrl,
                     maximumManifestBytes,
                     {},
                     [this, callback = std::move(callback)](std::expected<QByteArray, QString> manifest) mutable {
        if (!manifest) {
            callback(std::unexpected(manifest.error()));
            return;
        }
        auto identifier = HlsManifestValidator::extractM3u8sIdentifier(*manifest);
        if (!identifier) {
            callback(std::unexpected(identifier.error()));
            return;
        }
        EncryptedHlsIdentifierPreview result {
            .identifier = QStringLiteral("%1...%2")
                .arg(QString::fromLatin1(identifier->first(16)),
                     QString::fromLatin1(identifier->last(12))),
        };
        if (auto resolved = resolvePackageBytes(std::move(*manifest), m_store); resolved) {
            result.sourceFileName = std::move(resolved->sourceFileName);
        }
        callback(std::move(result));
    });
}

void EncryptedHlsPlaybackProxy::revoke(const QString& sessionId)
{
    auto session = m_sessions.find(sessionId);
    if (session == m_sessions.end()) {
        return;
    }
    session->package.sourceFileNameKey.fill('\0');
    for (auto key = session->package.segmentKeys.begin(); key != session->package.segmentKeys.end(); ++key) {
        key.value().fill('\0');
    }
    m_sessions.erase(session);
}

void EncryptedHlsPlaybackProxy::resolvePackage(
    const ServerConfig& server,
    const QString& password,
    const QUrl& rootManifestUrl,
    std::function<void(std::expected<ResolvedPackage, QString>)> callback)
{
    fetchRemoteBytes(server,
                     password,
                     rootManifestUrl,
                     maximumManifestBytes,
                     {},
                     [this, callback = std::move(callback)](std::expected<QByteArray, QString> manifest) mutable {
        if (!manifest) {
            callback(std::unexpected(manifest.error()));
            return;
        }
        auto resolved = resolvePackageBytes(std::move(*manifest), m_store);
        callback(std::move(resolved));
    });
}

std::expected<EncryptedHlsPlaybackProxy::ResolvedPackage, QString>
EncryptedHlsPlaybackProxy::resolvePackageBytes(QByteArray manifest, const TsslStore& store)
{
    if (auto validated = HlsManifestValidator::validate(manifest); !validated) {
        return std::unexpected(validated.error());
    }

    const auto digest = QCryptographicHash::hash(manifest, QCryptographicHash::Sha256);
    auto package = store.packageForRootDigest(digest);
    if (!package) {
        return std::unexpected(package.error());
    }
    if (!*package) {
        return std::unexpected(QStringLiteral("No matching local TSSL package was found. Restore it and try again."));
    }
    auto manifestIdentifier = HlsManifestValidator::extractM3u8sIdentifier(manifest);
    if (!manifestIdentifier) {
        return std::unexpected(manifestIdentifier.error());
    }
    if ((**package).identifier != *manifestIdentifier) {
        return std::unexpected(QStringLiteral("The M3U8S manifest and local TSSL identifier do not match"));
    }

    QString sourceFileName;
    if ((**package).version == 3 || (**package).version == 4) {
        auto encryptedSourceFileName = HlsManifestValidator::extractEncryptedSourceFileName(manifest);
        if (!encryptedSourceFileName) {
            return std::unexpected(encryptedSourceFileName.error());
        }
        if (*encryptedSourceFileName != (**package).encryptedSourceFileName) {
            return std::unexpected(QStringLiteral("The M3U8S manifest and local TSSL source filename do not match"));
        }
        auto decryptedSourceFileName = (**package).decryptedSourceFileName();
        if (!decryptedSourceFileName) {
            return std::unexpected(decryptedSourceFileName.error());
        }
        if (!*decryptedSourceFileName) {
            return std::unexpected(QStringLiteral("TSSL v3 does not contain a recoverable source filename"));
        }
        sourceFileName = **decryptedSourceFileName;
    } else if (manifest.contains(QByteArrayLiteral("#M3U8S-SOURCE-NAME:"))) {
        return std::unexpected(QStringLiteral("TSSL v2 cannot authenticate M3U8S source filename metadata"));
    }
    return ResolvedPackage {
        .rootManifest = std::move(manifest),
        .package = std::move(**package),
        .sourceFileName = std::move(sourceFileName),
        .containerIndex = std::nullopt,
    };
}

QNetworkReply* EncryptedHlsPlaybackProxy::fetchRemoteBytes(
    const ServerConfig& server,
    const QString& password,
    const QUrl& url,
    qint64 maximumBytes,
    const QByteArray& range,
    std::function<void(std::expected<QByteArray, QString>)> callback)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT/0.1"));
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("*/*"));
    request.setRawHeader(QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity"));
    if (!range.isEmpty()) request.setRawHeader(QByteArrayLiteral("Range"), range);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);

    auto* reply = m_manager.get(request);
    reply->setProperty("encryptedHlsRange", range);
    reply->setProperty("webdavUsername", server.username);
    reply->setProperty("webdavPassword", password);
    wireReply(reply, server);

    auto buffer = std::make_shared<QByteArray>();
    buffer->reserve(static_cast<qsizetype>(std::min<qint64>(maximumBytes, 1024 * 1024)));
    auto readAvailable = [this, reply, buffer, maximumBytes, server]() {
        const auto chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            emit networkTrafficSample(server.id,
                                      server.name,
                                      serviceTypeToString(server.serviceType),
                                      chunk.size(),
                                      0);
            buffer->append(chunk);
        }
        if (buffer->size() > maximumBytes && reply->isRunning()) {
            reply->setProperty("encryptedHlsTooLarge", true);
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::readyRead, reply, readAvailable);
    connect(reply,
            &QNetworkReply::finished,
            reply,
            [reply, buffer, readAvailable, callback = std::move(callback)]() mutable {
        readAvailable();
        if (reply->property("encryptedHlsTooLarge").toBool()) {
            callback(std::unexpected(QStringLiteral("Remote encrypted HLS object exceeds the configured size limit")));
        } else if (reply->error() != QNetworkReply::NoError) {
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            callback(std::unexpected(status >= 400
                                         ? QStringLiteral("WebDAV returned HTTP %1").arg(status)
                                         : reply->errorString()));
        } else if (!reply->property("encryptedHlsRange").toByteArray().isEmpty() &&
                   reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) {
            callback(std::unexpected(QStringLiteral("Remote M3U8SP server does not support byte ranges")));
        } else {
            callback(std::move(*buffer));
        }
        reply->deleteLater();
    });
    return reply;
}

QNetworkReply* EncryptedHlsPlaybackProxy::fetchRemoteRange(
    const ServerConfig& server,
    const QString& password,
    const QUrl& url,
    qint64 start,
    qint64 end,
    qint64 maximumBytes,
    const QByteArray& expectedEtag,
    std::function<void(std::expected<RemoteRangeResult, QString>)> callback)
{
    if (start < 0 || end < start || maximumBytes < end - start + 1) {
        callback(std::unexpected(QStringLiteral("Invalid M3U8SP byte range")));
        return nullptr;
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT/0.1"));
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("*/*"));
    request.setRawHeader(QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity"));
    request.setRawHeader(QByteArrayLiteral("Range"), QByteArrayLiteral("bytes=") + QByteArray::number(start) + '-' + QByteArray::number(end));
    if (!expectedEtag.isEmpty()) request.setRawHeader(QByteArrayLiteral("If-Match"), expectedEtag);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    auto* reply = m_manager.get(request);
    reply->setProperty("webdavUsername", server.username);
    reply->setProperty("webdavPassword", password);
    wireReply(reply, server);
    auto buffer = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, reply, [this, reply, buffer, maximumBytes, server]() {
        const auto chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            emit networkTrafficSample(server.id, server.name, serviceTypeToString(server.serviceType), chunk.size(), 0);
            buffer->append(chunk);
        }
        if (buffer->size() > maximumBytes && reply->isRunning()) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, reply,
            [reply, buffer, start, end, expectedEtag, callback = std::move(callback)]() mutable {
        const auto chunk = reply->readAll();
        if (!chunk.isEmpty()) buffer->append(chunk);
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            callback(std::unexpected(status == 412 ? QStringLiteral("Remote M3U8SP object changed") : reply->errorString()));
        } else if (status != 206) {
            callback(std::unexpected(QStringLiteral("Remote M3U8SP server does not support byte ranges")));
        } else {
            const auto contentRange = reply->rawHeader(QByteArrayLiteral("Content-Range"));
            static const QRegularExpression pattern(QStringLiteral("^bytes (\\d+)-(\\d+)/(\\d+)$"));
            const auto match = pattern.match(QString::fromLatin1(contentRange));
            bool ok = false;
            const auto responseStart = match.captured(1).toLongLong(&ok);
            const auto responseEnd = match.captured(2).toLongLong(&ok);
            const auto total = match.captured(3).toLongLong(&ok);
            const auto etag = reply->rawHeader(QByteArrayLiteral("ETag"));
            if (!match.hasMatch() || responseStart != start || responseEnd < start || responseEnd > end || total <= responseEnd ||
                buffer->size() != responseEnd - start + 1 || (!expectedEtag.isEmpty() && etag != expectedEtag)) {
                callback(std::unexpected(QStringLiteral("Remote M3U8SP Content-Range or ETag is invalid")));
            } else {
                callback(RemoteRangeResult { std::move(*buffer), etag, total });
            }
        }
        reply->deleteLater();
    });
    return reply;
}

void EncryptedHlsPlaybackProxy::fetchLocalBytes(
    const QString& path,
    qint64 maximumBytes,
    std::function<void(std::expected<QByteArray, QString>)> callback)
{
    auto* watcher = new QFutureWatcher<std::expected<QByteArray, QString>>(this);
    connect(watcher,
            &QFutureWatcherBase::finished,
            watcher,
            [watcher, callback = std::move(callback)]() mutable {
        auto result = watcher->result();
        watcher->deleteLater();
        callback(std::move(result));
    });
    watcher->setFuture(QtConcurrent::run([path, maximumBytes]() {
        return readBoundedLocalFile(path, maximumBytes);
    }));
}

QNetworkReply* EncryptedHlsPlaybackProxy::fetchSessionBytes(
    const Session& session,
    const QUrl& sourceUrl,
    const QString& relativePath,
    qint64 maximumBytes,
    std::function<void(std::expected<QByteArray, QString>)> callback)
{
    if (session.containerSource) {
        const auto* entry = session.containerIndex ? session.containerIndex->entry(relativePath) : nullptr;
        if (!entry || entry->size > maximumBytes) {
            callback(std::unexpected(QStringLiteral("M3U8SP entry is unavailable or too large")));
            return nullptr;
        }
        if (session.localSource) {
            auto result = EncryptedHlsTarContainer::readEntry(session.localContainerPath,
                                                              *session.containerIndex,
                                                              relativePath,
                                                              maximumBytes);
            callback(std::move(result));
            return nullptr;
        }
        return fetchRemoteRange(session.server, session.password, session.remoteContainerUrl,
                                entry->dataOffset, entry->dataOffset + entry->size - 1,
                                maximumBytes, session.remoteContainerEtag,
                                [callback = std::move(callback)](std::expected<RemoteRangeResult, QString> result) mutable {
            if (!result) callback(std::unexpected(result.error()));
            else callback(std::move(result->bytes));
        });
    }
    if (session.localSource) {
        fetchLocalBytes(sourceUrl.toLocalFile(), maximumBytes, std::move(callback));
        return nullptr;
    }
    return fetchRemoteBytes(session.server,
                            session.password,
                            sourceUrl,
                            maximumBytes,
                            {},
                            std::move(callback));
}

bool EncryptedHlsPlaybackProxy::ensureListening()
{
    if (m_server.isListening()) {
        return true;
    }
    if (!m_server.listen(QHostAddress::LocalHost, 0)) {
        AppLogger::warning(QStringLiteral("encrypted-hls"),
                           QStringLiteral("Unable to start encrypted HLS loopback proxy"));
        return false;
    }
    AppLogger::info(QStringLiteral("encrypted-hls"),
                    QStringLiteral("Encrypted HLS proxy listening on localhost"));
    return true;
}

void EncryptedHlsPlaybackProxy::handlePendingConnection()
{
    while (auto* socket = m_server.nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            if (buffer->size() > maximumRequestHeaderBytes) {
                writeError(socket, 431);
                return;
            }
            const auto headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            const auto request = buffer->left(headerEnd + 4);
            buffer->clear();
            handleRequest(socket, request);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

void EncryptedHlsPlaybackProxy::handleRequest(QTcpSocket* socket, const QByteArray& requestBytes)
{
    const auto lines = requestBytes.split('\n');
    if (lines.isEmpty()) {
        writeError(socket, 400);
        return;
    }
    const auto requestLine = lines.front().trimmed().split(' ');
    if (requestLine.size() < 2) {
        writeError(socket, 400);
        return;
    }
    const auto method = requestLine.at(0).toUpper();
    if (method != QByteArrayLiteral("GET") && method != QByteArrayLiteral("HEAD")) {
        writeError(socket, 405);
        return;
    }

    const auto requestUrl = QUrl::fromEncoded(requestLine.at(1), QUrl::StrictMode);
    const auto path = requestUrl.path(QUrl::FullyDecoded);
    const auto sessionId = path.section(QLatin1Char('/'), 1, 1);
    const auto rawRelativePath = path.section(QLatin1Char('/'), 2);
    const auto session = m_sessions.constFind(sessionId);
    if (!requestUrl.isValid() || sessionId.isEmpty() || session == m_sessions.cend()) {
        writeError(socket, 404);
        return;
    }
    const auto headers = parseHeaders(lines);

    if (rawRelativePath == session->localManifestName) {
        writeVerifiedBytes(socket,
                           session->rootManifest,
                           method,
                           QByteArrayLiteral("application/vnd.apple.mpegurl; charset=utf-8"),
                           headers);
        return;
    }
    auto relativePath = normalizedRequestPath(rawRelativePath);
    if (!relativePath) {
        writeError(socket, 403);
        return;
    }

    QUrl relativeUrl;
    relativeUrl.setPath(*relativePath);
    relativeUrl.setQuery(requestUrl.query(QUrl::FullyDecoded));
    QUrl sourceUrl;
    if (session->containerSource) {
        sourceUrl = session->remoteContainerUrl;
    } else if (session->localSource) {
        auto localUrl = localResourceUrl(session->localDirectoryPath, *relativePath);
        if (!localUrl) {
            writeError(socket, 403);
            return;
        }
        sourceUrl = std::move(*localUrl);
    } else {
        sourceUrl = session->remoteDirectoryUrl.resolved(relativeUrl);
        if (sourceUrl.scheme() != session->remoteDirectoryUrl.scheme() ||
            sourceUrl.host() != session->remoteDirectoryUrl.host() ||
            sourceUrl.port() != session->remoteDirectoryUrl.port() ||
            !sourceUrl.path(QUrl::FullyDecoded).startsWith(
                session->remoteDirectoryUrl.path(QUrl::FullyDecoded))) {
            writeError(socket, 403);
            return;
        }
    }

    if (session->package.segmentKeys.contains(*relativePath)) {
        serveSegment(socket,
                     sessionId,
                     *session,
                     *relativePath,
                     sourceUrl,
                     method,
                     headers,
                     session->package.segmentKeys.value(*relativePath));
        return;
    }
    if (session->package.manifestDigests.contains(*relativePath)) {
        serveManifest(socket,
                      sessionId,
                      *session,
                      *relativePath,
                      sourceUrl,
                      method,
                      session->package.manifestDigests.value(*relativePath));
        return;
    }
    if (session->package.resourceDigests.contains(*relativePath)) {
        serveResource(socket,
                      sessionId,
                      *session,
                      *relativePath,
                      sourceUrl,
                      method,
                      headers,
                      session->package.resourceDigests.value(*relativePath));
        return;
    }
    writeError(socket, 404);
}

void EncryptedHlsPlaybackProxy::serveManifest(QTcpSocket* socket,
                                              const QString& sessionId,
                                              const Session& session,
                                              const QString& relativePath,
                                              const QUrl& remoteUrl,
                                              const QByteArray& method,
                                              const QByteArray& expectedDigest)
{
    const QPointer<QTcpSocket> guardedSocket(socket);
    auto* reply = fetchSessionBytes(session,
                                   remoteUrl,
                                   relativePath, maximumManifestBytes,
                                   [this, guardedSocket, sessionId, relativePath, method, expectedDigest](
                                       std::expected<QByteArray, QString> manifest) {
        if (!guardedSocket || !m_sessions.contains(sessionId)) {
            return;
        }
        if (!manifest || QCryptographicHash::hash(*manifest, QCryptographicHash::Sha256) != expectedDigest ||
            !HlsManifestValidator::validate(*manifest, relativePath)) {
            AppLogger::warning(QStringLiteral("encrypted-hls"),
                               QStringLiteral("Rejected an invalid or untrusted child manifest"));
            writeError(guardedSocket, 502);
            return;
        }
        writeVerifiedBytes(guardedSocket,
                           *manifest,
                           method,
                           QByteArrayLiteral("application/vnd.apple.mpegurl; charset=utf-8"),
                           {});
    });
    if (reply) {
        connect(socket, &QTcpSocket::disconnected, reply, [reply]() {
            if (reply->isRunning()) {
                reply->abort();
            }
        });
    }
}

void EncryptedHlsPlaybackProxy::serveSegment(QTcpSocket* socket,
                                             const QString& sessionId,
                                             const Session& session,
                                             const QString& relativePath,
                                             const QUrl& remoteUrl,
                                             const QByteArray& method,
                                             const QHash<QByteArray, QByteArray>& requestHeaders,
                                             const QByteArray& key)
{
    const QPointer<QTcpSocket> guardedSocket(socket);
    auto* reply = fetchSessionBytes(session,
                                   remoteUrl,
                                   relativePath, maximumEncryptedSegmentBytes,
                                   [this, guardedSocket, sessionId, relativePath, method, requestHeaders, key](
                                       std::expected<QByteArray, QString> encrypted) {
        if (!guardedSocket || !m_sessions.contains(sessionId)) {
            return;
        }
        if (!encrypted) {
            AppLogger::warning(QStringLiteral("encrypted-hls"),
                               QStringLiteral("Unable to download encrypted TS segment"));
            writeError(guardedSocket, 502);
            return;
        }

        auto* watcher = new QFutureWatcher<std::expected<QByteArray, QString>>(this);
        connect(watcher,
                &QFutureWatcherBase::finished,
                watcher,
                [this, watcher, guardedSocket, sessionId, relativePath, method, requestHeaders]() {
            auto plaintext = watcher->result();
            watcher->deleteLater();
            if (!guardedSocket || !m_sessions.contains(sessionId)) {
                return;
            }
            if (!plaintext) {
                AppLogger::warning(QStringLiteral("encrypted-hls"),
                                   QStringLiteral("Rejected TS segment because GCM authentication failed: %1")
                                       .arg(relativePath));
                writeError(guardedSocket, 502);
                return;
            }
            writeVerifiedBytes(guardedSocket,
                               *plaintext,
                               method,
                               QByteArrayLiteral("video/mp2t"),
                               requestHeaders);
            plaintext->fill('\0');
        });
        watcher->setFuture(QtConcurrent::run(
            [encrypted = std::move(*encrypted), segmentKey = QByteArray(key)]() mutable {
            auto plaintext = AesGcmDecryptor::decryptTsSegment(encrypted, segmentKey);
            segmentKey.fill('\0');
            return plaintext;
        }));
    });
    if (reply) {
        connect(socket, &QTcpSocket::disconnected, reply, [reply]() {
            if (reply->isRunning()) {
                reply->abort();
            }
        });
    }
}

void EncryptedHlsPlaybackProxy::serveResource(QTcpSocket* socket,
                                              const QString& sessionId,
                                              const Session& session,
                                              const QString& relativePath,
                                              const QUrl& remoteUrl,
                                              const QByteArray& method,
                                              const QHash<QByteArray, QByteArray>& requestHeaders,
                                              const QByteArray& expectedDigest)
{
    const QPointer<QTcpSocket> guardedSocket(socket);
    auto* reply = fetchSessionBytes(session,
                                   remoteUrl,
                                   relativePath, maximumResourceBytes,
                                   [this, guardedSocket, sessionId, relativePath, method, requestHeaders, expectedDigest](
                                       std::expected<QByteArray, QString> resource) {
        if (!guardedSocket || !m_sessions.contains(sessionId)) {
            return;
        }
        if (!resource || QCryptographicHash::hash(*resource, QCryptographicHash::Sha256) != expectedDigest) {
            AppLogger::warning(QStringLiteral("encrypted-hls"),
                               QStringLiteral("Rejected an auxiliary HLS resource with a digest mismatch"));
            writeError(guardedSocket, 502);
            return;
        }
        writeVerifiedBytes(guardedSocket,
                           *resource,
                           method,
                           contentTypeForPath(relativePath),
                           requestHeaders);
    });
    if (reply) {
        connect(socket, &QTcpSocket::disconnected, reply, [reply]() {
            if (reply->isRunning()) {
                reply->abort();
            }
        });
    }
}

void EncryptedHlsPlaybackProxy::wireReply(QNetworkReply* reply, const ServerConfig& server)
{
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply]() {
        reply->abort();
    });
    timeout->start(requestTimeoutMs);

    connect(reply,
            &QNetworkReply::sslErrors,
            reply,
            [reply, allowSelfSigned = server.trustSelfSignedCertificate](const QList<QSslError>&) {
        if (allowSelfSigned) {
            reply->ignoreSslErrors();
        }
    });
}
