#pragma once

#include "models/ServerConfig.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>
#include <QStringList>

#include <expected>
#include <functional>

struct TsslBackupTarget final {
    enum class Type { WebDav, S3 };

    Type type { Type::WebDav };
    ServerConfig webDavServer;
    QString webDavPassword;
    QString webDavPath;
    QUrl s3Endpoint;
    QString s3Bucket;
    QString s3Region;
    QString s3Prefix;
    QString s3AccessKey;
    QString s3SecretKey;
    bool trustSelfSignedCertificate { false };
};

using TsslBackupResult = std::expected<int, QString>;

class TsslBackupService final : public QObject {
    Q_OBJECT

public:
    explicit TsslBackupService(QObject* parent = nullptr);

    bool isRunning() const;
    void backup(const TsslBackupTarget& target,
                QStringList localFiles,
                std::function<void(TsslBackupResult)> callback);
    void cancel();

signals:
    void progressChanged(int completed, int total);

private:
    void uploadNext();
    void finish(TsslBackupResult result);
    void uploadWebDav(const QString& localPath, const QByteArray& payload);
    void uploadS3(const QString& localPath, const QByteArray& payload);
    void wireReply(QNetworkReply* reply);

    QNetworkAccessManager m_manager;
    TsslBackupTarget m_target;
    QStringList m_localFiles;
    std::function<void(TsslBackupResult)> m_callback;
    int m_nextIndex { 0 };
    int m_completed { 0 };
    bool m_running { false };
    bool m_cancelRequested { false };
};
