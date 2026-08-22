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

SessionRepository::SessionRepository(QString connectionName, QString databasePathOverride)
    : m_connectionName(std::move(connectionName))
    , m_databasePathOverride(std::move(databasePathOverride))
    , m_settings(QStringLiteral("vibePlayerQT"), QStringLiteral("vibePlayerQT"))
{
}

SessionRepository::~SessionRepository()
{
    const auto connectionName = m_connectionName;
    if (m_database.isValid()) {
        m_database.close();
        m_database = QSqlDatabase {};
    }
    if (QSqlDatabase::contains(connectionName)) {
        QSqlDatabase::removeDatabase(connectionName);
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

    QSqlQuery linkHistoryQuery(m_database);
    if (!linkHistoryQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS link_playback_history ("
            "id TEXT PRIMARY KEY,"
            "playback_url TEXT NOT NULL,"
            "played_date TEXT NOT NULL,"
            "played_at TEXT NOT NULL,"
            "privacy_mode INTEGER NOT NULL DEFAULT 0"
            ")"))) {
        return std::unexpected(sqlError(linkHistoryQuery));
    }
    if (auto columnResult = ensureColumn(QStringLiteral("link_playback_history"),
                                         QStringLiteral("privacy_mode"),
                                         QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
        !columnResult) {
        return columnResult;
    }
    if (!linkHistoryQuery.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_link_playback_history_played_at "
            "ON link_playback_history(played_at DESC)"))) {
        return std::unexpected(sqlError(linkHistoryQuery));
    }
    if (!linkHistoryQuery.exec(QStringLiteral(
            "DELETE FROM link_playback_history "
            "WHERE EXISTS ("
            "SELECT 1 FROM link_playback_history newer "
            "WHERE newer.playback_url = link_playback_history.playback_url "
            "AND (newer.played_at > link_playback_history.played_at "
            "OR (newer.played_at = link_playback_history.played_at AND newer.id > link_playback_history.id))"
            ")"))) {
        return std::unexpected(sqlError(linkHistoryQuery));
    }
    if (!linkHistoryQuery.exec(QStringLiteral(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_link_playback_history_unique_url "
            "ON link_playback_history(playback_url)"))) {
        return std::unexpected(sqlError(linkHistoryQuery));
    }

    QSqlQuery playbackHistoryQuery(m_database);
    if (!playbackHistoryQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS playback_history ("
            "id TEXT PRIMARY KEY,"
            "source_type TEXT NOT NULL,"
            "service_id TEXT NOT NULL DEFAULT '',"
            "service_name TEXT NOT NULL DEFAULT '',"
            "replay_target TEXT NOT NULL,"
            "title TEXT NOT NULL,"
            "subtitle TEXT NOT NULL DEFAULT '',"
            "played_date TEXT NOT NULL,"
            "played_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL,"
            "position_seconds INTEGER NOT NULL DEFAULT 0,"
            "duration_seconds INTEGER NOT NULL DEFAULT 0,"
            "completed INTEGER NOT NULL DEFAULT 0,"
            "privacy_mode INTEGER NOT NULL DEFAULT 0"
            ")"))) {
        return std::unexpected(sqlError(playbackHistoryQuery));
    }
    if (!playbackHistoryQuery.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_playback_history_played_at "
            "ON playback_history(played_at DESC)"))) {
        return std::unexpected(sqlError(playbackHistoryQuery));
    }
    if (!playbackHistoryQuery.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_playback_history_source "
            "ON playback_history(source_type, played_at DESC)"))) {
        return std::unexpected(sqlError(playbackHistoryQuery));
    }
    if (!playbackHistoryQuery.exec(QStringLiteral(
            "INSERT OR IGNORE INTO playback_history "
            "(id, source_type, service_id, service_name, replay_target, title, subtitle, played_date, played_at, updated_at, "
            "position_seconds, duration_seconds, completed, privacy_mode) "
            "SELECT id, 'Link', 'builtin-link-playback', 'Link Playback', playback_url, 'Link Playback', '', "
            "played_date, played_at, played_at, 0, 0, 0, privacy_mode FROM link_playback_history"))) {
        return std::unexpected(sqlError(playbackHistoryQuery));
    }
    if (!playbackHistoryQuery.exec(QStringLiteral(
            "DELETE FROM playback_history "
            "WHERE EXISTS ("
            "SELECT 1 FROM playback_history newer "
            "WHERE newer.source_type = playback_history.source_type "
            "AND newer.service_id = playback_history.service_id "
            "AND newer.replay_target = playback_history.replay_target "
            "AND (newer.played_at > playback_history.played_at "
            "OR (newer.played_at = playback_history.played_at AND newer.id > playback_history.id))"
            ")"))) {
        return std::unexpected(sqlError(playbackHistoryQuery));
    }
    if (!playbackHistoryQuery.exec(QStringLiteral(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_playback_history_unique_item "
            "ON playback_history(source_type, service_id, replay_target)"))) {
        return std::unexpected(sqlError(playbackHistoryQuery));
    }

    QSqlQuery scheduledTaskQuery(m_database);
    if (!scheduledTaskQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS scheduled_playback_tasks ("
            "id TEXT PRIMARY KEY,"
            "server_id TEXT NOT NULL,"
            "schedule_type TEXT NOT NULL DEFAULT 'manual',"
            "start_time TEXT NOT NULL,"
            "schedule_days TEXT NOT NULL DEFAULT '',"
            "duration_minutes INTEGER NOT NULL DEFAULT 90,"
            "enabled INTEGER NOT NULL DEFAULT 1,"
            "last_run_date TEXT NOT NULL DEFAULT '',"
            "created_at TEXT NOT NULL,"
            "updated_at TEXT NOT NULL,"
            "FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE"
            ")"))) {
        return std::unexpected(sqlError(scheduledTaskQuery));
    }
    if (auto columnResult = ensureColumn(QStringLiteral("scheduled_playback_tasks"),
                                         QStringLiteral("schedule_type"),
                                         QStringLiteral("TEXT NOT NULL DEFAULT 'manual'"));
        !columnResult) {
        return columnResult;
    }
    if (auto columnResult = ensureColumn(QStringLiteral("scheduled_playback_tasks"),
                                         QStringLiteral("schedule_days"),
                                         QStringLiteral("TEXT NOT NULL DEFAULT ''"));
        !columnResult) {
        return columnResult;
    }

    QSqlQuery localMediaRootsQuery(m_database);
    if (!localMediaRootsQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS local_media_roots ("
            "id TEXT PRIMARY KEY,"
            "name TEXT NOT NULL,"
            "path TEXT NOT NULL UNIQUE,"
            "sort_order INTEGER NOT NULL DEFAULT 0,"
            "created_at TEXT NOT NULL"
            ")"))) {
        return std::unexpected(sqlError(localMediaRootsQuery));
    }

    QSqlQuery recommendationCacheQuery(m_database);
    if (!recommendationCacheQuery.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS emby_recommendation_cache ("
            "server_id TEXT NOT NULL,"
            "user_id TEXT NOT NULL,"
            "payload BLOB NOT NULL,"
            "refreshed_at TEXT NOT NULL,"
            "PRIMARY KEY(server_id, user_id),"
            "FOREIGN KEY(server_id) REFERENCES servers(id) ON DELETE CASCADE"
            ")"))) {
        return std::unexpected(sqlError(recommendationCacheQuery));
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

std::expected<std::vector<LocalMediaRoot>, QString> SessionRepository::loadLocalMediaRoots()
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id, name, path, sort_order "
            "FROM local_media_roots ORDER BY sort_order ASC, created_at ASC"))) {
        return std::unexpected(sqlError(query));
    }

    std::vector<LocalMediaRoot> roots;
    while (query.next()) {
        roots.push_back(LocalMediaRoot {
            .id = query.value(0).toString(),
            .name = query.value(1).toString(),
            .path = query.value(2).toString(),
            .sortOrder = query.value(3).toInt(),
        });
    }
    return roots;
}

