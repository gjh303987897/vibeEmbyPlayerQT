#pragma once

#include "models/DailyUsageStat.h"
#include "models/IptvChannel.h"
#include "models/IptvPlaylist.h"
#include "models/ServiceCard.h"
#include "models/UserSession.h"

#include <QSettings>
#include <QSqlDatabase>
#include <QString>

#include <expected>
#include <optional>
#include <vector>

class SessionRepository final {
public:
    explicit SessionRepository(QString connectionName = QStringLiteral("vibeplayer_session"));
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
                                               qint64 networkBytesOut);
    std::expected<std::vector<DailyUsageStat>, QString> loadDailyUsageStats(bool includePrivacyMode);
    std::expected<void, QString> pruneOldDailyUsage();
    std::expected<std::vector<ServiceCard>, QString> loadServiceCards(bool privacyMode);
    std::expected<std::vector<ServiceCard>, QString> loadAllServiceCards();
    std::expected<std::optional<IptvPlaylist>, QString> loadIptvPlaylist(const QString& serviceId);
    std::expected<std::vector<IptvChannel>, QString> loadIptvChannels(const QString& serviceId);
    std::expected<std::optional<UserSession>, QString> loadLastSession();
    std::expected<std::optional<UserSession>, QString> loadSession(const QString& serverId);
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
    QString defaultDownloadDirectory() const;
    void setDefaultDownloadDirectory(const QString& directory);

private:
    QString databasePath() const;
    std::expected<void, QString> ensureOpen();
    std::expected<void, QString> ensureColumn(const QString& table, const QString& column, const QString& definition);
    std::expected<void, QString> migrateSessionsTable();
    std::expected<void, QString> migrateDailyUsageStatsTable();
    std::expected<std::optional<UserSession>, QString> sessionFromQuery(QSqlQuery& query);

    QString m_connectionName;
    QSqlDatabase m_database;
    QSettings m_settings;
};
