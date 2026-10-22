#pragma once
#include "database/SessionRepository.h"
#include "services/iptv/IptvStreamWorkflow.h"
#include <QFutureWatcher>
#include <QSet>
#include <QTimer>
#include <QMap>
#include <functional>

namespace Cli {
class IptvPlayback final : public QObject {
public:
    IptvPlayback(SessionRepository& repository, QString databasePath, QObject* parent = nullptr);
    ~IptvPlayback() override;
    std::expected<void,QString> start(const QString& service, const QString& channel, const QString& preference,
                                     const QString& stream, std::function<void(const QString&)> load);
    void stop();
    bool active() const { return m_channel.has_value(); }
    bool retry(bool failed);
    void started();
    bool switchStream(const QString& id);
    QJsonArray streams(bool probe = false, bool refresh = false);
    void update(const QString& resolution, const QString& codec, const QString& bitrate);
    bool retrying() const { return m_retrying; }
    std::function<void()> timeoutExhausted;
private:
    bool next(const QString& id = {});
    void scheduleProbe(bool detailed, bool refresh);
    void nextProbe();
    void flush();
    SessionRepository& m_repository;
    QString m_databasePath;
    std::optional<IptvChannel> m_channel;
    IptvStreamSelector::Selection m_selected;
    QString m_preference;
    QSet<QString> m_tried;
    IptvProbe m_probe;
    QTimer m_startup, m_flushTimer;
    QFutureWatcher<std::expected<void,QString>> m_writer;
    QMap<QString,IptvChannel> m_dirty;
    QList<QPair<QString,bool>> m_queue;
    std::function<void(const QString&)> m_load;
    bool m_retrying {false};
    bool m_playing {false};
    quint64 m_generation {0};
};
}
