#include "services/backup/TsslBackupService.h"

#include <QAuthenticator>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMessageAuthenticationCode>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslError>
#include <QTimer>

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
    QStringList path;
    for (const auto& segment : target.webDavPath.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        path.append(segment);
    }
    path.append(fileName);
    return appendPath(QUrl(target.webDavServer.baseUrl), path).toString(QUrl::FullyEncoded);
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
    m_localFiles = std::move(localFiles);
    m_callback = std::move(callback);
    m_nextIndex = 0;
    m_completed = 0;
    m_cancelRequested = false;
    m_running = true;
    emit progressChanged(0, m_localFiles.size());
    uploadNext();
}

void TsslBackupService::cancel()
{
    if (!m_running) return;
    m_cancelRequested = true;
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
    m_localFiles.clear();
    auto callback = std::move(m_callback);
    if (callback) callback(std::move(result));
}
