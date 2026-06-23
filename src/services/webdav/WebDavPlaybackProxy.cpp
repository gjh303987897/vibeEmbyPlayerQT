#include "services/webdav/WebDavPlaybackProxy.h"

#include "utils/AppLogger.h"

#include <QAuthenticator>
#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

#include <memory>

namespace {
constexpr auto requestTimeoutMs = 30000;

QHash<QByteArray, QByteArray> parseHeaders(const QList<QByteArray>& lines)
{
    QHash<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const auto colon = lines[i].indexOf(':');
        if (colon <= 0) {
            continue;
        }
        const auto name = lines[i].left(colon).trimmed().toLower();
        const auto value = lines[i].mid(colon + 1).trimmed();
        headers.insert(name, value);
    }
    return headers;
}

QString streamIdFromPath(const QByteArray& rawPath)
{
    const auto question = rawPath.indexOf('?');
    const auto pathOnly = question >= 0 ? rawPath.left(question) : rawPath;
    const auto path = QString::fromUtf8(pathOnly);
    const auto normalized = path.startsWith(QLatin1Char('/')) ? path.mid(1) : path;
    return QUrl::fromPercentEncoding(normalized.section(QLatin1Char('/'), 0, 0).toUtf8());
}

void writeSimpleResponse(QTcpSocket* socket, int statusCode, const QByteArray& reason)
{
    const auto body = QByteArray::number(statusCode) + ' ' + reason + '\n';
    socket->write("HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + reason + "\r\n");
    socket->write("Content-Type: text/plain; charset=utf-8\r\n");
    socket->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
    socket->write("Connection: close\r\n\r\n");
    socket->write(body);
    socket->disconnectFromHost();
}
}

WebDavPlaybackProxy::WebDavPlaybackProxy(QObject* parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &WebDavPlaybackProxy::handlePendingConnection);
    connect(&m_manager, &QNetworkAccessManager::authenticationRequired, this, [](QNetworkReply* reply, QAuthenticator* authenticator) {
        authenticator->setUser(reply->property("webdavUsername").toString());
        authenticator->setPassword(reply->property("webdavPassword").toString());
    });
}

QUrl WebDavPlaybackProxy::streamUrlFor(const ServerConfig& server, const QString& password, const QUrl& remoteUrl)
{
    ensureListening();
    if (!m_server.isListening()) {
        return remoteUrl;
    }

    const auto streamId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_streams.insert(streamId, Stream {
        .server = server,
        .password = password,
        .remoteUrl = remoteUrl,
    });

    QUrl localUrl;
    localUrl.setScheme(QStringLiteral("http"));
    localUrl.setHost(QStringLiteral("127.0.0.1"));
    localUrl.setPort(m_server.serverPort());
    localUrl.setPath(QStringLiteral("/%1/stream").arg(streamId));
    return localUrl;
}

void WebDavPlaybackProxy::revoke(const QString& streamId)
{
    if (!streamId.isEmpty()) {
        m_streams.remove(streamId);
    }
}

void WebDavPlaybackProxy::ensureListening()
{
    if (m_server.isListening()) {
        return;
    }
    if (!m_server.listen(QHostAddress::LocalHost, 0)) {
        AppLogger::warning(QStringLiteral("webdav"), QStringLiteral("Unable to start WebDAV playback proxy"));
        return;
    }
    AppLogger::info(QStringLiteral("webdav"), QStringLiteral("WebDAV playback proxy listening on localhost"));
}

