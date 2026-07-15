#include "database/SessionRepository.h"

#include <QDateTime>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

#include <algorithm>
#include <optional>
#include <utility>

namespace {
QString sqlError(const QSqlQuery& query)
{
    return query.lastError().text();
}

QString nonNullText(const QString& value)
{
    return value.isNull() ? QStringLiteral("") : value;
}

std::expected<void, QString> upsertServer(QSqlDatabase& database, const ServerConfig& server)
{
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QSqlQuery orderQuery(database);
    int nextOrder = 0;
    if (orderQuery.exec(QStringLiteral("SELECT COALESCE(MAX(sort_order), -1) + 1 FROM servers")) && orderQuery.next()) {
        nextOrder = orderQuery.value(0).toInt();
    }

    QSqlQuery serverQuery(database);
    serverQuery.prepare(QStringLiteral(
        "INSERT INTO servers (id, name, base_url, username, service_type, trust_self_signed, auto_login, private_mode, enabled, sort_order, last_used_at) "
        "VALUES (:id, :name, :base_url, :username, :service_type, :trust_self_signed, :auto_login, :private_mode, 1, :sort_order, :last_used_at) "
        "ON CONFLICT(id) DO UPDATE SET "
        "name = excluded.name, "
        "base_url = excluded.base_url, "
        "username = excluded.username, "
        "service_type = excluded.service_type, "
        "trust_self_signed = excluded.trust_self_signed, "
        "auto_login = excluded.auto_login, "
        "private_mode = excluded.private_mode, "
        "enabled = 1, "
        "last_used_at = excluded.last_used_at"));
    serverQuery.bindValue(QStringLiteral(":id"), server.id);
    serverQuery.bindValue(QStringLiteral(":name"), server.name);
    serverQuery.bindValue(QStringLiteral(":base_url"), server.baseUrl);
    serverQuery.bindValue(QStringLiteral(":username"), nonNullText(server.username));
    serverQuery.bindValue(QStringLiteral(":service_type"), serviceTypeToString(server.serviceType));
    serverQuery.bindValue(QStringLiteral(":trust_self_signed"), server.trustSelfSignedCertificate ? 1 : 0);
    serverQuery.bindValue(QStringLiteral(":auto_login"), server.autoLogin ? 1 : 0);
    serverQuery.bindValue(QStringLiteral(":private_mode"), server.privateMode ? 1 : 0);
    serverQuery.bindValue(QStringLiteral(":sort_order"), nextOrder);
    serverQuery.bindValue(QStringLiteral(":last_used_at"), now);

    if (!serverQuery.exec()) {
        return std::unexpected(sqlError(serverQuery));
    }

    return {};
}

std::expected<void, QString> createDailyUsageStatsTable(QSqlDatabase& database)
{
    QSqlQuery usageQuery(database);
    if (!usageQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS daily_usage_stats ("
            "stat_date TEXT NOT NULL,"
            "service_id TEXT NOT NULL,"
            "service_name TEXT NOT NULL,"
            "service_type TEXT NOT NULL,"
            "watch_seconds INTEGER NOT NULL DEFAULT 0,"
            "network_bytes_in INTEGER NOT NULL DEFAULT 0,"
            "network_bytes_out INTEGER NOT NULL DEFAULT 0,"
            "keep_alive_network_bytes_in INTEGER NOT NULL DEFAULT 0,"
            "keep_alive_network_bytes_out INTEGER NOT NULL DEFAULT 0,"
            "privacy_mode INTEGER NOT NULL DEFAULT 0,"
            "updated_at TEXT NOT NULL,"
            "PRIMARY KEY(stat_date, service_id, privacy_mode)"
            ")"))) {
        return std::unexpected(sqlError(usageQuery));
    }
    return {};
}
}

SessionRepository::SessionRepository(QString connectionName)
    : m_connectionName(std::move(connectionName))
    , m_settings(QStringLiteral("vibePlayerQT"), QStringLiteral("vibePlayerQT"))
{
}

SessionRepository::~SessionRepository()
{
    if (m_database.isValid()) {
        m_database.close();
    }
}

