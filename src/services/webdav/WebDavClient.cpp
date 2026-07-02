#include "services/webdav/WebDavClient.h"

#include "utils/AppLogger.h"

#include <QBuffer>
#include <QAuthenticator>
#include <QDomDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QSet>

namespace {
constexpr auto requestTimeoutMs = 30000;

QString localName(const QDomElement& element)
{
    const auto name = element.localName();
    return name.isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : name;
}

QString childText(const QDomElement& parent, const QString& wanted)
{
    for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (!element.isNull() && localName(element).compare(wanted, Qt::CaseInsensitive) == 0) {
            return element.text();
        }
    }
    return {};
}

void collectElementsByLocalName(const QDomNode& parent, const QString& wanted, QList<QDomElement>& elements)
{
    for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (!element.isNull()) {
            if (localName(element).compare(wanted, Qt::CaseInsensitive) == 0) {
                elements.push_back(element);
            }
            collectElementsByLocalName(element, wanted, elements);
        }
    }
}

QDomElement childElement(const QDomElement& parent, const QString& wanted)
{
    for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (!element.isNull() && localName(element).compare(wanted, Qt::CaseInsensitive) == 0) {
            return element;
        }
    }
    return {};
}

bool hasCollection(const QDomElement& prop)
{
    const auto resourceType = childElement(prop, QStringLiteral("resourcetype"));
    for (auto node = resourceType.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (!element.isNull() && localName(element).compare(QStringLiteral("collection"), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString decodedSegmentName(const QUrl& url)
{
    auto path = url.path();
    if (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const auto segment = path.section(QLatin1Char('/'), -1);
    return QUrl::fromPercentEncoding(segment.toUtf8());
}

bool isVideoFile(const QString& name)
{
    const auto suffix = name.section(QLatin1Char('.'), -1).toLower();
    static const QSet<QString> extensions {
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("mov"),
        QStringLiteral("webm"), QStringLiteral("ts"), QStringLiteral("m2ts"), QStringLiteral("flv"),
        QStringLiteral("wmv"), QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("m4v"),
        QStringLiteral("3gp"), QStringLiteral("ogv")
    };
    return extensions.contains(suffix);
}

QUrl resolvedUrl(const QUrl& baseUrl, const QString& href)
{
    const auto hrefUrl = QUrl::fromEncoded(href.toUtf8());
    if (hrefUrl.isRelative()) {
        return baseUrl.resolved(hrefUrl);
    }
    return hrefUrl;
}

QString normalizedPath(QUrl url)
{
    auto path = url.path(QUrl::FullyDecoded);
    if (path.endsWith(QLatin1Char('/')) && path.size() > 1) {
        path.chop(1);
    }
    return path;
}

std::vector<WebDavItem> parseList(const QByteArray& body, const QUrl& baseUrl)
{
    QDomDocument document;
    if (!document.setContent(body)) {
        return {};
    }

    const auto basePath = normalizedPath(baseUrl);
    std::vector<WebDavItem> items;
    QList<QDomElement> responses;
    collectElementsByLocalName(document, QStringLiteral("response"), responses);
    for (const auto& response : responses) {
        const auto href = childText(response, QStringLiteral("href"));
        if (href.isEmpty()) {
            continue;
        }

        auto itemUrl = resolvedUrl(baseUrl, href);
        const auto itemPath = normalizedPath(itemUrl);
        if (itemPath == basePath) {
            continue;
        }

        auto propstat = childElement(response, QStringLiteral("propstat"));
        for (auto node = response.firstChild(); !node.isNull(); node = node.nextSibling()) {
            const auto candidate = node.toElement();
            if (candidate.isNull() || localName(candidate).compare(QStringLiteral("propstat"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            const auto status = childText(candidate, QStringLiteral("status"));
            if (status.contains(QStringLiteral(" 200 ")) || status.endsWith(QStringLiteral(" 200 OK"))) {
                propstat = candidate;
                break;
            }
        }
        const auto prop = childElement(propstat, QStringLiteral("prop"));
        const auto isDirectory = hasCollection(prop);
        auto name = childText(prop, QStringLiteral("displayname")).trimmed();
        if (name.isEmpty()) {
            name = decodedSegmentName(itemUrl);
        }

        WebDavItem item;
        item.name = name;
        item.url = itemUrl;
        item.relativePath = name;
        item.contentType = childText(prop, QStringLiteral("getcontenttype"));
        item.lastModified = childText(prop, QStringLiteral("getlastmodified"));
        item.directory = isDirectory;
        item.playable = !isDirectory && isVideoFile(name);

        bool ok = false;
        const auto size = childText(prop, QStringLiteral("getcontentlength")).toLongLong(&ok);
        item.size = ok ? size : -1;
        items.push_back(std::move(item));
    }

    std::sort(items.begin(), items.end(), [](const WebDavItem& left, const WebDavItem& right) {
        if (left.directory != right.directory) {
            return left.directory;
        }
        return left.name.localeAwareCompare(right.name) < 0;
    });
    return items;
}

QByteArray propfindBody()
{
    return QByteArrayLiteral(R"(<?xml version="1.0" encoding="utf-8" ?>
<D:propfind xmlns:D="DAV:">
  <D:prop>
    <D:displayname/>
    <D:resourcetype/>
    <D:getcontentlength/>
    <D:getcontenttype/>
    <D:getlastmodified/>
  </D:prop>
</D:propfind>)");
}
}

WebDavClient::WebDavClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_manager, &QNetworkAccessManager::authenticationRequired, this, [](QNetworkReply* reply, QAuthenticator* authenticator) {
        const auto username = reply->property("webdavUsername").toString();
        const auto password = reply->property("webdavPassword").toString();
        authenticator->setUser(username);
        authenticator->setPassword(password);
    });
}

void WebDavClient::listDirectory(const ServerConfig& server,
                                 const QString& password,
                                 const QUrl& directoryUrl,
                                 std::function<void(WebDavListResult)> callback)
{
    QNetworkRequest request(directoryUrl);
    configureRequest(request, server);
    request.setRawHeader("Depth", "1");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));

    auto* body = new QBuffer(this);
    body->setData(propfindBody());
    body->open(QIODevice::ReadOnly);
    auto* reply = m_manager.sendCustomRequest(request, QByteArrayLiteral("PROPFIND"), body);
    body->setParent(reply);
    reply->setProperty("webdavUsername", server.username);
    reply->setProperty("webdavPassword", password);
    wireReply(reply, server);

    connect(reply, &QNetworkReply::finished, reply, [this, reply, server, directoryUrl, callback = std::move(callback)]() mutable {
        const auto body = reply->readAll();
        emit networkTrafficSample(server.id, server.name, serviceTypeToString(server.serviceType), body.size(), propfindBody().size());
        if (reply->error() != QNetworkReply::NoError) {
            callback(std::unexpected(WebDavClient::replyError(reply)));
            reply->deleteLater();
            return;
        }
        auto items = parseList(body, directoryUrl);
        if (items.empty()) {
            AppLogger::warning(QStringLiteral("webdav"),
                               QStringLiteral("PROPFIND returned no visible items for %1, bodyBytes=%2")
                                   .arg(directoryUrl.toString(), QString::number(body.size())));
        }
        callback(std::move(items));
        reply->deleteLater();
    });
}

void WebDavClient::statItem(const ServerConfig& server,
                            const QString& password,
                            const QUrl& itemUrl,
                            std::function<void(WebDavItemResult)> callback)
{
    QNetworkRequest request(itemUrl);
    configureRequest(request, server);
    request.setRawHeader("Depth", "0");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));

    auto* body = new QBuffer(this);
    body->setData(propfindBody());
    body->open(QIODevice::ReadOnly);
    auto* reply = m_manager.sendCustomRequest(request, QByteArrayLiteral("PROPFIND"), body);
    body->setParent(reply);
    reply->setProperty("webdavUsername", server.username);
    reply->setProperty("webdavPassword", password);
    wireReply(reply, server);

    connect(reply, &QNetworkReply::finished, reply, [this, reply, server, itemUrl, callback = std::move(callback)]() mutable {
        const auto body = reply->readAll();
        emit networkTrafficSample(server.id, server.name, serviceTypeToString(server.serviceType), body.size(), propfindBody().size());
        if (reply->error() != QNetworkReply::NoError) {
            callback(std::unexpected(WebDavClient::replyError(reply)));
            reply->deleteLater();
            return;
        }
        auto parsed = parseList(body, itemUrl);
        if (parsed.empty()) {
            WebDavItem item;
            item.name = decodedSegmentName(itemUrl);
            item.url = itemUrl;
            item.directory = itemUrl.path().endsWith(QLatin1Char('/'));
            item.playable = !item.directory && isVideoFile(item.name);
            callback(std::move(item));
        } else {
            callback(std::move(parsed.front()));
        }
        reply->deleteLater();
    });
}

void WebDavClient::createDirectory(const ServerConfig& server,
                                   const QString& password,
                                   const QUrl& directoryUrl,
                                   std::function<void(WebDavVoidResult)> callback)
{
    QNetworkRequest request(directoryUrl);
    configureRequest(request, server);
    auto* reply = m_manager.sendCustomRequest(request, QByteArrayLiteral("MKCOL"));
    reply->setProperty("webdavUsername", server.username);
    reply->setProperty("webdavPassword", password);
    wireReply(reply, server);

    connect(reply, &QNetworkReply::finished, reply, [this, reply, server, callback = std::move(callback)]() mutable {
        const auto body = reply->readAll();
        emit networkTrafficSample(server.id, server.name, serviceTypeToString(server.serviceType), body.size(), 0);
        const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && statusCode != 405) {
            callback(std::unexpected(WebDavClient::replyError(reply)));
            reply->deleteLater();
            return;
        }
        callback({});
        reply->deleteLater();
    });
}

void WebDavClient::configureRequest(QNetworkRequest& request, const ServerConfig& server) const
{
    Q_UNUSED(server)
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT/0.1"));
}

void WebDavClient::wireReply(QNetworkReply* reply, const ServerConfig& server)
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

NetworkError WebDavClient::replyError(QNetworkReply* reply)
{
    const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode >= 400) {
        return {
            .kind = NetworkErrorKind::Http,
            .message = QStringLiteral("HTTP %1").arg(statusCode),
            .httpStatus = statusCode,
        };
    }
    const auto timeout = reply->error() == QNetworkReply::OperationCanceledError;
    return {
        .kind = timeout ? NetworkErrorKind::Timeout : NetworkErrorKind::Transport,
        .message = timeout ? QStringLiteral("Network request timed out") : reply->errorString(),
        .httpStatus = statusCode,
    };
}
