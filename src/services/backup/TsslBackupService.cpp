#include "services/backup/TsslBackupService.h"

#include <QAuthenticator>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDomDocument>
#include <QDomElement>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageAuthenticationCode>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSslError>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>

namespace {
constexpr auto requestTimeoutMs = 60'000;
constexpr qint64 maxPackageBytes = 256LL * 1024LL * 1024LL;

QByteArray hmac(const QByteArray& key, const QByteArray& data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

QByteArray hexHash(QByteArrayView value)
{
    return QCryptographicHash::hash(QByteArray(value.data(), value.size()), QCryptographicHash::Sha256).toHex();
}

QString encodedSegment(const QString& value)
{
    return value;
}

QUrl appendPath(const QUrl& base, const QStringList& segments)
{
    auto url = base;
    QStringList pathSegments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const auto& segment : segments) {
        if (!segment.isEmpty()) {
            pathSegments.append(encodedSegment(segment));
        }
    }
    url.setPath(QStringLiteral("/") + pathSegments.join(QLatin1Char('/')), QUrl::DecodedMode);
    return url;
}

QString cleanPrefix(QString prefix)
{
    prefix = prefix.trimmed();
    while (prefix.startsWith(QLatin1Char('/'))) prefix.remove(0, 1);
    while (prefix.endsWith(QLatin1Char('/'))) prefix.chop(1);
    return prefix;
}

QString localName(const QDomElement& element)
{
    const auto name = element.localName();
    return name.isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : name;
}

void collectElementsByLocalName(const QDomNode& parent,
                                const QString& wanted,
                                QList<QDomElement>& elements)
{
    for (auto node = parent.firstChild(); !node.isNull(); node = node.nextSibling()) {
        const auto element = node.toElement();
        if (element.isNull()) continue;
        if (localName(element).compare(wanted, Qt::CaseInsensitive) == 0) {
            elements.append(element);
        }
        collectElementsByLocalName(element, wanted, elements);
    }
}

QUrl resolvedUrl(const QUrl& baseUrl, const QString& href)
{
    const auto hrefUrl = QUrl::fromEncoded(href.toUtf8());
    return hrefUrl.isRelative() ? baseUrl.resolved(hrefUrl) : hrefUrl;
}

QUrl webDavFolder(const TsslBackupTarget& target)
{
    QStringList path;
    for (const auto& segment : target.webDavPath.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        path.append(segment);
    }
    auto url = appendPath(QUrl(target.webDavServer.baseUrl), path);
    auto urlPath = url.path(QUrl::FullyDecoded);
    if (!urlPath.endsWith(QLatin1Char('/'))) {
        urlPath += QLatin1Char('/');
        url.setPath(urlPath, QUrl::DecodedMode);
    }
    return url;
}

QString webDavFileName(const QUrl& url)
{
    auto path = url.path(QUrl::FullyDecoded);
    if (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    return path.section(QLatin1Char('/'), -1);
}

QStringList parseWebDavFiles(const QByteArray& body, const QUrl& folderUrl)
{
    QDomDocument document;
    if (!document.setContent(body)) {
        return {};
    }

    auto normalizedFolder = folderUrl.path(QUrl::FullyDecoded);
    while (normalizedFolder.endsWith(QLatin1Char('/')) && normalizedFolder.size() > 1) {
        normalizedFolder.chop(1);
    }

    QStringList files;
    QList<QDomElement> responses;
    collectElementsByLocalName(document, QStringLiteral("response"), responses);
    for (const auto& response : responses) {

        QString href;
        QDomElement propstat;
        for (auto child = response.firstChild(); !child.isNull(); child = child.nextSibling()) {
            const auto element = child.toElement();
            if (element.isNull()) continue;
            const auto name = localName(element);
            if (name.compare(QStringLiteral("href"), Qt::CaseInsensitive) == 0) {
                href = element.text();
            } else if (name.compare(QStringLiteral("propstat"), Qt::CaseInsensitive) == 0) {
                QString status;
                for (auto propChild = element.firstChild(); !propChild.isNull(); propChild = propChild.nextSibling()) {
                    const auto propElement = propChild.toElement();
                    if (!propElement.isNull() && localName(propElement).compare(QStringLiteral("status"), Qt::CaseInsensitive) == 0) {
                        status = propElement.text();
                        break;
                    }
                }
                if (status.contains(QStringLiteral(" 200 ")) || status.endsWith(QStringLiteral(" 200 OK"))) {
                    propstat = element;
                }
            }
        }
        if (href.isEmpty() || propstat.isNull()) continue;

        const auto itemUrl = resolvedUrl(folderUrl, href);
        auto itemPath = itemUrl.path(QUrl::FullyDecoded);
        while (itemPath.endsWith(QLatin1Char('/')) && itemPath.size() > 1) {
            itemPath.chop(1);
        }
        if (itemPath == normalizedFolder) continue;

        bool directory = false;
        QString displayName;
        QDomElement prop;
        for (auto child = propstat.firstChild(); !child.isNull(); child = child.nextSibling()) {
            const auto element = child.toElement();
            if (element.isNull()) continue;
            if (localName(element).compare(QStringLiteral("prop"), Qt::CaseInsensitive) == 0) {
                prop = element;
                break;
            }
        }
        for (auto child = prop.firstChild(); !child.isNull(); child = child.nextSibling()) {
            const auto element = child.toElement();
            if (element.isNull()) continue;
            const auto name = localName(element);
            if (name.compare(QStringLiteral("displayname"), Qt::CaseInsensitive) == 0) {
                displayName = element.text().trimmed();
            } else if (name.compare(QStringLiteral("resourcetype"), Qt::CaseInsensitive) == 0) {
                for (auto typeChild = element.firstChild(); !typeChild.isNull(); typeChild = typeChild.nextSibling()) {
                    const auto typeElement = typeChild.toElement();
                    if (!typeElement.isNull() && localName(typeElement).compare(QStringLiteral("collection"), Qt::CaseInsensitive) == 0) {
                        directory = true;
                        break;
                    }
                }
            }
        }
        if (directory) continue;
        const auto name = displayName.isEmpty() ? webDavFileName(itemUrl) : displayName;
        if (name.endsWith(QStringLiteral(".tssl"), Qt::CaseInsensitive)) {
            files.append(itemUrl.toString(QUrl::FullyEncoded));
        }
    }
    return files;
}

QStringList parseS3Files(const QByteArray& body, bool* truncated, QString* nextToken)
{
    QDomDocument document;
    if (!document.setContent(body)) {
        return {};
    }

    QStringList files;
    QList<QDomElement> contents;
    collectElementsByLocalName(document, QStringLiteral("Contents"), contents);
    for (const auto& element : contents) {
        for (auto child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
            const auto keyElement = child.toElement();
            if (!keyElement.isNull() && localName(keyElement).compare(QStringLiteral("Key"), Qt::CaseInsensitive) == 0) {
                const auto key = keyElement.text();
                if (key.endsWith(QStringLiteral(".tssl"), Qt::CaseInsensitive)) {
                    files.append(key);
                }
                break;
            }
        }
    }

    if (truncated) *truncated = false;
    if (nextToken) nextToken->clear();
    const auto root = document.documentElement();
    for (auto child = root.firstChild(); !child.isNull(); child = child.nextSibling()) {
        const auto element = child.toElement();
        if (element.isNull()) continue;
        const auto name = localName(element);
        if (name.compare(QStringLiteral("IsTruncated"), Qt::CaseInsensitive) == 0) {
            if (truncated) *truncated = element.text().trimmed().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
        } else if (name.compare(QStringLiteral("NextContinuationToken"), Qt::CaseInsensitive) == 0) {
            if (nextToken) *nextToken = element.text();
        }
    }
    return files;
}

bool validBucket(const QString& bucket)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$"));
    return pattern.match(bucket).hasMatch();
}

QString fileNameFor(const QString& path)
{
    return QFileInfo(path).fileName();
}

QString webDavDestination(const TsslBackupTarget& target, const QString& fileName)
{
    auto folder = webDavFolder(target);
    auto path = folder.path(QUrl::FullyDecoded);
    if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
    path += fileName;
    folder.setPath(path, QUrl::DecodedMode);
    return folder.toString(QUrl::FullyEncoded);
}

QUrl s3ObjectUrl(const TsslBackupTarget& target, const QString& key)
{
    QStringList keySegments;
    const auto prefix = cleanPrefix(target.s3Prefix);
    if (!prefix.isEmpty()) keySegments.append(prefix.split(QLatin1Char('/'), Qt::SkipEmptyParts));
    keySegments.append(key.section(QLatin1Char('/'), -1));
    // A listed key already contains the configured prefix.  Do not prepend it
    // a second time when constructing the download URL.
    if (key.startsWith(prefix + QLatin1Char('/'), Qt::CaseSensitive) || key == prefix) {
        keySegments = key.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    }
    return appendPath(appendPath(target.s3Endpoint, { target.s3Bucket }), keySegments);
}

QNetworkRequest signedS3Request(const TsslBackupTarget& target,
                                const QString& method,
                                const QUrl& url,
                                const QByteArray& payloadHash,
                                const QDateTime& now)
{
    const auto date = now.toString(QStringLiteral("yyyyMMdd"));
    const auto timestamp = now.toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    const auto host = url.host() + (url.port() > 0 ? QStringLiteral(":%1").arg(url.port()) : QString());
    const auto canonicalHeaders = QStringLiteral("host:%1\nx-amz-content-sha256:%2\nx-amz-date:%3\n")
        .arg(host, QString::fromLatin1(payloadHash), timestamp);
    const auto signedHeaders = QStringLiteral("host;x-amz-content-sha256;x-amz-date");
    auto queryItems = QUrlQuery(url).queryItems(QUrl::FullyEncoded);
    std::ranges::sort(queryItems, [](const auto& left, const auto& right) {
        if (left.first == right.first) return left.second < right.second;
        return left.first < right.first;
    });
    QStringList canonicalQueryItems;
    canonicalQueryItems.reserve(queryItems.size());
    for (const auto& [key, value] : queryItems) {
        canonicalQueryItems.append(key + QLatin1Char('=') + value);
    }
    const auto canonicalQuery = canonicalQueryItems.join(QLatin1Char('&'));
    const auto canonicalRequest = QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6")
        .arg(method,
             url.path(QUrl::FullyEncoded),
             canonicalQuery,
             canonicalHeaders,
             signedHeaders,
             QString::fromLatin1(payloadHash));
    const auto credentialScope = QStringLiteral("%1/%2/%3/aws4_request")
        .arg(date, target.s3Region, QStringLiteral("s3"));
    const auto stringToSign = QStringLiteral("AWS4-HMAC-SHA256\n%1\n%2\n%3")
        .arg(timestamp, credentialScope, QString::fromLatin1(hexHash(canonicalRequest.toUtf8())));
    const auto dateKey = hmac(QByteArrayLiteral("AWS4") + target.s3SecretKey.toUtf8(), date.toUtf8());
    const auto regionKey = hmac(dateKey, target.s3Region.toUtf8());
    const auto serviceKey = hmac(regionKey, QByteArrayLiteral("s3"));
    const auto signingKey = hmac(serviceKey, QByteArrayLiteral("aws4_request"));
    const auto signature = QString::fromLatin1(hmac(signingKey, stringToSign.toUtf8()).toHex());

    QNetworkRequest request(url);
    request.setRawHeader("Host", host.toUtf8());
    request.setRawHeader("x-amz-content-sha256", payloadHash);
    request.setRawHeader("x-amz-date", timestamp.toUtf8());
    request.setRawHeader("Authorization", QStringLiteral("AWS4-HMAC-SHA256 Credential=%1/%2, SignedHeaders=%3, Signature=%4")
        .arg(target.s3AccessKey, credentialScope, signedHeaders, signature).toUtf8());
    return request;
}
}