std::expected<void, QString> SessionRepository::initialize()
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery serverQuery(m_database);
    if (!serverQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS servers ("
            "id TEXT PRIMARY KEY,"
            "name TEXT NOT NULL,"
            "base_url TEXT NOT NULL,"
            "username TEXT NOT NULL DEFAULT '',"
            "service_type TEXT NOT NULL,"
            "trust_self_signed INTEGER NOT NULL DEFAULT 0,"
            "auto_login INTEGER NOT NULL DEFAULT 1,"
            "private_mode INTEGER NOT NULL DEFAULT 0,"
            "enabled INTEGER NOT NULL DEFAULT 1,"
            "sort_order INTEGER NOT NULL DEFAULT 0,"
            "last_used_at TEXT NOT NULL"
            ")"))) {
        return std::unexpected(sqlError(serverQuery));
    }

    if (auto columnResult = ensureColumn(QStringLiteral("servers"), QStringLiteral("username"), QStringLiteral("TEXT NOT NULL DEFAULT ''")); !columnResult) {
        return columnResult;
    }
    if (auto columnResult = ensureColumn(QStringLiteral("servers"), QStringLiteral("auto_login"), QStringLiteral("INTEGER NOT NULL DEFAULT 1")); !columnResult) {
        return columnResult;
    }
    if (auto columnResult = ensureColumn(QStringLiteral("servers"), QStringLiteral("private_mode"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")); !columnResult) {
        return columnResult;
    }
    if (auto columnResult = ensureColumn(QStringLiteral("servers"), QStringLiteral("enabled"), QStringLiteral("INTEGER NOT NULL DEFAULT 1")); !columnResult) {
        return columnResult;
    }
    if (auto columnResult = ensureColumn(QStringLiteral("servers"), QStringLiteral("sort_order"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")); !columnResult) {
        return columnResult;
    }

    if (auto migrateResult = migrateSessionsTable(); !migrateResult) {
        return migrateResult;
    }

    QSqlQuery playlistQuery(m_database);
    if (!playlistQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS iptv_playlists ("
            "id TEXT PRIMARY KEY,"
            "service_id TEXT NOT NULL UNIQUE,"
            "name TEXT NOT NULL,"
            "source_type TEXT NOT NULL,"
            "source_path TEXT NOT NULL,"
            "imported_path TEXT NOT NULL,"
            "imported_at TEXT NOT NULL,"
            "FOREIGN KEY(service_id) REFERENCES servers(id) ON DELETE CASCADE"
            ")"))) {
        return std::unexpected(sqlError(playlistQuery));
    }

    QSqlQuery channelQuery(m_database);
    if (!channelQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS iptv_channels ("
            "id TEXT NOT NULL,"
            "playlist_id TEXT NOT NULL,"
            "name TEXT NOT NULL,"
            "group_name TEXT NOT NULL,"
            "logo_url TEXT NOT NULL DEFAULT '',"
            "stream_url TEXT NOT NULL,"
            "sort_order INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY(id, playlist_id),"
            "FOREIGN KEY(playlist_id) REFERENCES iptv_playlists(id) ON DELETE CASCADE"
            ")"))) {
        return std::unexpected(sqlError(channelQuery));
    }

    if (auto migrateUsageResult = migrateDailyUsageStatsTable(); !migrateUsageResult) {
        return migrateUsageResult;
    }
    if (auto columnResult = ensureColumn(QStringLiteral("daily_usage_stats"),
                                         QStringLiteral("keep_alive_network_bytes_in"),
                                         QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
        !columnResult) {
        return columnResult;
    }
    if (auto columnResult = ensureColumn(QStringLiteral("daily_usage_stats"),
                                         QStringLiteral("keep_alive_network_bytes_out"),
                                         QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
        !columnResult) {
        return columnResult;
    }

    QSqlQuery scheduledTaskQuery(m_database);
    if (!scheduledTaskQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS scheduled_playback_tasks ("
            "id TEXT PRIMARY KEY,"
            "server_id TEXT NOT NULL,"
            "start_time TEXT NOT NULL,"
            "duration_minutes INTEGER NOT NULL DEFAULT 90,"
            "enabled INTEGER NOT NULL DEFAULT 1,"
            "last_run_date TEXT NOT NULL DEFAULT '',"
            "created_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL,"
            "FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE"
            ")"))) {
        return std::unexpected(sqlError(scheduledTaskQuery));
    }

    if (auto pruneResult = pruneOldDailyUsage(); !pruneResult) {
        return pruneResult;
    }

    return {};
}

std::expected<void, QString> SessionRepository::saveSession(const UserSession& session)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    if (auto serverResult = saveServer(session.server); !serverResult) {
        return serverResult;
    }

    QSqlQuery sessionQuery(m_database);
    sessionQuery.prepare(QStringLiteral(
        "INSERT INTO sessions (server_id, user_id, username, access_token, created_at) "
        "VALUES (:server_id, :user_id, :username, :access_token, :created_at) "
        "ON CONFLICT(server_id, username) DO UPDATE SET "
        "user_id = excluded.user_id, "
        "access_token = excluded.access_token, "
        "created_at = excluded.created_at"));
    sessionQuery.bindValue(QStringLiteral(":server_id"), session.server.id);
    sessionQuery.bindValue(QStringLiteral(":user_id"), session.userId);
    sessionQuery.bindValue(QStringLiteral(":username"), session.server.username.isEmpty() ? session.username : session.server.username);
    sessionQuery.bindValue(QStringLiteral(":access_token"), session.accessToken);
    sessionQuery.bindValue(QStringLiteral(":created_at"), session.createdAt.toString(Qt::ISODate));

    if (!sessionQuery.exec()) {
        return std::unexpected(sqlError(sessionQuery));
    }

    return {};
}

std::expected<void, QString> SessionRepository::saveServer(const ServerConfig& server)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    return upsertServer(m_database, server);
}

