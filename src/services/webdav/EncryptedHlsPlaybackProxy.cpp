#include "services/webdav/EncryptedHlsPlaybackProxy.h"

#include "services/webdav/AesGcmDecryptor.h"
#include "services/webdav/HlsManifestValidator.h"
#include "utils/AppLogger.h"

#include <QAuthenticator>
#include <QCryptographicHash>
#include <QDir>
#include <QFutureWatcher>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
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
    directory.setQuery({});
    directory.setFragment({});
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
    resolvePackage(server,
                   password,
                   rootManifestUrl,
                   [this, server, password, rootManifestUrl, callback = std::move(callback)](
                       std::expected<ResolvedPackage, QString> resolved) mutable {
        if (!resolved) {
            callback(std::unexpected(resolved.error()));
            return;
        }
        if (!ensureListening()) {
            callback(std::unexpected(QStringLiteral("Unable to start the local encrypted HLS proxy")));
            return;
        }

        const auto localManifestName = localManifestNameFor(rootManifestUrl);
        if (resolved->package.manifestDigests.contains(localManifestName) ||
            resolved->package.segmentKeys.contains(localManifestName) ||
            resolved->package.resourceDigests.contains(localManifestName)) {
            callback(std::unexpected(QStringLiteral("The root manifest name conflicts with a registered package path")));
            return;
        }

        const auto sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_sessions.insert(sessionId, Session {
            .server = server,
            .password = password,
            .remoteDirectoryUrl = remoteDirectoryFor(rootManifestUrl),
            .localManifestName = localManifestName,
            .rootManifest = std::move(resolved->rootManifest),
            .package = std::move(resolved->package),
        });

        QUrl localUrl;
        localUrl.setScheme(QStringLiteral("http"));
        localUrl.setHost(QStringLiteral("127.0.0.1"));
        localUrl.setPort(m_server.serverPort());
        localUrl.setPath(QStringLiteral("/%1/%2").arg(sessionId, localManifestName));
        callback(EncryptedHlsPreparedStream { .url = localUrl, .sessionId = sessionId });
    });
}

void EncryptedHlsPlaybackProxy::resolveRootDigest(const ServerConfig& server,
                                                  const QString& password,
                                                  const QUrl& rootManifestUrl,
                                                  std::function<void(EncryptedHlsDigestResult)> callback)
{
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

void EncryptedHlsPlaybackProxy::revoke(const QString& sessionId)
{
    if (!sessionId.isEmpty()) {
        m_sessions.remove(sessionId);
    }
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
                     [this, callback = std::move(callback)](std::expected<QByteArray, QString> manifest) mutable {
        if (!manifest) {
            callback(std::unexpected(manifest.error()));
            return;
        }
        if (auto validated = HlsManifestValidator::validate(*manifest); !validated) {
            callback(std::unexpected(validated.error()));
            return;
        }

        const auto digest = QCryptographicHash::hash(*manifest, QCryptographicHash::Sha256);
        auto package = m_store.packageForRootDigest(digest);
        if (!package) {
            callback(std::unexpected(package.error()));
            return;
        }
        if (!*package) {
            callback(std::unexpected(QStringLiteral("No matching local TSSL package was found. Restore it and try again.")));
            return;
        }
        auto manifestIdentifier = HlsManifestValidator::extractM3u8sIdentifier(*manifest);
        if (!manifestIdentifier) {
            callback(std::unexpected(manifestIdentifier.error()));
            return;
        }
        if ((**package).identifier != *manifestIdentifier) {
            callback(std::unexpected(QStringLiteral("The M3U8S manifest and local TSSL identifier do not match")));
            return;
        }
        callback(ResolvedPackage {
            .rootManifest = std::move(*manifest),
            .package = std::move(**package),
        });
    });
}

QNetworkReply* EncryptedHlsPlaybackProxy::fetchRemoteBytes(
    const ServerConfig& server,
    const QString& password,
    const QUrl& url,
    qint64 maximumBytes,
    std::function<void(std::expected<QByteArray, QString>)> callback)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT/0.1"));
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("*/*"));
    request.setRawHeader(QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);

    auto* reply = m_manager.get(request);
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
        } else {
            callback(std::move(*buffer));
        }
        reply->deleteLater();
    });
    return reply;
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
    const auto remoteUrl = session->remoteDirectoryUrl.resolved(relativeUrl);
    if (remoteUrl.scheme() != session->remoteDirectoryUrl.scheme() ||
        remoteUrl.host() != session->remoteDirectoryUrl.host() ||
        remoteUrl.port() != session->remoteDirectoryUrl.port() ||
        !remoteUrl.path(QUrl::FullyDecoded).startsWith(session->remoteDirectoryUrl.path(QUrl::FullyDecoded))) {
        writeError(socket, 403);
        return;
    }

    if (session->package.segmentKeys.contains(*relativePath)) {
        serveSegment(socket,
                     sessionId,
                     *session,
                     *relativePath,
                     remoteUrl,
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
                      remoteUrl,
                      method,
                      session->package.manifestDigests.value(*relativePath));
        return;
    }
    if (session->package.resourceDigests.contains(*relativePath)) {
        serveResource(socket,
                      sessionId,
                      *session,
                      *relativePath,
                      remoteUrl,
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
    auto* reply = fetchRemoteBytes(session.server,
                                   session.password,
                                   remoteUrl,
                                   maximumManifestBytes,
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
    connect(socket, &QTcpSocket::disconnected, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
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
    auto* reply = fetchRemoteBytes(session.server,
                                   session.password,
                                   remoteUrl,
                                   maximumEncryptedSegmentBytes,
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
        });
        watcher->setFuture(QtConcurrent::run([encrypted = std::move(*encrypted), key]() {
            return AesGcmDecryptor::decryptTsSegment(encrypted, key);
        }));
    });
    connect(socket, &QTcpSocket::disconnected, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
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
    auto* reply = fetchRemoteBytes(session.server,
                                   session.password,
                                   remoteUrl,
                                   maximumResourceBytes,
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
    connect(socket, &QTcpSocket::disconnected, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
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