TsslBackupService::TsslBackupService(QObject* parent)
    : QObject(parent)
{
    connect(&m_manager, &QNetworkAccessManager::authenticationRequired, this,
            [](QNetworkReply* reply, QAuthenticator* authenticator) {
                authenticator->setUser(reply->property("webdavUsername").toString());
                authenticator->setPassword(reply->property("webdavPassword").toString());
            });
}

bool TsslBackupService::isRunning() const
{
    return m_running;
}

void TsslBackupService::backup(const TsslBackupTarget& target,
                               QStringList localFiles,
                               std::function<void(TsslBackupResult)> callback)
{
    if (m_running) {
        callback(std::unexpected(QStringLiteral("A TSSL backup is already running")));
        return;
    }
    if (localFiles.isEmpty()) {
        callback(std::unexpected(QStringLiteral("No valid TSSL packages are available to back up")));
        return;
    }
    if (target.type == TsslBackupTarget::Type::WebDav) {
        if (!target.webDavServer.baseUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) &&
            !target.webDavServer.baseUrl.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
            callback(std::unexpected(QStringLiteral("The WebDAV backup endpoint is invalid")));
            return;
        }
    } else {
        if (!target.s3Endpoint.isValid() ||
            (target.s3Endpoint.scheme() != QStringLiteral("https") && target.s3Endpoint.host() != QStringLiteral("localhost")) ||
            !validBucket(target.s3Bucket) || target.s3Region.trimmed().isEmpty() ||
            target.s3AccessKey.trimmed().isEmpty() || target.s3SecretKey.isEmpty()) {
            callback(std::unexpected(QStringLiteral("Complete the S3 endpoint, bucket, region, and credentials")));
            return;
        }
    }
    m_target = target;
    m_restoreMode = false;
    m_localFiles = std::move(localFiles);
    m_callback = std::move(callback);
    m_nextIndex = 0;
    m_completed = 0;
    m_cancelRequested = false;
    m_running = true;
    emit progressChanged(0, m_localFiles.size());
    uploadNext();
}