std::expected<void, QString> SessionRepository::saveIptvPlaylist(const ServerConfig& server,
                                                                 const IptvPlaylist& playlist,
                                                                 const std::vector<IptvChannel>& channels)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery transaction(m_database);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        return std::unexpected(sqlError(transaction));
    }

    if (auto serverResult = upsertServer(m_database, server); !serverResult) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return serverResult;
    }

    QSqlQuery playlistQuery(m_database);
    playlistQuery.prepare(QStringLiteral(
        "INSERT INTO iptv_playlists (id, service_id, name, source_type, source_path, imported_path, imported_at) "
        "VALUES (:id, :service_id, :name, :source_type, :source_path, :imported_path, :imported_at) "
        "ON CONFLICT(service_id) DO UPDATE SET "
        "id = excluded.id, "
        "name = excluded.name, "
        "source_type = excluded.source_type, "
        "source_path = excluded.source_path, "
        "imported_path = excluded.imported_path, "
        "imported_at = excluded.imported_at"));
    playlistQuery.bindValue(QStringLiteral(":id"), playlist.id);
    playlistQuery.bindValue(QStringLiteral(":service_id"), server.id);
    playlistQuery.bindValue(QStringLiteral(":name"), playlist.name);
    playlistQuery.bindValue(QStringLiteral(":source_type"), playlist.sourceType);
    playlistQuery.bindValue(QStringLiteral(":source_path"), playlist.sourcePath);
    playlistQuery.bindValue(QStringLiteral(":imported_path"), playlist.importedPath);
    playlistQuery.bindValue(QStringLiteral(":imported_at"), playlist.importedAt);
    if (!playlistQuery.exec()) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(playlistQuery));
    }

    QSqlQuery deleteChannels(m_database);
    deleteChannels.prepare(QStringLiteral("DELETE FROM iptv_channels WHERE playlist_id = :playlist_id"));
    deleteChannels.bindValue(QStringLiteral(":playlist_id"), playlist.id);
    if (!deleteChannels.exec()) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(deleteChannels));
    }

    QSqlQuery channelQuery(m_database);
    channelQuery.prepare(QStringLiteral(
        "INSERT INTO iptv_channels (id, playlist_id, name, group_name, logo_url, stream_url, sort_order) "
        "VALUES (:id, :playlist_id, :name, :group_name, :logo_url, :stream_url, :sort_order)"));
    for (const auto& channel : channels) {
        channelQuery.bindValue(QStringLiteral(":id"), channel.id);
        channelQuery.bindValue(QStringLiteral(":playlist_id"), playlist.id);
        channelQuery.bindValue(QStringLiteral(":name"), channel.name);
        channelQuery.bindValue(QStringLiteral(":group_name"), channel.groupName);
        channelQuery.bindValue(QStringLiteral(":logo_url"), channel.logoUrl);
        channelQuery.bindValue(QStringLiteral(":stream_url"), channel.streamUrl);
        channelQuery.bindValue(QStringLiteral(":sort_order"), channel.sortOrder);
        if (!channelQuery.exec()) {
            transaction.exec(QStringLiteral("ROLLBACK"));
            return std::unexpected(sqlError(channelQuery));
        }
    }

    QSqlQuery commit(m_database);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        return std::unexpected(sqlError(commit));
    }
    return {};
}

std::expected<void, QString> SessionRepository::addDailyUsage(const ServerConfig& server,
                                                              bool privacyMode,
                                                              qint64 watchSeconds,
                                                              qint64 networkBytesIn,
                                                              qint64 networkBytesOut,
                                                              qint64 keepAliveNetworkBytesIn,
                                                              qint64 keepAliveNetworkBytesOut)
{
    if (server.id.isEmpty()) {
        return {};
    }
    if (watchSeconds <= 0 && networkBytesIn <= 0 && networkBytesOut <= 0 &&
        keepAliveNetworkBytesIn <= 0 && keepAliveNetworkBytesOut <= 0) {
        return {};
    }
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    const auto today = QDate::currentDate().toString(Qt::ISODate);
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO daily_usage_stats "
        "(stat_date, service_id, service_name, service_type, watch_seconds, network_bytes_in, network_bytes_out, "
        "keep_alive_network_bytes_in, keep_alive_network_bytes_out, privacy_mode, updated_at) "
        "VALUES (:stat_date, :service_id, :service_name, :service_type, :watch_seconds, :network_bytes_in, :network_bytes_out, "
        ":keep_alive_network_bytes_in, :keep_alive_network_bytes_out, :privacy_mode, :updated_at) "
        "ON CONFLICT(stat_date, service_id, privacy_mode) DO UPDATE SET "
        "service_name = excluded.service_name, "
        "service_type = excluded.service_type, "
        "watch_seconds = watch_seconds + excluded.watch_seconds, "
        "network_bytes_in = network_bytes_in + excluded.network_bytes_in, "
        "network_bytes_out = network_bytes_out + excluded.network_bytes_out, "
        "keep_alive_network_bytes_in = keep_alive_network_bytes_in + excluded.keep_alive_network_bytes_in, "
        "keep_alive_network_bytes_out = keep_alive_network_bytes_out + excluded.keep_alive_network_bytes_out, "
        "updated_at = excluded.updated_at"));
    query.bindValue(QStringLiteral(":stat_date"), today);
    query.bindValue(QStringLiteral(":service_id"), server.id);
    query.bindValue(QStringLiteral(":service_name"), server.name.isEmpty() ? serviceTypeToString(server.serviceType) : server.name);
    query.bindValue(QStringLiteral(":service_type"), serviceTypeToString(server.serviceType));
    query.bindValue(QStringLiteral(":watch_seconds"), std::max<qint64>(0, watchSeconds));
    query.bindValue(QStringLiteral(":network_bytes_in"), std::max<qint64>(0, networkBytesIn));
    query.bindValue(QStringLiteral(":network_bytes_out"), std::max<qint64>(0, networkBytesOut));
    query.bindValue(QStringLiteral(":keep_alive_network_bytes_in"), std::max<qint64>(0, keepAliveNetworkBytesIn));
    query.bindValue(QStringLiteral(":keep_alive_network_bytes_out"), std::max<qint64>(0, keepAliveNetworkBytesOut));
    query.bindValue(QStringLiteral(":privacy_mode"), privacyMode ? 1 : 0);
    query.bindValue(QStringLiteral(":updated_at"), now);

    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<std::vector<DailyUsageStat>, QString> SessionRepository::loadDailyUsageStats(bool includePrivacyMode)
{
    if (auto pruneResult = pruneOldDailyUsage(); !pruneResult) {
        return std::unexpected(pruneResult.error());
    }
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    const auto cutoff = QDate::currentDate().addDays(-29).toString(Qt::ISODate);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT stats.stat_date, stats.service_id, MAX(stats.service_name), MAX(stats.service_type), "
        "SUM(stats.watch_seconds), SUM(stats.network_bytes_in), SUM(stats.network_bytes_out), "
        "SUM(stats.keep_alive_network_bytes_in), SUM(stats.keep_alive_network_bytes_out), "
        "COALESCE(servers.private_mode, stats.privacy_mode) "
        "FROM daily_usage_stats stats "
        "LEFT JOIN servers ON servers.id = stats.service_id "
        "WHERE stats.stat_date >= :cutoff "
        "AND (:include_privacy = 1 OR COALESCE(servers.private_mode, stats.privacy_mode) = 0) "
        "GROUP BY stats.stat_date, stats.service_id, COALESCE(servers.private_mode, stats.privacy_mode) "
        "ORDER BY stats.stat_date DESC, COALESCE(servers.private_mode, stats.privacy_mode) ASC, "
        "MAX(stats.service_name) COLLATE NOCASE ASC"));
    query.bindValue(QStringLiteral(":cutoff"), cutoff);
    query.bindValue(QStringLiteral(":include_privacy"), includePrivacyMode ? 1 : 0);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }

    std::vector<DailyUsageStat> stats;
    while (query.next()) {
        stats.push_back(DailyUsageStat {
            .date = query.value(0).toString(),
            .serviceId = query.value(1).toString(),
            .serviceName = query.value(2).toString(),
            .serviceType = query.value(3).toString(),
            .watchSeconds = query.value(4).toLongLong(),
            .networkBytesIn = query.value(5).toLongLong(),
            .networkBytesOut = query.value(6).toLongLong(),
            .keepAliveNetworkBytesIn = query.value(7).toLongLong(),
            .keepAliveNetworkBytesOut = query.value(8).toLongLong(),
            .privacyMode = query.value(9).toInt() == 1,
        });
    }
    return stats;
}

