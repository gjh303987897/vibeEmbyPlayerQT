#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <deque>
#include "models/IptvChannel.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryFile;
class QTimer;

struct IptvProbeResult final {
    bool available { false };
    int width { 0 };
    int height { 0 };
    qint64 bitrate { 0 };
    QString codec;
    QString state;
    QString error;
    std::vector<IptvStreamVariant> variants;
    bool detailed { false };
};

class IptvProbe final : public QObject {
    Q_OBJECT

public:
    explicit IptvProbe(QObject* parent = nullptr);
    ~IptvProbe() override;
    void probe(const QString& channelId, const QUrl& url, const QString& preference, bool detailed = false);
    void cancel();
    static IptvProbeResult parse(const QByteArray& payload, int statusCode,
                                const QString& preference, const QUrl& baseUrl);

signals:
    void finished(const QString& channelId, const QUrl& url, const IptvProbeResult& result);

private:
    enum class FetchKind { Document, Initialization, Segment };
    struct Target { QUrl url; int variant { -1 }; };
    void fetch(const QUrl& url, FetchKind kind = FetchKind::Document, qint64 offset = 0, qint64 length = -1, bool range = true);
    void consume(const QByteArray& payload, const QUrl& url, FetchKind kind);
    void nextTarget();
    void targetFinished(const IptvProbeResult& result);
    void finishRoute(const QString& incompleteState = {});
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
    bool m_detailed { false };
    IptvProbeResult m_result;
    std::deque<Target> m_targets;
    int m_variant { -1 };
    int m_depth { 0 };
    int m_requests { 0 };
    qint64 m_bytes { 0 };
    QByteArray m_initialization;
    QUrl m_segmentUrl;
    qint64 m_segmentOffset { 0 };
    qint64 m_segmentLength { -1 };
    QTimer* m_deadline { nullptr };
};