void TsslBackupService::restore(const TsslBackupTarget& target,
                                std::function<void(TsslBackupRestoreResult)> callback)
{
    if (m_running) {
        callback(std::unexpected(QStringLiteral("A TSSL backup or restore is already running")));
        return;
    }
    if (target.type == TsslBackupTarget::Type::WebDav) {
        if (!target.webDavServer.baseUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) &&
            !target.webDavServer.baseUrl.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
            callback(std::unexpected(QStringLiteral("The WebDAV backup endpoint is invalid")));
            return;
        }
    } else if (!target.s3Endpoint.isValid() ||
               (target.s3Endpoint.scheme() != QStringLiteral("https") &&
                target.s3Endpoint.host() != QStringLiteral("localhost")) ||
               !validBucket(target.s3Bucket) || target.s3Region.trimmed().isEmpty() ||
               target.s3AccessKey.trimmed().isEmpty() || target.s3SecretKey.isEmpty()) {
        callback(std::unexpected(QStringLiteral("Complete the S3 endpoint, bucket, region, and credentials")));
        return;
    }

    if (!m_restoreDirectory.isValid()) {
        callback(std::unexpected(QStringLiteral("Unable to create a temporary TSSL restore directory")));
        return;
    }
    m_target = target;
    m_restoreCallback = std::move(callback);
    m_remoteFiles.clear();
    m_downloadedFiles.clear();
    m_restoreNextIndex = 0;
    m_restoreCompleted = 0;
    m_cancelRequested = false;
    m_restoreMode = true;
    m_running = true;

    m_restorePath = QDir(m_restoreDirectory.path()).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(m_restorePath)) {
        finishRestore(std::unexpected(QStringLiteral("Unable to create a temporary TSSL restore directory")));
        return;
    }
    listRemoteFiles();
}