std::expected<void, QString> SessionRepository::pruneOldDailyUsage()
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    const auto cutoff = QDate::currentDate().addDays(-29).toString(Qt::ISODate);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM daily_usage_stats WHERE stat_date < :cutoff"));
    query.bindValue(QStringLiteral(":cutoff"), cutoff);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<std::vector<ServiceCard>, QString> SessionRepository::loadServiceCards(bool privacyMode)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "SELECT s.id, s.name, s.base_url, s.username, s.service_type, s.trust_self_signed, "
            "s.auto_login, s.last_used_at, s.private_mode, sess.access_token "
            "FROM servers s "
            "LEFT JOIN sessions sess ON sess.server_id = s.id AND sess.username = s.username "
            "WHERE s.enabled = 1 AND (:include_private = 1 OR s.private_mode = 0) "
            "ORDER BY s.sort_order ASC, s.last_used_at DESC"));
    query.bindValue(QStringLiteral(":include_private"), privacyMode ? 1 : 0);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }

    std::vector<ServiceCard> cards;
    while (query.next()) {
        ServerConfig server;
        server.id = query.value(0).toString();
        server.name = query.value(1).toString();
        server.baseUrl = query.value(2).toString();
        server.username = query.value(3).toString();
        server.serviceType = serviceTypeFromString(query.value(4).toString());
        server.trustSelfSignedCertificate = query.value(5).toInt() == 1;
        server.autoLogin = query.value(6).toInt() == 1;
        server.privateMode = query.value(8).toInt() == 1;

        cards.push_back(ServiceCard {
            .server = server,
            .hasSession = server.serviceType == ServiceType::IPTV || !query.value(9).toString().isEmpty(),
            .lastUsedAt = query.value(7).toString(),
        });
    }

    return cards;
}

std::expected<std::vector<ServiceCard>, QString> SessionRepository::loadAllServiceCards()
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT s.id, s.name, s.base_url, s.username, s.service_type, s.trust_self_signed, "
            "s.auto_login, s.last_used_at, s.private_mode, sess.access_token "
            "FROM servers s "
            "LEFT JOIN sessions sess ON sess.server_id = s.id AND sess.username = s.username "
            "WHERE s.enabled = 1 "
            "ORDER BY s.sort_order ASC, s.last_used_at DESC"))) {
        return std::unexpected(sqlError(query));
    }

    std::vector<ServiceCard> cards;
    while (query.next()) {
        ServerConfig server;
        server.id = query.value(0).toString();
        server.name = query.value(1).toString();
        server.baseUrl = query.value(2).toString();
        server.username = query.value(3).toString();
        server.serviceType = serviceTypeFromString(query.value(4).toString());
        server.trustSelfSignedCertificate = query.value(5).toInt() == 1;
        server.autoLogin = query.value(6).toInt() == 1;
        server.privateMode = query.value(8).toInt() == 1;

        cards.push_back(ServiceCard {
            .server = server,
            .hasSession = server.serviceType == ServiceType::IPTV || !query.value(9).toString().isEmpty(),
            .lastUsedAt = query.value(7).toString(),
        });
    }

    return cards;
}

