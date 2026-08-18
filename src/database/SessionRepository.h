#pragma once

#include "models/DailyUsageStat.h"
#include "models/IptvChannel.h"
#include "models/IptvPlaylist.h"
#include "models/LinkPlaybackHistoryItem.h"
#include "models/LocalMediaRoot.h"
#include "models/PlaybackHistoryItem.h"
#include "models/ScheduledPlaybackTask.h"
#include "models/ServiceCard.h"
#include "models/UserSession.h"

#include <QByteArray>
#include <QDateTime>
#include <QSettings>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <expected>
#include <optional>
#include <vector>

struct RecommendationCacheRecord final {
    QByteArray payload;
    QDateTime refreshedAt;
};

class SessionRepository final {
public:
    explicit SessionRepository(QString connectionName = QStringLiteral("vibeplayer_session"),
                               QString databasePathOverride = {});
    ~SessionRepository();

    std::expected<void, QString> initialize();
    std::expected<void, QString> saveSession(const UserSession& session);
    std::expected<void, QString> saveServer(const ServerConfig& server);
    std::expected<void, QString> saveIptvPlaylist(const ServerConfig& server,
                                                  const IptvPlaylist& playlist,
                                                  const std::vector<IptvChannel>& channels);
    std::expected<void, QString> addDailyUsage(const ServerConfig& server,
                                               bool privacyMode,
                                               qint64 watchSeconds,
                                               qint64 networkBytesIn,
                                               qint64 networkBytesOut,
                                               qint64 keepAliveNetworkBytesIn,
                                               qint64 keepAliveNetworkBytesOut);
    std::expected<std::vector<DailyUsageStat>, QString> loadDailyUsageStats(bool includePrivacyMode);
    std::expected<void, QString> pruneOldDailyUsage();
    std::expected<void, QString> saveLinkPlaybackHistory(const LinkPlaybackHistoryItem& item);
    std::expected<std::vector<LinkPlaybackHistoryItem>, QString> loadLinkPlaybackHistory(bool includePrivacyMode = false);
    std::expected<void, QString> deleteLinkPlaybackHistory(const QString& recordId);
    std::expected<void, QString> savePlaybackHistory(const PlaybackHistoryItem& item);
    std::expected<std::vector<PlaybackHistoryItem>, QString> loadPlaybackHistory(bool includePrivacyMode,
                                                                                 int startIndex,
                                                                                 int limit,
                                                                                 PlaybackHistorySource source = PlaybackHistorySource::Unknown);
    std::expected<void, QString> updatePlaybackHistoryProgress(const QString& recordId,
                                                               qint64 positionSeconds,
                                                               qint64 durationSeconds,
                                                               bool completed,
                                                               const QDateTime& updatedAt);
    std::expected<void, QString> deletePlaybackHistory(const QString& recordId);
    std::expected<std::vector<ServiceCard>, QString> loadServiceCards(bool privacyMode);
    std::expected<std::vector<ServiceCard>, QString> loadAllServiceCards();
    std::expected<std::optional<IptvPlaylist>, QString> loadIptvPlaylist(const QString& serviceId);
    std::expected<std::vector<IptvChannel>, QString> loadIptvChannels(const QString& serviceId);
    std::expected<std::vector<LocalMediaRoot>, QString> loadLocalMediaRoots();
    std::expected<void, QString> saveLocalMediaRoot(const LocalMediaRoot& root);
    std::expected<void, QString> deleteLocalMediaRoot(const QString& rootId);
    std::expected<std::vector<ScheduledPlaybackTask>, QString> loadScheduledPlaybackTasks(bool privacyMode);
    std::expected<void, QString> saveScheduledPlaybackTask(const ScheduledPlaybackTask& task);
    std::expected<void, QString> deleteScheduledPlaybackTask(const QString& taskId);
    std::expected<void, QString> setScheduledPlaybackTaskLastRun(const QString& taskId, const QString& date);
    QString scheduledPlaybackCheckpoint(bool privateMode) const;
    void setScheduledPlaybackCheckpoint(bool privateMode, const QString& timestamp);
    QStringList pendingMissedScheduledPlaybackTaskIds() const;
    void setPendingMissedScheduledPlaybackTaskIds(const QStringList& taskIds);
    std::expected<std::optional<UserSession>, QString> loadLastSession();
    std::expected<std::optional<UserSession>, QString> loadSession(const QString& serverId);
    std::expected<std::optional<RecommendationCacheRecord>, QString> loadEmbyRecommendationCache(
        const QString& serverId,
        const QString& userId);
    std::expected<void, QString> saveEmbyRecommendationCache(const QString& serverId,
                                                             const QString& userId,
                                                             const QByteArray& payload,
                                                             const QDateTime& refreshedAt);
    std::expected<void, QString> clearEmbyRecommendationCaches();
    std::expected<void, QString> deleteServer(const QString& serverId, bool deleteLocalData);
    std::expected<void, QString> moveServer(const QString& serverId, int direction, bool privacyMode);
    std::expected<void, QString> moveServerTo(const QString& serverId, int targetIndex, bool privacyMode);
    std::expected<void, QString> setServerPrivateMode(const QString& serverId, bool privateMode);
    std::expected<void, QString> clearSession();

