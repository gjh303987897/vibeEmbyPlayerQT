#include "services/backup/S3CredentialProvider.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QSettings>
#include <QTimer>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QXmlStreamReader>

std::expected<S3Credentials,QString> S3CredentialProvider::load(const QString& provider, const QString& profile,
                                                             std::atomic_bool* cancel)
{
    S3Credentials result;
    if (provider == "env") {
        result = {qEnvironmentVariable("AWS_ACCESS_KEY_ID"),qEnvironmentVariable("AWS_SECRET_ACCESS_KEY"),
                  qEnvironmentVariable("AWS_SESSION_TOKEN")};
    } else if (provider == "profile") {
        const auto path = qEnvironmentVariable("AWS_SHARED_CREDENTIALS_FILE",
                                               QDir::home().filePath(".aws/credentials"));
        QSettings settings(path,QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        settings.beginGroup(profile.isEmpty() ? qEnvironmentVariable("AWS_PROFILE","default") : profile);
        result = {settings.value("aws_access_key_id").toString(),settings.value("aws_secret_access_key").toString(),
                  settings.value("aws_session_token").toString()};
    } else if (provider == "instance") {
        QNetworkAccessManager manager;
        manager.setProxy(QNetworkProxy::NoProxy);
        auto request = [&](const QString& suffix, const QByteArray& token, bool put = false)
            -> std::expected<QByteArray,QString> {
            QNetworkRequest request(QUrl("http://169.254.169.254/latest/" + suffix));
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
            request.setTransferTimeout(2000);
            if (put) request.setRawHeader("X-aws-ec2-metadata-token-ttl-seconds","60");
            else request.setRawHeader("X-aws-ec2-metadata-token",token);
            auto* reply = put ? manager.put(request,QByteArray()) : manager.get(request);
            QEventLoop loop;
            QTimer deadline;
            QTimer cancellation;
            QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
            QObject::connect(&deadline,&QTimer::timeout,reply,&QNetworkReply::abort);
            QObject::connect(&cancellation,&QTimer::timeout,reply,[reply,cancel] {
                if (cancel && cancel->load()) reply->abort();
            });
            cancellation.start(50);
            deadline.start(2500);
            loop.exec();
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto body = reply->isOpen() ? reply->readAll() : QByteArray(); const auto ok = reply->error() == QNetworkReply::NoError && status == 200;
            delete reply;
            return ok ? std::expected<QByteArray,QString>(body)
                      : std::unexpected(QStringLiteral("EC2 IMDSv2 credential request failed"));
        };
        auto token = request("api/token",{},true);
        if (!token) return std::unexpected(token.error());
        auto role = request("meta-data/iam/security-credentials/",*token);
        if (!role) return std::unexpected(role.error());
        const auto name = QString::fromUtf8(*role).trimmed();
        if (name.isEmpty() || name.contains('/') || name.contains('\n'))
            return std::unexpected(QStringLiteral("Invalid EC2 instance role"));
        auto credentials = request("meta-data/iam/security-credentials/" + name,*token);
        if (!credentials) return std::unexpected(credentials.error());
        const auto object = QJsonDocument::fromJson(*credentials).object();
        const auto expiration = QDateTime::fromString(object.value("Expiration").toString(),Qt::ISODate);
        if (object.value("Code") != "Success" ||
            !expiration.isValid() || expiration <= QDateTime::currentDateTimeUtc())
            return std::unexpected(QStringLiteral("EC2 role credentials are unavailable or expired"));
        result = {object.value("AccessKeyId").toString(),object.value("SecretAccessKey").toString(),
                  object.value("Token").toString()};
    } else return std::unexpected(QStringLiteral("S3 credential provider must be env, profile or instance"));
    if (result.accessKey.isEmpty() || result.secretKey.isEmpty())
        return std::unexpected(QStringLiteral("The selected S3 credential provider has no complete credentials"));
    for (const auto& field : {result.accessKey,result.secretKey,result.sessionToken})
        if (field.contains('\n') || field.contains('\r')) return std::unexpected(QStringLiteral("Invalid S3 credential value"));
    return result;
}

std::expected<S3Credentials,QString> S3CredentialProvider::assumeRole(const S3Credentials& source,
    const QString& roleArn, const QString& region, const QString& sessionName, const QString& externalId,
    std::atomic_bool* cancel, QUrl endpoint)
{
    static const QRegularExpression arn("^[a-zA-Z0-9:/_+=,.@-]{20,2048}$");
    static const QRegularExpression name("^[a-zA-Z0-9_+=,.@-]{2,64}$");
    static const QRegularExpression regionPattern("^[a-z0-9-]+$");
    if (!arn.match(roleArn).hasMatch() || !roleArn.startsWith("arn:") || !roleArn.contains(":role/") ||
        !name.match(sessionName).hasMatch() || !regionPattern.match(region).hasMatch() ||
        source.accessKey.isEmpty() || source.secretKey.isEmpty())
        return std::unexpected(QStringLiteral("Invalid STS role, session name, region or source credentials"));
    for (const auto& value : {source.accessKey,source.secretKey,source.sessionToken})
        if (value.contains('\n') || value.contains('\r')) return std::unexpected(QStringLiteral("Invalid source credentials"));
    if (endpoint.isEmpty()) endpoint = QUrl("https://sts." + region + ".amazonaws.com" +
                                           (roleArn.startsWith("arn:aws-cn:") ? ".cn" : "") + '/');
    if (!endpoint.isValid() || !endpoint.userInfo().isEmpty() || endpoint.hasQuery() ||
        (endpoint.scheme() != "https" && !(endpoint.scheme() == "http" && endpoint.host() == "127.0.0.1")))
        return std::unexpected(QStringLiteral("Invalid STS endpoint"));
    QUrlQuery query;
    query.addQueryItem("Action","AssumeRole"); query.addQueryItem("Version","2011-06-15");
    query.addQueryItem("RoleArn",roleArn); query.addQueryItem("RoleSessionName",sessionName);
    if (!externalId.isEmpty()) query.addQueryItem("ExternalId",externalId);
    QByteArray body;
    for (const auto& [key,value] : query.queryItems()) {
        if (!body.isEmpty()) body += '&';
        body += QUrl::toPercentEncoding(key) + '=' + QUrl::toPercentEncoding(value);
    }
    const auto now = QDateTime::currentDateTimeUtc();
    const auto date = now.toString("yyyyMMdd"), timestamp = now.toString("yyyyMMdd'T'HHmmss'Z'");
    const auto host = endpoint.host() + (endpoint.port() > 0 ? ':' + QString::number(endpoint.port()) : QString());
    QString headers = "content-type:application/x-www-form-urlencoded\nhost:" + host + "\nx-amz-date:" + timestamp + '\n';
    QString signedHeaders = "content-type;host;x-amz-date";
    if (!source.sessionToken.isEmpty()) { headers += "x-amz-security-token:" + source.sessionToken + '\n'; signedHeaders += ";x-amz-security-token"; }
    const auto hash = [](const QByteArray& data) { return QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex(); };
    const auto hmac = [](const QByteArray& key, const QByteArray& data) { return QMessageAuthenticationCode::hash(data,key,QCryptographicHash::Sha256); };
    const auto canonical = "POST\n" + endpoint.path(QUrl::FullyEncoded) + "\n\n" + headers + '\n' + signedHeaders + '\n' +
        QString::fromLatin1(hash(body));
    const auto scope = date + '/' + region + "/sts/aws4_request";
    const auto toSign = "AWS4-HMAC-SHA256\n" + timestamp + '\n' + scope + '\n' + QString::fromLatin1(hash(canonical.toUtf8()));
    const auto key = hmac(hmac(hmac(hmac("AWS4" + source.secretKey.toUtf8(),date.toUtf8()),region.toUtf8()),"sts"),"aws4_request");
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader,"application/x-www-form-urlencoded");
    request.setRawHeader("Host",host.toUtf8()); request.setRawHeader("X-Amz-Date",timestamp.toUtf8());
    if (!source.sessionToken.isEmpty()) request.setRawHeader("X-Amz-Security-Token",source.sessionToken.toUtf8());
    request.setRawHeader("Authorization",("AWS4-HMAC-SHA256 Credential=" + source.accessKey + '/' + scope +
        ", SignedHeaders=" + signedHeaders + ", Signature=" + QString::fromLatin1(hmac(key,toSign.toUtf8()).toHex())).toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    QNetworkAccessManager manager;
    auto* reply = manager.post(request,body);
    QEventLoop loop; QTimer cancellation, deadline;
    QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);
    QObject::connect(&cancellation,&QTimer::timeout,reply,[reply,cancel] { if (cancel && cancel->load()) reply->abort(); });
    QObject::connect(&deadline,&QTimer::timeout,reply,&QNetworkReply::abort);
    cancellation.start(50); deadline.setSingleShot(true); deadline.start(12000); loop.exec();
    const bool ok = reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    const auto payload = reply->isOpen() ? reply->readAll() : QByteArray(); delete reply;
    if (!ok) return std::unexpected(QStringLiteral("STS AssumeRole failed"));
    QXmlStreamReader xml(payload);
    S3Credentials result; QDateTime expiration;
    while (!xml.atEnd()) {
        xml.readNext(); if (!xml.isStartElement()) continue;
        if (xml.name() == QStringLiteral("AccessKeyId")) result.accessKey = xml.readElementText();
        else if (xml.name() == QStringLiteral("SecretAccessKey")) result.secretKey = xml.readElementText();
        else if (xml.name() == QStringLiteral("SessionToken")) result.sessionToken = xml.readElementText();
        else if (xml.name() == QStringLiteral("Expiration")) expiration = QDateTime::fromString(xml.readElementText(),Qt::ISODate);
    }
    if (xml.hasError() || result.accessKey.isEmpty() || result.secretKey.isEmpty() || result.sessionToken.isEmpty() ||
        !expiration.isValid() || expiration <= now)
        return std::unexpected(QStringLiteral("STS returned incomplete or expired credentials"));
    return result;
}