std::expected<std::optional<IptvPlaylist>, QString> SessionRepository::loadIptvPlaylist(const QString& serviceId)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, service_id, name, source_type, source_path, imported_path, imported_at "
        "FROM iptv_playlists "
        "WHERE service_id = :service_id "
        "LIMIT 1"));
    query.bindValue(QStringLiteral(":service_id"), serviceId);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }

    if (!query.next()) {
        return std::optional<IptvPlaylist> {};
    }

    return std::optional<IptvPlaylist> { IptvPlaylist {
        .id = query.value(0).toString(),
        .serviceId = query.value(1).toString(),
        .name = query.value(2).toString(),
        .sourceType = query.value(3).toString(),
        .sourcePath = query.value(4).toString(),
        .importedPath = query.value(5).toString(),
        .importedAt = query.value(6).toString(),
    } };
}

std::expected<std::vector<IptvChannel>, QString> SessionRepository::loadIptvChannels(const QString& serviceId)
{
    const auto playlistResult = loadIptvPlaylist(serviceId);
    if (!playlistResult) {
        return std::unexpected(playlistResult.error());
    }
    if (!playlistResult->has_value()) {
        return std::vector<IptvChannel> {};
    }

    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, playlist_id, name, group_name, logo_url, stream_url, sort_order "
        "FROM iptv_channels "
        "WHERE playlist_id = :playlist_id "
        "ORDER BY group_name COLLATE NOCASE ASC, sort_order ASC, name COLLATE NOCASE ASC"));
    query.bindValue(QStringLiteral(":playlist_id"), playlistResult->value().id);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }

    std::vector<IptvChannel> channels;
    while (query.next()) {
        channels.push_back(IptvChannel {
            .id = query.value(0).toString(),
            .playlistId = query.value(1).toString(),
            .name = query.value(2).toString(),
            .groupName = query.value(3).toString(),
            .logoUrl = query.value(4).toString(),
            .streamUrl = query.value(5).toString(),
            .sortOrder = query.value(6).toInt(),
        });
    }
    return channels;
}

std::expected<std::vector<ScheduledPlaybackTask>, QString> SessionRepository::loadScheduledPlaybackTasks(bool privacyMode)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "SELECT t.id, t.server_id, s.name, s.username, t.start_time, t.duration_minutes, t.enabled, t.last_run_date, s.private_mode "
            "FROM scheduled_playback_tasks t "
            "JOIN servers s ON s.id = t.server_id "
            "WHERE s.enabled = 1 AND s.service_type = 'Emby' "
            "AND (:include_private = 1 OR s.private_mode = 0) "
            "ORDER BY t.start_time ASC, s.name COLLATE NOCASE ASC"));
    query.bindValue(QStringLiteral(":include_private"), privacyMode ? 1 : 0);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }

    std::vector<ScheduledPlaybackTask> tasks;
    while (query.next()) {
        tasks.push_back(ScheduledPlaybackTask {
            .id = query.value(0).toString(),
            .serverId = query.value(1).toString(),
            .serverName = query.value(2).toString(),
            .username = query.value(3).toString(),
            .startTime = query.value(4).toString(),
            .durationMinutes = query.value(5).toInt(),
            .enabled = query.value(6).toInt() == 1,
            .lastRunDate = query.value(7).toString(),
            .privateMode = query.value(8).toInt() == 1,
        });
    }
    return tasks;
}

std::expected<void, QString> SessionRepository::saveScheduledPlaybackTask(const ScheduledPlaybackTask& task)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO scheduled_playback_tasks "
        "(id, server_id, start_time, duration_minutes, enabled, last_run_date, created_at, updated_at) "
        "VALUES (:id, :server_id, :start_time, :duration_minutes, :enabled, :last_run_date, :created_at, :updated_at) "
        "ON CONFLICT(id) DO UPDATE SET "
        "server_id = excluded.server_id, "
        "start_time = excluded.start_time, "
        "duration_minutes = excluded.duration_minutes, "
        "enabled = excluded.enabled, "
        "last_run_date = excluded.last_run_date, "
        "updated_at = excluded.updated_at"));
    query.bindValue(QStringLiteral(":id"), task.id);
    query.bindValue(QStringLiteral(":server_id"), task.serverId);
    query.bindValue(QStringLiteral(":start_time"), task.startTime);
    query.bindValue(QStringLiteral(":duration_minutes"), task.durationMinutes);
    query.bindValue(QStringLiteral(":enabled"), task.enabled ? 1 : 0);
    query.bindValue(QStringLiteral(":last_run_date"),
                    task.lastRunDate.isNull() ? QStringLiteral("") : task.lastRunDate);
    query.bindValue(QStringLiteral(":created_at"), now);
    query.bindValue(QStringLiteral(":updated_at"), now);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<void, QString> SessionRepository::deleteScheduledPlaybackTask(const QString& taskId)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM scheduled_playback_tasks WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), taskId);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<void, QString> SessionRepository::setScheduledPlaybackTaskLastRun(const QString& taskId, const QString& date)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE scheduled_playback_tasks SET last_run_date = :date, updated_at = :updated_at WHERE id = :id"));
    query.bindValue(QStringLiteral(":date"), date);
    query.bindValue(QStringLiteral(":updated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":id"), taskId);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<std::optional<UserSession>, QString> SessionRepository::loadLastSession()
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT s.id, s.name, s.base_url, s.username, s.service_type, s.trust_self_signed, s.auto_login, s.private_mode, "
            "sess.user_id, sess.username, sess.access_token, sess.created_at "
            "FROM servers s "
            "JOIN sessions sess ON sess.server_id = s.id "
            "WHERE s.enabled = 1 AND s.private_mode = 0 "
            "ORDER BY s.last_used_at DESC "
            "LIMIT 1"))) {
        return std::unexpected(sqlError(query));
    }

    return sessionFromQuery(query);
}