std::expected<void, QString> SessionRepository::saveLocalMediaRoot(const LocalMediaRoot& root)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO local_media_roots (id, name, path, sort_order, created_at) "
        "VALUES (:id, :name, :path, :sort_order, :created_at) "
        "ON CONFLICT(id) DO UPDATE SET "
        "name = excluded.name, path = excluded.path, sort_order = excluded.sort_order"));
    query.bindValue(QStringLiteral(":id"), root.id);
    query.bindValue(QStringLiteral(":name"), root.name);
    query.bindValue(QStringLiteral(":path"), root.path);
    query.bindValue(QStringLiteral(":sort_order"), root.sortOrder);
    query.bindValue(QStringLiteral(":created_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<void, QString> SessionRepository::deleteLocalMediaRoot(const QString& rootId)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM local_media_roots WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), rootId);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
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

std::expected<void, QString> SessionRepository::saveLinkPlaybackHistory(const LinkPlaybackHistoryItem& item)
{
    if (item.id.trimmed().isEmpty() || !item.playbackUrl.isValid() || item.playbackUrl.isRelative() ||
        !item.playedDate.isValid() || !item.playedAt.isValid()) {
        return std::unexpected(QStringLiteral("Invalid link playback history item"));
    }
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO link_playback_history (id, playback_url, played_date, played_at, privacy_mode) "
        "VALUES (:id, :playback_url, :played_date, :played_at, :privacy_mode)"));
    query.bindValue(QStringLiteral(":id"), item.id);
    query.bindValue(QStringLiteral(":playback_url"), item.playbackUrl.toString(QUrl::FullyEncoded));
    query.bindValue(QStringLiteral(":played_date"), item.playedDate.toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":played_at"), item.playedAt.toUTC().toString(Qt::ISODateWithMs));
    query.bindValue(QStringLiteral(":privacy_mode"), item.privacyMode ? 1 : 0);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<std::vector<LinkPlaybackHistoryItem>, QString> SessionRepository::loadLinkPlaybackHistory(bool includePrivacyMode)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, playback_url, played_date, played_at, privacy_mode "
        "FROM link_playback_history "
        "WHERE (:include_privacy = 1 OR privacy_mode = 0) "
        "ORDER BY played_at DESC, id DESC"));
    query.bindValue(QStringLiteral(":include_privacy"), includePrivacyMode ? 1 : 0);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }

    std::vector<LinkPlaybackHistoryItem> items;
    while (query.next()) {
        const auto playedAtText = query.value(3).toString();
        auto playedAt = QDateTime::fromString(playedAtText, Qt::ISODateWithMs);
        if (!playedAt.isValid()) {
            playedAt = QDateTime::fromString(playedAtText, Qt::ISODate);
        }
        items.push_back(LinkPlaybackHistoryItem {
            .id = query.value(0).toString(),
            .playbackUrl = QUrl(query.value(1).toString(), QUrl::StrictMode),
            .playedDate = QDate::fromString(query.value(2).toString(), Qt::ISODate),
            .playedAt = playedAt,
            .privacyMode = query.value(4).toInt() == 1,
        });
    }
    return items;
}

