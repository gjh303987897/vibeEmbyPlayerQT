#pragma once

#include <QDateTime>
#include <QFile>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStringList>
#include <QUrl>

#include <optional>

struct SemVersion final {
    int major { 0 };
    int minor { 0 };
    int patch { 0 };
    QStringList prerelease;
    bool valid { false };
    QString toString() const;
};

enum class UpdateChannel { Stable, Beta, Alpha };

struct UpdateAsset final {
    QString name;
    QUrl downloadUrl;
    qint64 size { 0 };
    QString checksumName;
    QUrl checksumUrl;
    bool checksumAvailable { false };
    bool preferred { false };
};

struct UpdateRelease final {
    SemVersion version;
    QString tagName;
    QString notes;
    QDateTime publishedAt;
    QList<UpdateAsset> assets;
};

struct UpdateCheckResult final {
    bool success { false };
    bool notModified { false };
    std::optional<UpdateRelease> release;
    QString error;
};

class UpdateService final : public QObject {
    Q_OBJECT

public:
    explicit UpdateService(QObject* parent = nullptr);

    static std::optional<SemVersion> parseVersion(const QString& value);
    static int compareVersions(const SemVersion& left, const SemVersion& right);
    static std::optional<UpdateChannel> classifyVersion(const SemVersion& version);
    static bool channelAccepts(const SemVersion& version, UpdateChannel channel);
    static std::optional<UpdateChannel> channelFromString(const QString& value);
    static QString channelToString(UpdateChannel channel);
    static std::optional<QString> parseChecksum(const QByteArray& content, const QString& fileName);
    static std::optional<UpdateRelease> parseReleases(const QByteArray& content,
                                                       UpdateChannel channel,
                                                       const SemVersion& currentVersion);
    static QList<UpdateAsset> selectPlatformAssets(const QList<UpdateAsset>& assets);

    void check(UpdateChannel channel, const SemVersion& currentVersion, const QByteArray& etag = {});
    void download(const UpdateAsset& asset);
    void cancelDownload();

signals:
    void checkFinished(const UpdateCheckResult& result, const QByteArray& etag);
    void downloadProgress(qint64 received, qint64 total);
    void downloadFinished(const QString& filePath);
    void downloadFailed(const QString& error);
    void downloadStateChanged(bool active);

private:
    void finishDownloadWithError(const QString& error);
    void downloadChecksum(const UpdateAsset& asset, const QString& packagePath);
    bool verifyPackage(const QString& packagePath, const QByteArray& checksumContent, const QString& fileName);

    QNetworkAccessManager m_manager;
    QNetworkReply* m_checkReply { nullptr };
    QNetworkReply* m_downloadReply { nullptr };
    QNetworkReply* m_checksumReply { nullptr };
    QFile m_downloadFile;
    QString m_downloadPath;
    UpdateAsset m_downloadAsset;
};