    bool privacyPinConfigured() const;
    QString privacyPinSalt() const;
    QString privacyPinHash() const;
    void setPrivacyPinHash(const QString& salt, const QString& hash);
    bool minimizeToTray() const;
    void setMinimizeToTray(bool enabled);
    QString themeMode() const;
    void setThemeMode(const QString& mode);
    QString languageMode() const;
    void setLanguageMode(const QString& mode);
    QString embyHomeLayout() const;
    void setEmbyHomeLayout(const QString& layout);
    QString jellyfinHomeLayout() const;
    void setJellyfinHomeLayout(const QString& layout);
    QString playerLayout() const;
    void setPlayerLayout(const QString& layout);
    bool pageTransitionsEnabled() const;
    void setPageTransitionsEnabled(bool enabled);
    QStringList embyRecommendationExcludedGenres() const;
    void setEmbyRecommendationExcludedGenres(const QStringList& genres);
    QStringList embyRecommendationAvailableGenres() const;
    void setEmbyRecommendationAvailableGenres(const QStringList& genres);
    QString defaultDownloadDirectory() const;
    void setDefaultDownloadDirectory(const QString& directory);
    QString m3u8sOutputDirectory() const;
    void setM3u8sOutputDirectory(const QString& directory);
    QString m3u8sVideoEncoding() const;
    void setM3u8sVideoEncoding(const QString& encoding);
    QString m3u8sAudioEncoding() const;
    void setM3u8sAudioEncoding(const QString& encoding);
    QString m3u8sVideoQuality() const;
    void setM3u8sVideoQuality(const QString& quality);
    QString updateChannel() const;
    void setUpdateChannel(const QString& channel);
    bool automaticUpdateCheck() const;
    void setAutomaticUpdateCheck(bool enabled);
    QDateTime updateLastCheckedAt() const;
    void setUpdateLastCheckedAt(const QDateTime& value);
    QByteArray updateEtag() const;
    void setUpdateEtag(const QByteArray& value);
    QString updateLastVersion() const;
    void setUpdateLastVersion(const QString& value);

private:
    QString databasePath() const;
    std::expected<void, QString> ensureOpen();
    std::expected<void, QString> ensureColumn(const QString& table, const QString& column, const QString& definition);
    std::expected<void, QString> migrateSessionsTable();
    std::expected<void, QString> migrateDailyUsageStatsTable();
    std::expected<std::optional<UserSession>, QString> sessionFromQuery(QSqlQuery& query);

    QString m_connectionName;
    QString m_databasePathOverride;
    QSqlDatabase m_database;
    QSettings m_settings;
};