std::expected<void, QString> SessionRepository::deleteLinkPlaybackHistory(const QString& recordId)
{
    if (recordId.trimmed().isEmpty()) {
        return std::unexpected(QStringLiteral("Invalid link playback history id"));
    }
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM link_playback_history WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), recordId);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<void, QString> SessionRepository::savePlaybackHistory(const PlaybackHistoryItem& item)
{
    if (item.id.trimmed().isEmpty() || item.source == PlaybackHistorySource::Unknown ||
        item.replayTarget.trimmed().isEmpty() || item.title.trimmed().isEmpty() ||
        !item.playedDate.isValid() || !item.playedAt.isValid() || !item.updatedAt.isValid()) {
        return std::unexpected(QStringLiteral("Invalid playback history item"));
    }
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO playback_history "
        "(id, source_type, service_id, service_name, replay_target, title, subtitle, played_date, played_at, updated_at, "
        "position_seconds, duration_seconds, completed, privacy_mode) "
        "VALUES (:id, :source_type, :service_id, :service_name, :replay_target, :title, :subtitle, :played_date, "
        ":played_at, :updated_at, :position_seconds, :duration_seconds, :completed, :privacy_mode)"));
    query.bindValue(QStringLiteral(":id"), item.id);
    query.bindValue(QStringLiteral(":source_type"), playbackHistorySourceToString(item.source));
    query.bindValue(QStringLiteral(":service_id"), nonNullText(item.serviceId));
    query.bindValue(QStringLiteral(":service_name"), nonNullText(item.serviceName));
    query.bindValue(QStringLiteral(":replay_target"), item.replayTarget);
    query.bindValue(QStringLiteral(":title"), item.title);
    query.bindValue(QStringLiteral(":subtitle"), nonNullText(item.subtitle));
    query.bindValue(QStringLiteral(":played_date"), item.playedDate.toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":played_at"), item.playedAt.toUTC().toString(Qt::ISODateWithMs));
    query.bindValue(QStringLiteral(":updated_at"), item.updatedAt.toUTC().toString(Qt::ISODateWithMs));
    query.bindValue(QStringLiteral(":position_seconds"), std::max<qint64>(0, item.positionSeconds));
    query.bindValue(QStringLiteral(":duration_seconds"), std::max<qint64>(0, item.durationSeconds));
    query.bindValue(QStringLiteral(":completed"), item.completed ? 1 : 0);
    query.bindValue(QStringLiteral(":privacy_mode"), item.privacyMode ? 1 : 0);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<std::vector<PlaybackHistoryItem>, QString> SessionRepository::loadPlaybackHistory(
    bool includePrivacyMode,
    int startIndex,
    int limit,
    PlaybackHistorySource source)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT history.id, history.source_type, history.service_id, history.service_name, history.replay_target, "
        "history.title, history.subtitle, history.played_date, history.played_at, history.updated_at, "
        "history.position_seconds, history.duration_seconds, history.completed, "
        "COALESCE(servers.private_mode, history.privacy_mode) "
        "FROM playback_history history "
        "LEFT JOIN servers ON servers.id = history.service_id "
        "WHERE (:include_privacy = 1 OR COALESCE(servers.private_mode, history.privacy_mode) = 0) "
        "AND (:source_type = '' OR history.source_type = :source_type) "
        "ORDER BY history.played_at DESC, history.id DESC "
        "LIMIT :limit OFFSET :offset"));
    query.bindValue(QStringLiteral(":include_privacy"), includePrivacyMode ? 1 : 0);
    query.bindValue(QStringLiteral(":source_type"), source == PlaybackHistorySource::Unknown
            ? QStringLiteral("")
            : playbackHistorySourceToString(source));
    query.bindValue(QStringLiteral(":limit"), std::clamp(limit, 1, 500));
    query.bindValue(QStringLiteral(":offset"), std::max(0, startIndex));
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }

    std::vector<PlaybackHistoryItem> items;
    while (query.next()) {
        const auto parseDateTime = [](const QString& value) {
            auto dateTime = QDateTime::fromString(value, Qt::ISODateWithMs);
            if (!dateTime.isValid()) {
                dateTime = QDateTime::fromString(value, Qt::ISODate);
            }
            return dateTime;
        };
        items.push_back(PlaybackHistoryItem {
            .id = query.value(0).toString(),
            .source = playbackHistorySourceFromString(query.value(1).toString()),
            .serviceId = query.value(2).toString(),
            .serviceName = query.value(3).toString(),
            .replayTarget = query.value(4).toString(),
            .title = query.value(5).toString(),
            .subtitle = query.value(6).toString(),
            .playedDate = QDate::fromString(query.value(7).toString(), Qt::ISODate),
            .playedAt = parseDateTime(query.value(8).toString()),
            .updatedAt = parseDateTime(query.value(9).toString()),
            .positionSeconds = query.value(10).toLongLong(),
            .durationSeconds = query.value(11).toLongLong(),
            .completed = query.value(12).toInt() == 1,
            .privacyMode = query.value(13).toInt() == 1,
        });
    }
    return items;
}