void TsslBackupService::cleanupRestoreFiles()
{
    if (m_restoreCleanupPath.isEmpty()) return;
    QDir(m_restoreCleanupPath).removeRecursively();
    m_restoreCleanupPath.clear();
}

void TsslBackupService::cancel()
{
    if (!m_running) return;
    m_cancelRequested = true;
}

void TsslBackupService::listRemoteFiles()
{
    if (!m_running || !m_restoreMode) return;
    if (m_cancelRequested) {
        finishRestore(std::unexpected(QStringLiteral("TSSL restore canceled")));
        return;
    }
    if (m_target.type == TsslBackupTarget::Type::WebDav) {
        listWebDavFiles();
    } else {
        listS3Files();
    }
}

void TsslBackupService::listWebDavFiles()
{
    const auto folderUrl = webDavFolder(m_target);
    QNetworkRequest request(folderUrl);
    request.setRawHeader("Depth", "1");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));
    request.setRawHeader("Authorization", QByteArrayLiteral("Basic ") +
        (m_target.webDavServer.username.toUtf8() + QByteArrayLiteral(":") + m_target.webDavPassword.toUtf8()).toBase64());
    auto* body = new QBuffer(this);
    body->setData(QByteArrayLiteral(R"(<?xml version="1.0" encoding="utf-8" ?>
<D:propfind xmlns:D="DAV:">
  <D:prop>
    <D:displayname/>
    <D:resourcetype/>
    <D:getcontentlength/>
  </D:prop>
</D:propfind>)"));
    body->open(QIODevice::ReadOnly);
    auto* reply = m_manager.sendCustomRequest(request, QByteArrayLiteral("PROPFIND"), body);
    body->setParent(reply);
    reply->setProperty("webdavUsername", m_target.webDavServer.username);
    reply->setProperty("webdavPassword", m_target.webDavPassword);
    wireReply(reply);
    connect(reply, &QNetworkReply::finished, reply, [this, reply, folderUrl]() {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            const auto message = status > 0 ? QStringLiteral("WebDAV backup listing failed with HTTP %1").arg(status)
                                            : reply->errorString();
            reply->deleteLater();
            finishRestore(std::unexpected(message));
            return;
        }
        reply->deleteLater();
        m_remoteFiles = parseWebDavFiles(body, folderUrl);
        restoreNext();
    });
}