void WebDavPlaybackProxy::handlePendingConnection()
{
    while (auto* socket = m_server.nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            if (!buffer->contains("\r\n\r\n")) {
                return;
            }
            const auto request = *buffer;
            buffer->clear();
            handleRequest(socket, request.left(request.indexOf("\r\n\r\n") + 4));
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
}

void WebDavPlaybackProxy::handleRequest(QTcpSocket* socket, const QByteArray& requestBytes)
{
    const auto lines = requestBytes.split('\n');
    if (lines.isEmpty()) {
        writeSimpleResponse(socket, 400, QByteArrayLiteral("Bad Request"));
        return;
    }

    const auto requestLine = lines.front().trimmed().split(' ');
    if (requestLine.size() < 2) {
        writeSimpleResponse(socket, 400, QByteArrayLiteral("Bad Request"));
        return;
    }

    const auto method = requestLine[0].toUpper();
    if (method != "GET" && method != "HEAD") {
        writeSimpleResponse(socket, 405, QByteArrayLiteral("Method Not Allowed"));
        return;
    }

    const auto streamId = streamIdFromPath(requestLine[1]);
    if (!m_streams.contains(streamId)) {
        writeSimpleResponse(socket, 404, QByteArrayLiteral("Not Found"));
        return;
    }

    proxyRemoteRequest(socket, m_streams.value(streamId), method, parseHeaders(lines));
}

void WebDavPlaybackProxy::proxyRemoteRequest(QTcpSocket* socket,
                                             const Stream& stream,
                                             const QByteArray& method,
                                             const QHash<QByteArray, QByteArray>& requestHeaders)
{
    QNetworkRequest request(stream.remoteUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT/0.1"));
    request.setRawHeader("Accept", "*/*");
    if (requestHeaders.contains("range")) {
        request.setRawHeader("Range", requestHeaders.value("range"));
    }

    QNetworkReply* reply = method == "HEAD" ? m_manager.head(request) : m_manager.get(request);
    reply->setProperty("webdavUsername", stream.server.username);
    reply->setProperty("webdavPassword", stream.password);
    wireReply(reply, stream.server);

    auto* headersSent = new bool(false);
    auto sendHeaders = [this, socket, reply, headersSent]() {
        if (*headersSent || socket->state() == QAbstractSocket::UnconnectedState) {
            return;
        }
        *headersSent = true;
        auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode <= 0) {
            statusCode = 200;
        }
        socket->write("HTTP/1.1 " + QByteArray::number(statusCode) + ' ' + responseReason(statusCode) + "\r\n");
        const auto headers = reply->rawHeaderPairs();
        for (const auto& header : headers) {
            const auto lower = header.first.toLower();
            if (lower == "connection" || lower == "transfer-encoding" || lower == "www-authenticate") {
                continue;
            }
            socket->write(header.first + ": " + header.second + "\r\n");
        }
        socket->write("Connection: close\r\n\r\n");
    };

    connect(reply, &QNetworkReply::readyRead, reply, [reply, socket, sendHeaders]() {
        sendHeaders();
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            socket->write(reply->readAll());
        }
    });
    connect(reply, &QNetworkReply::finished, reply, [reply, socket, headersSent, sendHeaders]() {
        sendHeaders();
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            const auto rest = reply->readAll();
            if (!rest.isEmpty()) {
                socket->write(rest);
            }
            socket->disconnectFromHost();
        }
        delete headersSent;
        reply->deleteLater();
    });
    connect(socket, &QTcpSocket::disconnected, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
}

void WebDavPlaybackProxy::wireReply(QNetworkReply* reply, const ServerConfig& server)
{
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply]() {
        reply->abort();
    });
    timeout->start(requestTimeoutMs);

    connect(reply, &QNetworkReply::sslErrors, reply, [reply, allowSelfSigned = server.trustSelfSignedCertificate](const QList<QSslError>&) {
        if (!allowSelfSigned) {
            return;
        }

        reply->ignoreSslErrors();
    });
}

QByteArray WebDavPlaybackProxy::responseReason(int statusCode) const
{
    switch (statusCode) {
    case 200:
        return QByteArrayLiteral("OK");
    case 206:
        return QByteArrayLiteral("Partial Content");
    case 401:
        return QByteArrayLiteral("Unauthorized");
    case 403:
        return QByteArrayLiteral("Forbidden");
    case 404:
        return QByteArrayLiteral("Not Found");
    case 416:
        return QByteArrayLiteral("Range Not Satisfiable");
    case 500:
        return QByteArrayLiteral("Internal Server Error");
    default:
        return QByteArrayLiteral("Proxy Response");
    }
}