std::expected<void, QString> SessionRepository::updatePlaybackHistoryProgress(const QString& recordId,
                                                                               qint64 positionSeconds,
                                                                               qint64 durationSeconds,
                                                                               bool completed,
                                                                               const QDateTime& updatedAt)
{
    if (recordId.trimmed().isEmpty() || !updatedAt.isValid()) {
        return std::unexpected(QStringLiteral("Invalid playback history progress"));
    }
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE playback_history SET position_seconds = :position_seconds, duration_seconds = :duration_seconds, "
        "completed = :completed, updated_at = :updated_at WHERE id = :id"));
    query.bindValue(QStringLiteral(":position_seconds"), std::max<qint64>(0, positionSeconds));
    query.bindValue(QStringLiteral(":duration_seconds"), std::max<qint64>(0, durationSeconds));
    query.bindValue(QStringLiteral(":completed"), completed ? 1 : 0);
    query.bindValue(QStringLiteral(":updated_at"), updatedAt.toUTC().toString(Qt::ISODateWithMs));
    query.bindValue(QStringLiteral(":id"), recordId);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<void, QString> SessionRepository::deletePlaybackHistory(const QString& recordId)
{
    if (recordId.trimmed().isEmpty()) {
        return std::unexpected(QStringLiteral("Invalid playback history id"));
    }
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery transaction(m_database);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        return std::unexpected(sqlError(transaction));
    }
    QSqlQuery historyQuery(m_database);
    historyQuery.prepare(QStringLiteral("DELETE FROM playback_history WHERE id = :id"));
    historyQuery.bindValue(QStringLiteral(":id"), recordId);
    if (!historyQuery.exec()) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(historyQuery));
    }
    QSqlQuery linkQuery(m_database);
    linkQuery.prepare(QStringLiteral("DELETE FROM link_playback_history WHERE id = :id"));
    linkQuery.bindValue(QStringLiteral(":id"), recordId);
    if (!linkQuery.exec()) {
        transaction.exec(QStringLiteral("ROLLBACK"));
        return std::unexpected(sqlError(linkQuery));
    }
    QSqlQuery commit(m_database);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        return std::unexpected(sqlError(commit));
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
            "SELECT s.id, s.name, "
            "CASE WHEN s.service_type = 'IPTV' AND p.imported_path <> '' THEN p.imported_path ELSE s.base_url END, "
            "s.username, s.service_type, s.trust_self_signed, "
            "s.auto_login, s.last_used_at, s.private_mode, sess.access_token "
            "FROM servers s "
            "LEFT JOIN sessions sess ON sess.server_id = s.id AND sess.username = s.username "
            "LEFT JOIN iptv_playlists p ON p.service_id = s.id "
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
            "SELECT s.id, s.name, "
            "CASE WHEN s.service_type = 'IPTV' AND p.imported_path <> '' THEN p.imported_path ELSE s.base_url END, "
            "s.username, s.service_type, s.trust_self_signed, "
            "s.auto_login, s.last_used_at, s.private_mode, sess.access_token "
            "FROM servers s "
            "LEFT JOIN sessions sess ON sess.server_id = s.id AND sess.username = s.username "
            "LEFT JOIN iptv_playlists p ON p.service_id = s.id "
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
            "SELECT t.id, t.server_id, s.name, s.username, t.schedule_type, t.start_time, t.schedule_days, "
            "t.duration_minutes, t.enabled, t.last_run_date, s.private_mode, t.created_at "
            "FROM scheduled_playback_tasks t "
            "JOIN servers s ON s.id = t.server_id "
            "WHERE s.enabled = 1 AND s.service_type = 'Emby' "
            "AND (:include_private = 1 OR s.private_mode = 0) "
            "ORDER BY t.enabled DESC, t.schedule_type ASC, t.start_time ASC, s.name COLLATE NOCASE ASC"));
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
            .scheduleType = query.value(4).toString(),
            .startTime = query.value(5).toString(),
            .scheduleDays = query.value(6).toString(),
            .durationMinutes = query.value(7).toInt(),
            .enabled = query.value(8).toInt() == 1,
            .lastRunDate = query.value(9).toString(),
            .createdAt = query.value(11).toString(),
            .privateMode = query.value(10).toInt() == 1,
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
        "(id, server_id, schedule_type, start_time, schedule_days, duration_minutes, enabled, last_run_date, created_at, updated_at) "
        "VALUES (:id, :server_id, :schedule_type, :start_time, :schedule_days, :duration_minutes, :enabled, :last_run_date, :created_at, :updated_at) "
        "ON CONFLICT(id) DO UPDATE SET "
        "server_id = excluded.server_id, "
        "schedule_type = excluded.schedule_type, "
        "start_time = excluded.start_time, "
        "schedule_days = excluded.schedule_days, "
        "duration_minutes = excluded.duration_minutes, "
        "enabled = excluded.enabled, "
        "last_run_date = excluded.last_run_date, "
        "updated_at = excluded.updated_at"));
    query.bindValue(QStringLiteral(":id"), task.id);
    query.bindValue(QStringLiteral(":server_id"), task.serverId);
    query.bindValue(QStringLiteral(":schedule_type"), task.scheduleType);
    query.bindValue(QStringLiteral(":start_time"), task.startTime);
    query.bindValue(QStringLiteral(":schedule_days"), nonNullText(task.scheduleDays));
    query.bindValue(QStringLiteral(":duration_minutes"), task.durationMinutes);
    query.bindValue(QStringLiteral(":enabled"), task.enabled ? 1 : 0);
    query.bindValue(QStringLiteral(":last_run_date"), nonNullText(task.lastRunDate));
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