void TsslBackupService::listS3Files()
{
    listS3Page();
}

void TsslBackupService::listS3Page(const QString& continuationToken)
{
    auto url = appendPath(m_target.s3Endpoint, { m_target.s3Bucket });
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("list-type"), QStringLiteral("2"));
    const auto prefix = cleanPrefix(m_target.s3Prefix);
    if (!prefix.isEmpty()) {
        query.addQueryItem(QStringLiteral("prefix"), prefix + QLatin1Char('/'));
    }
    if (!continuationToken.isEmpty()) {
        query.addQueryItem(QStringLiteral("continuation-token"), continuationToken);
    }
    url.setQuery(query);
    const auto emptyHash = hexHash(QByteArray());
    auto request = signedS3Request(m_target, QStringLiteral("GET"), url, emptyHash, QDateTime::currentDateTimeUtc());
    auto* reply = m_manager.get(request);
    wireReply(reply);
    connect(reply, &QNetworkReply::finished, reply, [this, reply]() {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            const auto message = status > 0 ? QStringLiteral("S3 backup listing failed with HTTP %1").arg(status)
                                            : reply->errorString();
            reply->deleteLater();
            finishRestore(std::unexpected(message));
            return;
        }
        reply->deleteLater();
        bool truncated = false;
        QString nextToken;
        m_remoteFiles.append(parseS3Files(body, &truncated, &nextToken));
        if (truncated && !nextToken.isEmpty()) {
            listS3Page(nextToken);
        } else {
            restoreNext();
        }
    });
}

void TsslBackupService::restoreNext()
{
    if (!m_running || !m_restoreMode) return;
    if (m_cancelRequested) {
        finishRestore(std::unexpected(QStringLiteral("TSSL restore canceled")));
        return;
    }
    if (m_remoteFiles.isEmpty()) {
        finishRestore(QStringList());
        return;
    }
    if (m_restoreNextIndex >= m_remoteFiles.size()) {
        finishRestore(std::move(m_downloadedFiles));
        return;
    }
    const auto remoteFile = m_remoteFiles.at(m_restoreNextIndex++);
    if (m_target.type == TsslBackupTarget::Type::WebDav) {
        downloadWebDav(QUrl(remoteFile));
    } else {
        downloadS3(remoteFile);
    }
}

void TsslBackupService::downloadWebDav(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArrayLiteral("Basic ") +
        (m_target.webDavServer.username.toUtf8() + QByteArrayLiteral(":") + m_target.webDavPassword.toUtf8()).toBase64());
    auto* reply = m_manager.get(request);
    reply->setProperty("webdavUsername", m_target.webDavServer.username);
    reply->setProperty("webdavPassword", m_target.webDavPassword);
    wireReply(reply);
    connect(reply, &QNetworkReply::finished, reply, [this, reply, url]() {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto payload = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            const auto message = status > 0 ? QStringLiteral("WebDAV TSSL download failed with HTTP %1").arg(status)
                                            : reply->errorString();
            reply->deleteLater();
            finishRestore(std::unexpected(message));
            return;
        }
        reply->deleteLater();
        const auto name = webDavFileName(url);
        if (name.isEmpty() || !name.endsWith(QStringLiteral(".tssl"), Qt::CaseInsensitive) ||
            payload.isEmpty() || payload.size() > maxPackageBytes) {
            finishRestore(std::unexpected(QStringLiteral("The WebDAV TSSL backup file is invalid or too large")));
            return;
        }
        const auto path = QDir(m_restorePath).filePath(QStringLiteral("%1-%2").arg(m_restoreNextIndex - 1).arg(name));
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() || !file.commit()) {
            finishRestore(std::unexpected(QStringLiteral("Unable to save the downloaded WebDAV TSSL backup")));
            return;
        }
        m_downloadedFiles.append(path);
        ++m_restoreCompleted;
        restoreNext();
    });
}

