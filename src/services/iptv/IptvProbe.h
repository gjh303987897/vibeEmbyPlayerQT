#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include "models/IptvChannel.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryFile;

struct IptvProbeResult final {
    bool available { false };
    int width { 0 };
    int height { 0 };
    qint64 bitrate { 0 };
    QString codec;
    QString state;
    QString error;
    std::vector<IptvStreamVariant> variants;
};

class IptvProbe final : public QObject {
    Q_OBJECT

public:
    explicit IptvProbe(QObject* parent = nullptr);
    ~IptvProbe() override;
    void probe(const QString& channelId, const QUrl& url, const QString& preference);
    void cancel();
    static IptvProbeResult parse(const QByteArray& payload, int statusCode,
                                const QString& preference, const QUrl& baseUrl);

signals:
    void finished(const QString& channelId, const QUrl& url, const IptvProbeResult& result);

private:
    void finish(QNetworkReply* reply, const IptvProbeResult& result);
    void probePrefix(const QByteArray& payload, IptvProbeResult result);
    void complete(const IptvProbeResult& result);

    QNetworkAccessManager* m_manager { nullptr };
    QNetworkReply* m_activeReply { nullptr };
    QString m_channelId;
    QUrl m_url;
    QString m_preference;
    QByteArray m_payload;
    QProcess* m_process { nullptr };
    QTemporaryFile* m_sample { nullptr };
    quint64 m_generation { 0 };
};