QString SessionRepository::scheduledPlaybackCheckpoint(bool privateMode) const
{
    const auto key = privateMode
        ? QStringLiteral("scheduledPlayback/privateCheckpoint")
        : QStringLiteral("scheduledPlayback/normalCheckpoint");
    return m_settings.value(key).toString();
}

void SessionRepository::setScheduledPlaybackCheckpoint(bool privateMode, const QString& timestamp)
{
    const auto key = privateMode
        ? QStringLiteral("scheduledPlayback/privateCheckpoint")
        : QStringLiteral("scheduledPlayback/normalCheckpoint");
    m_settings.setValue(key, timestamp);
}

QStringList SessionRepository::pendingMissedScheduledPlaybackTaskIds() const
{
    return m_settings.value(QStringLiteral("scheduledPlayback/pendingMissedTaskIds")).toStringList();
}

void SessionRepository::setPendingMissedScheduledPlaybackTaskIds(const QStringList& taskIds)
{
    m_settings.setValue(QStringLiteral("scheduledPlayback/pendingMissedTaskIds"), taskIds);
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

std::expected<std::optional<RecommendationCacheRecord>, QString> SessionRepository::loadEmbyRecommendationCache(
    const QString& serverId,
    const QString& userId)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return std::unexpected(openResult.error());
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT payload, refreshed_at FROM emby_recommendation_cache "
        "WHERE server_id = :server_id AND user_id = :user_id LIMIT 1"));
    query.bindValue(QStringLiteral(":server_id"), serverId);
    query.bindValue(QStringLiteral(":user_id"), userId);
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    if (!query.next()) {
        return std::optional<RecommendationCacheRecord> {};
    }

    const auto refreshedAt = QDateTime::fromString(query.value(1).toString(), Qt::ISODateWithMs);
    if (!refreshedAt.isValid()) {
        return std::unexpected(QStringLiteral("Stored Emby recommendation refresh time is invalid"));
    }
    return std::optional<RecommendationCacheRecord> { RecommendationCacheRecord {
        .payload = query.value(0).toByteArray(),
        .refreshedAt = refreshedAt,
    } };
}