void TsslBackupService::downloadS3(const QString& key)
{
    const auto url = s3ObjectUrl(m_target, key);
    const auto emptyHash = hexHash(QByteArray());
    const auto request = signedS3Request(m_target, QStringLiteral("GET"), url, emptyHash, QDateTime::currentDateTimeUtc());
    auto* reply = m_manager.get(request);
    wireReply(reply);
    connect(reply, &QNetworkReply::finished, reply, [this, reply, key]() {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto payload = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            const auto message = status > 0 ? QStringLiteral("S3 TSSL download failed with HTTP %1").arg(status)
                                            : reply->errorString();
            reply->deleteLater();
            finishRestore(std::unexpected(message));
            return;
        }
        reply->deleteLater();
        const auto name = key.section(QLatin1Char('/'), -1);
        if (name.isEmpty() || !name.endsWith(QStringLiteral(".tssl"), Qt::CaseInsensitive) ||
            payload.isEmpty() || payload.size() > maxPackageBytes) {
            finishRestore(std::unexpected(QStringLiteral("The S3 TSSL backup file is invalid or too large")));
            return;
        }
        const auto path = QDir(m_restorePath).filePath(QStringLiteral("%1-%2").arg(m_restoreNextIndex - 1).arg(name));
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() || !file.commit()) {
            finishRestore(std::unexpected(QStringLiteral("Unable to save the downloaded S3 TSSL backup")));
            return;
        }
        m_downloadedFiles.append(path);
        ++m_restoreCompleted;
        restoreNext();
    });
}

void TsslBackupService::uploadNext()
{
    if (!m_running) return;
    if (m_cancelRequested) {
        finish(std::unexpected(QStringLiteral("TSSL backup canceled")));
        return;
    }
    if (m_nextIndex >= m_localFiles.size()) {
        finish(m_completed);
        return;
    }
    const auto localPath = m_localFiles.at(m_nextIndex++);
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        finish(std::unexpected(QStringLiteral("Unable to read TSSL package: %1").arg(file.errorString())));
        return;
    }
    if (file.size() <= 0 || file.size() > maxPackageBytes) {
        finish(std::unexpected(QStringLiteral("TSSL package size is invalid or exceeds 256 MiB")));
        return;
    }
    const auto payload = file.readAll();
    if (payload.size() != file.size()) {
        finish(std::unexpected(QStringLiteral("Unable to read the complete TSSL package")));
        return;
    }
    if (m_target.type == TsslBackupTarget::Type::WebDav) {
        uploadWebDav(localPath, payload);
    } else {
        uploadS3(localPath, payload);
    }
}

void TsslBackupService::uploadWebDav(const QString& localPath, const QByteArray& payload)
{
    QNetworkRequest request { QUrl(webDavDestination(m_target, fileNameFor(localPath))) };
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    auto* body = new QBuffer(this);
    body->setData(payload);
    body->open(QIODevice::ReadOnly);
    auto* reply = m_manager.put(request, body);
    body->setParent(reply);
    reply->setProperty("webdavUsername", m_target.webDavServer.username);
    reply->setProperty("webdavPassword", m_target.webDavPassword);
    connect(reply, &QNetworkReply::finished, reply, [this, reply]() {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            const auto message = status > 0 ? QStringLiteral("WebDAV upload failed with HTTP %1").arg(status)
                                            : reply->errorString();
            reply->deleteLater();
            finish(std::unexpected(message));
            return;
        }
        reply->deleteLater();
        ++m_completed;
        emit progressChanged(m_completed, m_localFiles.size());
        uploadNext();
    });
    wireReply(reply);
}