std::expected<std::optional<UserSession>, QString> SessionRepository::loadSession(const QString& serverId)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT s.id, s.name, s.base_url, s.username, s.service_type, s.trust_self_signed, s.auto_login, s.private_mode, "
        "sess.user_id, sess.username, sess.access_token, sess.created_at "
        "FROM servers s "
        "JOIN sessions sess ON sess.server_id = s.id AND sess.username = s.username "
        "WHERE s.id = :server_id AND s.enabled = 1 "
        "LIMIT 1"));
    query.bindValue(QStringLiteral(":server_id"), serverId);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }

    return sessionFromQuery(query);
}

std::expected<void, QString> SessionRepository::deleteServer(const QString& serverId, bool deleteLocalData)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QString importedIptvPath;
    if (deleteLocalData) {
        QSqlQuery playlistQuery(m_database);
        playlistQuery.prepare(QStringLiteral("SELECT imported_path FROM iptv_playlists WHERE service_id = :server_id LIMIT 1"));
        playlistQuery.bindValue(QStringLiteral(":server_id"), serverId);
        if (playlistQuery.exec() && playlistQuery.next()) {
            importedIptvPath = playlistQuery.value(0).toString();
        }
    }

    if (!deleteLocalData) {
        QSqlQuery hideQuery(m_database);
        hideQuery.prepare(QStringLiteral("UPDATE servers SET enabled = 0 WHERE id = :server_id"));
        hideQuery.bindValue(QStringLiteral(":server_id"), serverId);
        if (!hideQuery.exec()) {
            return std::unexpected(sqlError(hideQuery));
        }
        return {};
    }

    {
        QSqlQuery sessionQuery(m_database);
        sessionQuery.prepare(QStringLiteral("DELETE FROM sessions WHERE server_id = :server_id"));
        sessionQuery.bindValue(QStringLiteral(":server_id"), serverId);
        if (!sessionQuery.exec()) {
            return std::unexpected(sqlError(sessionQuery));
        }
    }

    QSqlQuery serverQuery(m_database);
    serverQuery.prepare(QStringLiteral("DELETE FROM servers WHERE id = :server_id"));
    serverQuery.bindValue(QStringLiteral(":server_id"), serverId);
    if (!serverQuery.exec()) {
        return std::unexpected(sqlError(serverQuery));
    }
    if (!importedIptvPath.isEmpty()) {
        QFile::remove(importedIptvPath);
    }
    return {};
}

std::expected<void, QString> SessionRepository::moveServer(const QString& serverId, int direction, bool privacyMode)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    const auto cardsResult = loadServiceCards(privacyMode);
    if (!cardsResult) {
        return std::unexpected(cardsResult.error());
    }

    auto cards = *cardsResult;
    auto current = std::find_if(cards.begin(), cards.end(), [&serverId](const ServiceCard& card) {
        return card.server.id == serverId;
    });
    if (current == cards.end()) {
        return {};
    }

    const auto index = static_cast<int>(std::distance(cards.begin(), current));
    const auto targetIndex = index + direction;
    if (targetIndex < 0 || targetIndex >= static_cast<int>(cards.size())) {
        return {};
    }
    std::swap(cards[static_cast<size_t>(index)], cards[static_cast<size_t>(targetIndex)]);

    QSqlQuery transaction(m_database);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        return std::unexpected(sqlError(transaction));
    }

    for (int i = 0; i < static_cast<int>(cards.size()); ++i) {
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral("UPDATE servers SET sort_order = :sort_order WHERE id = :id"));
        update.bindValue(QStringLiteral(":sort_order"), i);
        update.bindValue(QStringLiteral(":id"), cards[static_cast<size_t>(i)].server.id);
        if (!update.exec()) {
            transaction.exec(QStringLiteral("ROLLBACK"));
            return std::unexpected(sqlError(update));
        }
    }

    QSqlQuery commit(m_database);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        return std::unexpected(sqlError(commit));
    }
    return {};
}