std::expected<void, QString> SessionRepository::saveEmbyRecommendationCache(const QString& serverId,
                                                                              const QString& userId,
                                                                              const QByteArray& payload,
                                                                              const QDateTime& refreshedAt)
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO emby_recommendation_cache (server_id, user_id, payload, refreshed_at) "
        "VALUES (:server_id, :user_id, :payload, :refreshed_at) "
        "ON CONFLICT(server_id, user_id) DO UPDATE SET "
        "payload = excluded.payload, refreshed_at = excluded.refreshed_at"));
    query.bindValue(QStringLiteral(":server_id"), serverId);
    query.bindValue(QStringLiteral(":user_id"), userId);
    query.bindValue(QStringLiteral(":payload"), payload);
    query.bindValue(QStringLiteral(":refreshed_at"), refreshedAt.toUTC().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        return std::unexpected(sqlError(query));
    }
    return {};
}

std::expected<void, QString> SessionRepository::clearEmbyRecommendationCaches()
{
    if (auto openResult = ensureOpen(); !openResult) {
        return openResult;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("DELETE FROM emby_recommendation_cache"))) {
        return std::unexpected(sqlError(query));
    }
    return {};
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

    {
        QSqlQuery historyQuery(m_database);
        historyQuery.prepare(QStringLiteral("DELETE FROM playback_history WHERE service_id = :server_id"));
        historyQuery.bindValue(QStringLiteral(":server_id"), serverId);
        if (!historyQuery.exec()) {
            return std::unexpected(sqlError(historyQuery));
        }
    }

    {
        QSqlQuery recommendationQuery(m_database);
        recommendationQuery.prepare(QStringLiteral("DELETE FROM emby_recommendation_cache WHERE server_id = :server_id"));
        recommendationQuery.bindValue(QStringLiteral(":server_id"), serverId);
        if (!recommendationQuery.exec()) {
            return std::unexpected(sqlError(recommendationQuery));
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

QString SessionRepository::embyHomeLayout() const
{
    return m_settings.value(QStringLiteral("appearance/embyHomeLayout"), QStringLiteral("trendy")).toString();
}

void SessionRepository::setEmbyHomeLayout(const QString& layout)
{
    m_settings.setValue(QStringLiteral("appearance/embyHomeLayout"), layout);
}

QString SessionRepository::jellyfinHomeLayout() const
{
    return m_settings.value(QStringLiteral("appearance/jellyfinHomeLayout"), QStringLiteral("trendy")).toString();
}

void SessionRepository::setJellyfinHomeLayout(const QString& layout)
{
    m_settings.setValue(QStringLiteral("appearance/jellyfinHomeLayout"), layout);
}

QString SessionRepository::playerLayout() const
{
    return m_settings.value(QStringLiteral("appearance/playerLayout"), QStringLiteral("trendy")).toString();
}

void SessionRepository::setPlayerLayout(const QString& layout)
{
    m_settings.setValue(QStringLiteral("appearance/playerLayout"), layout);
}

bool SessionRepository::pageTransitionsEnabled() const
{
    return m_settings.value(QStringLiteral("appearance/pageTransitionsEnabled"), true).toBool();
}

void SessionRepository::setPageTransitionsEnabled(bool enabled)
{
    m_settings.setValue(QStringLiteral("appearance/pageTransitionsEnabled"), enabled);
}

QStringList SessionRepository::embyRecommendationExcludedGenres() const
{
    return m_settings.value(QStringLiteral("recommendations/embyExcludedGenres")).toStringList();
}

void SessionRepository::setEmbyRecommendationExcludedGenres(const QStringList& genres)
{
    m_settings.setValue(QStringLiteral("recommendations/embyExcludedGenres"), genres);
}

QStringList SessionRepository::embyRecommendationAvailableGenres() const
{
    return m_settings.value(QStringLiteral("recommendations/embyAvailableGenres")).toStringList();
}

void SessionRepository::setEmbyRecommendationAvailableGenres(const QStringList& genres)
{
    m_settings.setValue(QStringLiteral("recommendations/embyAvailableGenres"), genres);
}

QString SessionRepository::defaultDownloadDirectory() const
{
    return m_settings.value(QStringLiteral("webdav/defaultDownloadDirectory")).toString();
}

void SessionRepository::setDefaultDownloadDirectory(const QString& directory)
{
    m_settings.setValue(QStringLiteral("webdav/defaultDownloadDirectory"), directory);
}

bool SessionRepository::webDavShowM3u8sIdentifier() const
{
    return m_settings.value(QStringLiteral("webdav/showM3u8sIdentifier"), true).toBool();
}

void SessionRepository::setWebDavShowM3u8sIdentifier(bool enabled)
{
    m_settings.setValue(QStringLiteral("webdav/showM3u8sIdentifier"), enabled);
}

bool SessionRepository::webDavShowM3u8sSourceFileName() const
{
    return m_settings.value(QStringLiteral("webdav/showM3u8sSourceFileName"), true).toBool();
}

void SessionRepository::setWebDavShowM3u8sSourceFileName(bool enabled)
{
    m_settings.setValue(QStringLiteral("webdav/showM3u8sSourceFileName"), enabled);
}

QString SessionRepository::tsslBackupTarget() const
{
    return m_settings.value(QStringLiteral("tsslBackup/target"), QStringLiteral("none")).toString();
}

void SessionRepository::setTsslBackupTarget(const QString& target)
{
    m_settings.setValue(QStringLiteral("tsslBackup/target"), target);
}

QString SessionRepository::tsslBackupWebDavServiceId() const
{
    return m_settings.value(QStringLiteral("tsslBackup/webDavServiceId")).toString();
}

void SessionRepository::setTsslBackupWebDavServiceId(const QString& serviceId)
{
    m_settings.setValue(QStringLiteral("tsslBackup/webDavServiceId"), serviceId);
}

QString SessionRepository::tsslBackupWebDavPath() const
{
    return m_settings.value(QStringLiteral("tsslBackup/webDavPath"), QStringLiteral("vibePlayerQT/tssl")).toString();
}

void SessionRepository::setTsslBackupWebDavPath(const QString& path)
{
    m_settings.setValue(QStringLiteral("tsslBackup/webDavPath"), path);
}

QString SessionRepository::tsslBackupS3Endpoint() const
{
    return m_settings.value(QStringLiteral("tsslBackup/s3Endpoint")).toString();
}

void SessionRepository::setTsslBackupS3Endpoint(const QString& endpoint)
{
    m_settings.setValue(QStringLiteral("tsslBackup/s3Endpoint"), endpoint);
}

QString SessionRepository::tsslBackupS3Bucket() const
{
    return m_settings.value(QStringLiteral("tsslBackup/s3Bucket")).toString();
}

void SessionRepository::setTsslBackupS3Bucket(const QString& bucket)
{
    m_settings.setValue(QStringLiteral("tsslBackup/s3Bucket"), bucket);
}

QString SessionRepository::tsslBackupS3Region() const
{
    return m_settings.value(QStringLiteral("tsslBackup/s3Region"), QStringLiteral("us-east-1")).toString();
}

void SessionRepository::setTsslBackupS3Region(const QString& region)
{
    m_settings.setValue(QStringLiteral("tsslBackup/s3Region"), region);
}

QString SessionRepository::tsslBackupS3Prefix() const
{
    return m_settings.value(QStringLiteral("tsslBackup/s3Prefix"), QStringLiteral("vibePlayerQT/tssl")).toString();
}

void SessionRepository::setTsslBackupS3Prefix(const QString& prefix)
{
    m_settings.setValue(QStringLiteral("tsslBackup/s3Prefix"), prefix);
}

QString SessionRepository::tsslBackupS3AccessKey() const
{
    return m_settings.value(QStringLiteral("tsslBackup/s3AccessKey")).toString();
}

void SessionRepository::setTsslBackupS3AccessKey(const QString& accessKey)
{
    m_settings.setValue(QStringLiteral("tsslBackup/s3AccessKey"), accessKey);
}

QString SessionRepository::m3u8sOutputDirectory() const
{
    return m_settings.value(QStringLiteral("m3u8s/outputDirectory")).toString();
}

void SessionRepository::setM3u8sOutputDirectory(const QString& directory)
{
    m_settings.setValue(QStringLiteral("m3u8s/outputDirectory"), directory);
}

QString SessionRepository::m3u8sVideoEncoding() const
{
    return m_settings.value(QStringLiteral("m3u8s/videoEncoding"), QStringLiteral("h264")).toString();
}

void SessionRepository::setM3u8sVideoEncoding(const QString& encoding)
{
    m_settings.setValue(QStringLiteral("m3u8s/videoEncoding"), encoding);
}

QString SessionRepository::m3u8sAudioEncoding() const
{
    return m_settings.value(QStringLiteral("m3u8s/audioEncoding"), QStringLiteral("aac")).toString();
}

void SessionRepository::setM3u8sAudioEncoding(const QString& encoding)
{
    m_settings.setValue(QStringLiteral("m3u8s/audioEncoding"), encoding);
}

QString SessionRepository::m3u8sVideoQuality() const
{
    return m_settings.value(QStringLiteral("m3u8s/videoQuality"), QStringLiteral("balanced")).toString();
}

void SessionRepository::setM3u8sVideoQuality(const QString& quality)
{
    m_settings.setValue(QStringLiteral("m3u8s/videoQuality"), quality);
}

QString SessionRepository::updateChannel() const
{
    return m_settings.value(QStringLiteral("updates/channel"), QStringLiteral("stable")).toString();
}

void SessionRepository::setUpdateChannel(const QString& channel)
{
    m_settings.setValue(QStringLiteral("updates/channel"), channel);
}

bool SessionRepository::automaticUpdateCheck() const
{
    return m_settings.value(QStringLiteral("updates/automaticCheck"), true).toBool();
}

void SessionRepository::setAutomaticUpdateCheck(bool enabled)
{
    m_settings.setValue(QStringLiteral("updates/automaticCheck"), enabled);
}

QDateTime SessionRepository::updateLastCheckedAt() const
{
    return QDateTime::fromString(m_settings.value(QStringLiteral("updates/lastCheckedAt")).toString(), Qt::ISODate);
}

void SessionRepository::setUpdateLastCheckedAt(const QDateTime& value)
{
    m_settings.setValue(QStringLiteral("updates/lastCheckedAt"), value.toUTC().toString(Qt::ISODate));
}

QByteArray SessionRepository::updateEtag() const
{
    return m_settings.value(QStringLiteral("updates/etag")).toByteArray();
}

void SessionRepository::setUpdateEtag(const QByteArray& value)
{
    m_settings.setValue(QStringLiteral("updates/etag"), value);
}

QString SessionRepository::updateLastVersion() const
{
    return m_settings.value(QStringLiteral("updates/lastVersion")).toString();
}

void SessionRepository::setUpdateLastVersion(const QString& value)
{
    m_settings.setValue(QStringLiteral("updates/lastVersion"), value);
}

QString SessionRepository::databasePath() const
{
    if (!m_databasePathOverride.isEmpty()) {
        const QFileInfo overrideInfo(m_databasePathOverride);
        QDir().mkpath(overrideInfo.absolutePath());
        return overrideInfo.absoluteFilePath();
    }
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