void TsslBackupService::uploadS3(const QString& localPath, const QByteArray& payload)
{
    const auto now = QDateTime::currentDateTimeUtc();
    const auto date = now.toString(QStringLiteral("yyyyMMdd"));
    const auto timestamp = now.toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    const auto prefix = cleanPrefix(m_target.s3Prefix);
    QStringList keySegments;
    if (!prefix.isEmpty()) keySegments.append(prefix.split(QLatin1Char('/'), Qt::SkipEmptyParts));
    keySegments.append(fileNameFor(localPath));
    const auto url = appendPath(appendPath(m_target.s3Endpoint, { m_target.s3Bucket }), keySegments);
    const auto payloadHash = QString::fromLatin1(hexHash(payload));
    const auto host = url.host() + (url.port() > 0 ? QStringLiteral(":%1").arg(url.port()) : QString());
    const auto canonicalHeaders = QStringLiteral("host:%1\nx-amz-content-sha256:%2\nx-amz-date:%3\n")
        .arg(host, payloadHash, timestamp);
    const auto signedHeaders = QStringLiteral("host;x-amz-content-sha256;x-amz-date");
    const auto canonicalRequest = QStringLiteral("PUT\n%1\n\n%2\n%3\n%4")
        .arg(url.path(QUrl::FullyEncoded), canonicalHeaders, signedHeaders, payloadHash);
    const auto credentialScope = QStringLiteral("%1/%2/%3/aws4_request").arg(date, m_target.s3Region, QStringLiteral("s3"));
    const auto stringToSign = QStringLiteral("AWS4-HMAC-SHA256\n%1\n%2\n%3")
        .arg(timestamp, credentialScope, QString::fromLatin1(hexHash(canonicalRequest.toUtf8())));
    const auto dateKey = hmac(QByteArrayLiteral("AWS4") + m_target.s3SecretKey.toUtf8(), date.toUtf8());
    const auto regionKey = hmac(dateKey, m_target.s3Region.toUtf8());
    const auto serviceKey = hmac(regionKey, QByteArrayLiteral("s3"));
    const auto signingKey = hmac(serviceKey, QByteArrayLiteral("aws4_request"));
    const auto signature = QString::fromLatin1(hmac(signingKey, stringToSign.toUtf8()).toHex());
    QNetworkRequest request(url);
    request.setRawHeader("Host", host.toUtf8());
    request.setRawHeader("x-amz-content-sha256", payloadHash.toUtf8());
    request.setRawHeader("x-amz-date", timestamp.toUtf8());
    request.setRawHeader("Authorization", QStringLiteral("AWS4-HMAC-SHA256 Credential=%1/%2, SignedHeaders=%3, Signature=%4")
        .arg(m_target.s3AccessKey, credentialScope, signedHeaders, signature).toUtf8());
    auto* body = new QBuffer(this);
    body->setData(payload);
    body->open(QIODevice::ReadOnly);
    auto* reply = m_manager.put(request, body);
    body->setParent(reply);
    connect(reply, &QNetworkReply::finished, reply, [this, reply]() {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            const auto message = status > 0 ? QStringLiteral("S3 upload failed with HTTP %1").arg(status)
                                            : reply->errorString();
            reply->deleteLater();
            finish(std::unexpected(message));
            return;
        }
        reply->deleteLater();
        ++m_completed;
        emit progressChanged(m_completed, m_localFiles.size());
        uploadNext();
    });
    wireReply(reply);
}

void TsslBackupService::wireReply(QNetworkReply* reply)
{
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(requestTimeoutMs);
    connect(reply, &QNetworkReply::sslErrors, reply, [reply, allow = m_target.type == TsslBackupTarget::Type::WebDav
                                                              ? m_target.webDavServer.trustSelfSignedCertificate
                                                              : m_target.trustSelfSignedCertificate](const QList<QSslError>&) {
        if (allow) reply->ignoreSslErrors();
    });
}

void TsslBackupService::finish(TsslBackupResult result)
{
    if (!m_running) return;
    m_running = false;
    m_restoreMode = false;
    m_localFiles.clear();
    auto callback = std::move(m_callback);
    if (callback) callback(std::move(result));
}

void TsslBackupService::finishRestore(TsslBackupRestoreResult result)
{
    if (!m_running || !m_restoreMode) return;
    m_running = false;
    m_restoreMode = false;
    m_remoteFiles.clear();
    m_restoreCleanupPath = m_restorePath;
    m_restorePath.clear();
    auto callback = std::move(m_restoreCallback);
    if (callback) callback(std::move(result));
    m_downloadedFiles.clear();
}