std::expected<void, QString> SessionRepository::moveServerTo(const QString& serverId, int targetIndex, bool privacyMode)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    const auto cardsResult = loadServiceCards(privacyMode);
    if (!cardsResult) {
        return std::unexpected(cardsResult.error());
    }

    auto cards = *cardsResult;
    if (cards.empty()) {
        return {};
    }

    targetIndex = std::clamp(targetIndex, 0, static_cast<int>(cards.size()) - 1);
    const auto current = std::find_if(cards.begin(), cards.end(), [&serverId](const ServiceCard& card) {
        return card.server.id == serverId;
    });
    if (current == cards.end()) {
        return {};
    }

    auto card = *current;
    cards.erase(current);
    cards.insert(cards.begin() + targetIndex, std::move(card));

    QSqlQuery transaction(m_database);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        return std::unexpected(sqlError(transaction));
    }

    for (int i = 0; i < static_cast<int>(cards.size()); ++i) {
        QSqlQuery update(m_database);
        update.prepare(QStringLiteral("UPDATE servers SET sort_order = :sort_order WHERE id = :id"));
        update.bindValue(QStringLiteral(":sort_order"), i);
        update.bindValue(QStringLiteral(":id"), cards[static_cast<size_t>(i)].server.id);
        if (!update.exec()) {
            transaction.exec(QStringLiteral("ROLLBACK"));
            return std::unexpected(sqlError(update));
        }
    }

    QSqlQuery commit(m_database);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        return std::unexpected(sqlError(commit));
    }
    return {};
}

std::expected<void, QString> SessionRepository::setServerPrivateMode(const QString& serverId, bool privateMode)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE servers SET private_mode = :private_mode WHERE id = :server_id AND enabled = 1"));
    query.bindValue(QStringLiteral(":private_mode"), privateMode ? 1 : 0);
    query.bindValue(QStringLiteral(":server_id"), serverId);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<void, QString> SessionRepository::clearSession()
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("DELETE FROM sessions"))) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

bool SessionRepository::privacyPinConfigured() const
{
    return !privacyPinSalt().isEmpty() && !privacyPinHash().isEmpty();
}

QString SessionRepository::privacyPinSalt() const
{
    return m_settings.value(QStringLiteral("privacy/pinSalt")).toString();
}

QString SessionRepository::privacyPinHash() const
{
    return m_settings.value(QStringLiteral("privacy/pinHash")).toString();
}

void SessionRepository::setPrivacyPinHash(const QString& salt, const QString& hash)
{
    m_settings.setValue(QStringLiteral("privacy/pinSalt"), salt);
    m_settings.setValue(QStringLiteral("privacy/pinHash"), hash);
}

bool SessionRepository::minimizeToTray() const
{
    return m_settings.value(QStringLiteral("desktop/minimizeToTray"), true).toBool();
}

void SessionRepository::setMinimizeToTray(bool enabled)
{
    m_settings.setValue(QStringLiteral("desktop/minimizeToTray"), enabled);
}

QString SessionRepository::themeMode() const
{
    return m_settings.value(QStringLiteral("appearance/themeMode"), QStringLiteral("dark")).toString();
}

void SessionRepository::setThemeMode(const QString& mode)
{
    m_settings.setValue(QStringLiteral("appearance/themeMode"), mode);
}

QString SessionRepository::languageMode() const
{
    return m_settings.value(QStringLiteral("appearance/languageMode"), QStringLiteral("system")).toString();
}

void SessionRepository::setLanguageMode(const QString& mode)
{
    m_settings.setValue(QStringLiteral("appearance/languageMode"), mode);
}

bool SessionRepository::pageTransitionsEnabled() const
{
    return m_settings.value(QStringLiteral("appearance/pageTransitionsEnabled"), true).toBool();
}

void SessionRepository::setPageTransitionsEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("appearance/pageTransitionsEnabled"), enabled);
}

QString SessionRepository::defaultDownloadDirectory() const
{
    return m_settings.value(QStringLiteral("webdav/defaultDownloadDirectory")).toString();
}

void SessionRepository::setDefaultDownloadDirectory(const QString& directory)
{
    m_settings.setValue(QStringLiteral("webdav/defaultDownloadDirectory"), directory);
}

QString SessionRepository::databasePath() const
{
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("vibeplayer.sqlite3"));
}

std::expected<void, QString> SessionRepository::ensureOpen()
{
    if (m_database.isOpen()) {
        return {};
    }

    if (QSqlDatabase::contains(m_connectionName)) {
        m_database = QSqlDatabase::database(m_connectionName);
    } else {
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    }

    m_database.setDatabaseName(databasePath());
    if (!m_database.open()) {
        return std::unexpected(m_database.lastError().text());
    }

    QSqlQuery pragma(m_database);
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    return {};
}

std::expected<void, QString> SessionRepository::ensureColumn(const QString& table, const QString& column, const QString& definition)
{
    QSqlQuery pragma(m_database);
    if (!pragma.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        return std::unexpected(sqlError(pragma));
    }

    while (pragma.next()) {
        if (pragma.value(QStringLiteral("name")).toString() == column) {
            return {};
        }
    }

    QSqlQuery alter(m_database);
    if (!alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, definition))) {
        return std::unexpected(sqlError(alter));
    }
    return {};
}

std::expected<void, QString> SessionRepository::migrateSessionsTable()
{
    QSqlQuery info(m_database);
    if (!info.exec(QStringLiteral("PRAGMA table_info(sessions)"))) {
        return std::unexpected(sqlError(info));
    }

    bool hasRows = false;
    bool hasIdColumn = false;
    while (info.next()) {
        hasRows = true;
        if (info.value(QStringLiteral("name")).toString() == QStringLiteral("id")) {
            hasIdColumn = true;
        }
    }

    if (!hasRows) {
        QSqlQuery create(m_database);
        if (!create.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sessions ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "server_id TEXT NOT NULL,"
            "user_id TEXT NOT NULL,"
            "username TEXT NOT NULL,"
            "access_token TEXT NOT NULL,"
            "created_at TEXT NOT NULL,"
            "UNIQUE(server_id, username),"
            "FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE"
            ")"))) {
            return std::unexpected(sqlError(create));
        }
    }

    QSqlQuery indexQuery(m_database);
    if (hasIdColumn && indexQuery.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_sessions_server_username ON sessions(server_id, username)"))) {
        return {};
    }

    if (hasIdColumn) {
        return std::unexpected(sqlError(indexQuery));
    }

    QSqlQuery transaction(m_database);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        return std::unexpected(sqlError(transaction));
    }

    QSqlQuery rename(m_database);
    if (!rename.exec(QStringLiteral("ALTER TABLE sessions RENAME TO sessions_legacy"))) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(rename));
    }

    QSqlQuery createNew(m_database);
    if (!createNew.exec(QStringLiteral(
            "CREATE TABLE sessions ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "server_id TEXT NOT NULL,"
            "user_id TEXT NOT NULL,"
            "username TEXT NOT NULL,"
            "access_token TEXT NOT NULL,"
            "created_at TEXT NOT NULL,"
            "UNIQUE(server_id, username),"
            "FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE"
            ")"))) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(createNew));
    }

    QSqlQuery copy(m_database);
    if (!copy.exec(QStringLiteral(
            "INSERT OR REPLACE INTO sessions (server_id, user_id, username, access_token, created_at) "
            "SELECT server_id, user_id, username, access_token, created_at FROM sessions_legacy"))) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(copy));
    }

    QSqlQuery dropOld(m_database);
    if (!dropOld.exec(QStringLiteral("DROP TABLE sessions_legacy"))) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(dropOld));
    }

    QSqlQuery commit(m_database);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        return std::unexpected(sqlError(commit));
    }
    return {};
}

std::expected<void, QString> SessionRepository::migrateDailyUsageStatsTable()
{
    QSqlQuery info(m_database);
    if (!info.exec(QStringLiteral("PRAGMA table_info(daily_usage_stats)"))) {
        return std::unexpected(sqlError(info));
    }

    bool hasRows = false;
    bool hasPrivacyModeColumn = false;
    bool privacyModeInPrimaryKey = false;
    while (info.next()) {
        hasRows = true;
        const auto name = info.value(QStringLiteral("name")).toString();
        if (name == QStringLiteral("privacy_mode")) {
            hasPrivacyModeColumn = true;
            privacyModeInPrimaryKey = info.value(QStringLiteral("pk")).toInt() > 0;
        }
    }

    if (!hasRows) {
        return createDailyUsageStatsTable(m_database);
    }
    if (hasPrivacyModeColumn && privacyModeInPrimaryKey) {
        return {};
    }

    QSqlQuery transaction(m_database);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        return std::unexpected(sqlError(transaction));
    }

    QSqlQuery dropLegacy(m_database);
    if (!dropLegacy.exec(QStringLiteral("DROP TABLE IF EXISTS daily_usage_stats_legacy"))) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(dropLegacy));
    }

    QSqlQuery rename(m_database);
    if (!rename.exec(QStringLiteral("ALTER TABLE daily_usage_stats RENAME TO daily_usage_stats_legacy"))) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(rename));
    }

    if (auto createResult = createDailyUsageStatsTable(m_database); !createResult) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return createResult;
    }

    const auto copySql = hasPrivacyModeColumn
        ? QStringLiteral(
            "INSERT OR REPLACE INTO daily_usage_stats "
            "(stat_date, service_id, service_name, service_type, watch_seconds, network_bytes_in, network_bytes_out, privacy_mode, updated_at) "
            "SELECT stat_date, service_id, service_name, service_type, watch_seconds, network_bytes_in, network_bytes_out, "
            "privacy_mode, updated_at FROM daily_usage_stats_legacy")
        : QStringLiteral(
            "INSERT OR REPLACE INTO daily_usage_stats "
            "(stat_date, service_id, service_name, service_type, watch_seconds, network_bytes_in, network_bytes_out, privacy_mode, updated_at) "
            "SELECT stat_date, service_id, service_name, service_type, watch_seconds, network_bytes_in, network_bytes_out, "
            "0, updated_at FROM daily_usage_stats_legacy");
    QSqlQuery copy(m_database);
    if (!copy.exec(copySql)) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(copy));
    }

    QSqlQuery dropOld(m_database);
    if (!dropOld.exec(QStringLiteral("DROP TABLE daily_usage_stats_legacy"))) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(dropOld));
    }

    QSqlQuery commit(m_database);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        return std::unexpected(sqlError(commit));
    }
    return {};
}

std::expected<std::optional<UserSession>, QString> SessionRepository::sessionFromQuery(QSqlQuery& query)
{
    if (!query.next()) {
        return std::optional<UserSession> {};
    }

    ServerConfig server;
    server.id = query.value(0).toString();
    server.name = query.value(1).toString();
    server.baseUrl = query.value(2).toString();
    server.username = query.value(3).toString();
    server.serviceType = serviceTypeFromString(query.value(4).toString());
    server.trustSelfSignedCertificate = query.value(5).toInt() == 1;
    server.autoLogin = query.value(6).toInt() == 1;
    server.privateMode = query.value(7).toInt() == 1;

    UserSession session;
    session.server = server;
    session.userId = query.value(8).toString();
    session.username = query.value(9).toString();
    session.accessToken = query.value(10).toString();
    session.createdAt = QDateTime::fromString(query.value(11).toString(), Qt::ISODate);
    return std::optional<UserSession> { session };
}
