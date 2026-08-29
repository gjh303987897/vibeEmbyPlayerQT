#include "database/SessionRepository.h"
#include "viewmodels/PlaybackHistoryListModel.h"
#include "TimeFixtures.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace TimeFixtures;

class PlaybackHistoryTest final : public QObject {
    Q_OBJECT

private slots:
    void persistsFiltersUpdatesAndDeletesRecords();
    void supportsDateManagement();
    void configuresHistoryRetentionAndPrunesExpiredRecords();
    void keepsSameTargetFromDifferentServices();
    void migratesExistingLinkHistory();
    void deduplicatesExistingGlobalHistory();
    void usesManagedPathForIptvServiceCards();
    void persistsAndClearsEmbyRecommendationCache();
};

namespace {
QString uniqueConnectionName()
{
    return QStringLiteral("playback-history-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

PlaybackHistoryItem historyItem(const QString& id,
                                PlaybackHistorySource source,
                                const QString& target,
                                const QString& timestamp,
                                bool privacyMode = false)
{
    const auto playedAt = QDateTime::fromString(timestamp, Qt::ISODate);
    return PlaybackHistoryItem {
        .id = id,
        .source = source,
        .serviceId = source == PlaybackHistorySource::Local ? QString {} : QStringLiteral("service-1"),
        .serviceName = source == PlaybackHistorySource::Local ? QStringLiteral("Local Playback") : QStringLiteral("Server One"),
        .replayTarget = target,
        .title = id,
        .subtitle = QStringLiteral("subtitle"),
        .playedDate = playedAt.toLocalTime().date(),
        .playedAt = playedAt,
        .updatedAt = playedAt,
        .privacyMode = privacyMode,
    };
}
}

void PlaybackHistoryTest::persistsFiltersUpdatesAndDeletesRecords()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SessionRepository repository(uniqueConnectionName(), directory.filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(repository.initialize().has_value());

    const auto older = historyItem(QStringLiteral("older"),
                                   PlaybackHistorySource::Local,
                                   QStringLiteral("C:/Media/older.mkv"),
                                   stampAt(2, 8, 0));
    const auto newer = historyItem(QStringLiteral("newer"),
                                   PlaybackHistorySource::Emby,
                                   QStringLiteral("item-42"),
                                   stampAt(1, 9, 30));
    const auto privateItem = historyItem(QStringLiteral("private"),
                                         PlaybackHistorySource::Link,
                                         QStringLiteral("https://example.com/private.m3u8"),
                                         stampAt(0, 10, 0),
                                         true);
    const auto latestSameItem = historyItem(QStringLiteral("latest-same-item"),
                                            PlaybackHistorySource::Emby,
                                            QStringLiteral("item-42"),
                                            stampAt(1, 11, 0));
    QVERIFY(repository.savePlaybackHistory(older).has_value());
    QVERIFY(repository.savePlaybackHistory(newer).has_value());
    QVERIFY(repository.savePlaybackHistory(latestSameItem).has_value());
    QVERIFY(repository.savePlaybackHistory(privateItem).has_value());

    const auto normalItems = repository.loadPlaybackHistory(false, 0, 20);
    QVERIFY(normalItems.has_value());
    QCOMPARE(normalItems->size(), size_t { 2 });
    QCOMPARE(normalItems->front().id, QStringLiteral("latest-same-item"));

    const auto allItems = repository.loadPlaybackHistory(true, 0, 20);
    QVERIFY(allItems.has_value());
    QCOMPARE(allItems->size(), size_t { 3 });
    QCOMPARE(allItems->front().id, QStringLiteral("private"));

    const auto localItems = repository.loadPlaybackHistory(true, 0, 20, PlaybackHistorySource::Local);
    QVERIFY(localItems.has_value());
    QCOMPARE(localItems->size(), size_t { 1 });
    QCOMPARE(localItems->front().id, QStringLiteral("older"));

    const auto firstPage = repository.loadPlaybackHistory(true, 0, 1);
    const auto secondPage = repository.loadPlaybackHistory(true, 1, 1);
    QVERIFY(firstPage.has_value());
    QVERIFY(secondPage.has_value());
    QCOMPARE(firstPage->size(), size_t { 1 });
    QCOMPARE(secondPage->size(), size_t { 1 });
    QCOMPARE(firstPage->front().id, QStringLiteral("private"));
    QCOMPARE(secondPage->front().id, QStringLiteral("latest-same-item"));

    QVERIFY(repository.updatePlaybackHistoryProgress(QStringLiteral("latest-same-item"),
                                                     315,
                                                     600,
                                                     true,
                                                     playedAt(1, 11, 6))
                .has_value());
    const auto updated = repository.loadPlaybackHistory(true, 0, 20, PlaybackHistorySource::Emby);
    QVERIFY(updated.has_value());
    QCOMPARE(updated->front().positionSeconds, qint64 { 315 });
    QCOMPARE(updated->front().durationSeconds, qint64 { 600 });
    QVERIFY(updated->front().completed);

    PlaybackHistoryListModel model;
    model.setItems(*allItems);
    QCOMPARE(model.count(), 3);
    QVERIFY(model.itemById(QStringLiteral("latest-same-item")) != nullptr);

    QVERIFY(repository.deletePlaybackHistory(QStringLiteral("latest-same-item")).has_value());
    const auto afterDelete = repository.loadPlaybackHistory(true, 0, 20);
    QVERIFY(afterDelete.has_value());
    QCOMPARE(afterDelete->size(), size_t { 2 });
}

void PlaybackHistoryTest::keepsSameTargetFromDifferentServices()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    SessionRepository repository(uniqueConnectionName(), directory.filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(repository.initialize().has_value());

    PlaybackHistoryItem first = historyItem(
        QStringLiteral("first-service"),
        PlaybackHistorySource::Emby,
        QStringLiteral("item-42"),
        stampAt(1, 10, 0));
    first.serviceId = QStringLiteral("emby-service-1");
    first.serviceName = QStringLiteral("Emby One");
    first.title = QStringLiteral("Shared Movie");

    PlaybackHistoryItem second = first;
    second.id = QStringLiteral("second-service");
    second.serviceId = QStringLiteral("emby-service-2");
    second.serviceName = QStringLiteral("Emby Two");
    second.playedAt = playedAt(1, 11, 0);

    QVERIFY(repository.savePlaybackHistory(first).has_value());
    QVERIFY(repository.savePlaybackHistory(second).has_value());

    const auto loaded = repository.loadPlaybackHistory(false, 0, 20);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->size(), size_t { 2 });
    QCOMPARE(loaded->at(0).id, QStringLiteral("second-service"));
    QCOMPARE(loaded->at(1).id, QStringLiteral("first-service"));
}

void PlaybackHistoryTest::supportsDateManagement()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SessionRepository repository(uniqueConnectionName(), directory.filePath(QStringLiteral("date-management.sqlite3")));
    QVERIFY(repository.initialize().has_value());

    const auto firstDay = historyItem(QStringLiteral("first-day"),
                                      PlaybackHistorySource::Local,
                                      QStringLiteral("C:/Media/first-day.mkv"),
                                      stampAt(1, 8, 0));
    const auto firstDayPrivate = historyItem(QStringLiteral("first-day-private"),
                                             PlaybackHistorySource::Link,
                                             QStringLiteral("https://example.com/private.m3u8"),
                                             stampAt(1, 9, 0),
                                             true);
    const auto secondDay = historyItem(QStringLiteral("second-day"),
                                       PlaybackHistorySource::Local,
                                       QStringLiteral("C:/Media/second-day.mkv"),
                                       stampAt(0, 8, 0));
    QVERIFY(repository.savePlaybackHistory(firstDay).has_value());
    QVERIFY(repository.savePlaybackHistory(firstDayPrivate).has_value());
    QVERIFY(repository.savePlaybackHistory(secondDay).has_value());

    const auto dates = repository.loadPlaybackHistoryDates(false);
    QVERIFY(dates.has_value());
    QCOMPARE(dates->size(), 2);
    QVERIFY(dates->contains(firstDay.playedDate.toString(Qt::ISODate)));
    QVERIFY(dates->contains(secondDay.playedDate.toString(Qt::ISODate)));

    const auto normalDay = repository.loadPlaybackHistoryForDate(false, firstDay.playedDate);
    QVERIFY(normalDay.has_value());
    QCOMPARE(normalDay->size(), size_t { 1 });
    QCOMPARE(normalDay->front().id, firstDay.id);

    QVERIFY(repository.deletePlaybackHistoryForDate(false, firstDay.playedDate).has_value());
    const auto afterNormalDelete = repository.loadPlaybackHistoryForDate(true, firstDay.playedDate);
    QVERIFY(afterNormalDelete.has_value());
    QCOMPARE(afterNormalDelete->size(), size_t { 1 });
    QCOMPARE(afterNormalDelete->front().id, firstDayPrivate.id);

    QVERIFY(repository.deletePlaybackHistoryForDate(true, firstDay.playedDate).has_value());
    const auto afterAllDelete = repository.loadPlaybackHistoryForDate(true, firstDay.playedDate);
    QVERIFY(afterAllDelete.has_value());
    QVERIFY(afterAllDelete->empty());
}

void PlaybackHistoryTest::configuresHistoryRetentionAndPrunesExpiredRecords()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SessionRepository repository(uniqueConnectionName(), directory.filePath(QStringLiteral("retention.sqlite3")));
    QVERIFY(repository.initialize().has_value());

    const auto previousRetention = repository.historyRetentionDays();
    repository.setHistoryRetentionDays(2);
    QCOMPARE(repository.historyRetentionDays(), 2);

    const auto now = QDateTime::currentDateTimeUtc();
    const auto expired = historyItem(QStringLiteral("expired"),
                                     PlaybackHistorySource::Local,
                                     QStringLiteral("C:/Media/expired.mkv"),
                                     now.addDays(-3).toString(Qt::ISODate));
    const auto retained = historyItem(QStringLiteral("retained"),
                                      PlaybackHistorySource::Local,
                                      QStringLiteral("C:/Media/retained.mkv"),
                                      now.addDays(-1).toString(Qt::ISODate));
    QVERIFY(repository.savePlaybackHistory(expired).has_value());
    QVERIFY(repository.savePlaybackHistory(retained).has_value());

    LinkPlaybackHistoryItem expiredLink {
        .id = QStringLiteral("expired-link"),
        .playbackUrl = QUrl(QStringLiteral("https://example.com/expired.m3u8")),
        .playedDate = expired.playedDate,
        .playedAt = expired.playedAt,
        .privacyMode = false,
    };
    QVERIFY(repository.saveLinkPlaybackHistory(expiredLink).has_value());

    QVERIFY(repository.pruneOldHistory().has_value());
    const auto loaded = repository.loadPlaybackHistory(true, 0, 20);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->size(), size_t { 1 });
    QCOMPARE(loaded->front().id, retained.id);
    const auto loadedLinks = repository.loadLinkPlaybackHistory(true);
    QVERIFY(loadedLinks.has_value());
    QVERIFY(loadedLinks->empty());

    repository.setHistoryRetentionDays(previousRetention);
}

void PlaybackHistoryTest::usesManagedPathForIptvServiceCards()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    SessionRepository repository(uniqueConnectionName(), directory.filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(repository.initialize().has_value());

    ServerConfig server {
        .id = QStringLiteral("iptv-service"),
        .name = QStringLiteral("Managed IPTV"),
        .baseUrl = QStringLiteral("D:/external/channels.m3u8"),
        .serviceType = ServiceType::IPTV,
    };
    const auto managedPath = directory.filePath(QStringLiteral("app-data/iptv/channels.m3u8"));
    IptvPlaylist playlist {
        .id = QStringLiteral("iptv-playlist-iptv-service"),
        .serviceId = server.id,
        .name = server.name,
        .sourceType = QStringLiteral("LocalFile"),
        .sourcePath = server.baseUrl,
        .importedPath = managedPath,
        .importedAt = QStringLiteral("2026-08-16T00:00:00Z"),
    };
    IptvChannel channel {
        .id = QStringLiteral("channel-one"),
        .playlistId = playlist.id,
        .name = QStringLiteral("Channel One"),
        .groupName = QStringLiteral("Default"),
        .logoUrl = QStringLiteral(""),
        .streamUrl = QStringLiteral("https://example.com/live.m3u8"),
    };

    const auto saveResult = repository.saveIptvPlaylist(server, playlist, { channel });
    if (!saveResult) {
        QFAIL(qPrintable(saveResult.error()));
    }
    const auto cards = repository.loadAllServiceCards();
    QVERIFY(cards.has_value());
    QCOMPARE(cards->size(), size_t { 1 });
    QCOMPARE(cards->front().server.baseUrl, managedPath);
}

void PlaybackHistoryTest::persistsAndClearsEmbyRecommendationCache()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    SessionRepository repository(uniqueConnectionName(), directory.filePath(QStringLiteral("recommendations.sqlite3")));
    QVERIFY(repository.initialize().has_value());
    ServerConfig server {
        .id = QStringLiteral("emby-service"),
        .name = QStringLiteral("Emby"),
        .baseUrl = QStringLiteral("https://emby.example"),
        .serviceType = ServiceType::Emby,
    };
    QVERIFY(repository.saveServer(server).has_value());

    const QByteArray payload = R"([{"id":"series-1"}])";
    const auto refreshedAt = QDateTime::fromString(QStringLiteral("2026-08-18T01:02:03.456Z"), Qt::ISODateWithMs);
    QVERIFY(repository.saveEmbyRecommendationCache(server.id,
                                                   QStringLiteral("user-1"),
                                                   payload,
                                                   refreshedAt)
                .has_value());

    const auto loaded = repository.loadEmbyRecommendationCache(server.id, QStringLiteral("user-1"));
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->has_value());
    QCOMPARE((*loaded)->payload, payload);
    QCOMPARE((*loaded)->refreshedAt.toUTC(), refreshedAt);

    QVERIFY(repository.clearEmbyRecommendationCaches().has_value());
    const auto cleared = repository.loadEmbyRecommendationCache(server.id, QStringLiteral("user-1"));
    QVERIFY(cleared.has_value());
    QVERIFY(!cleared->has_value());
}

void PlaybackHistoryTest::migratesExistingLinkHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("migration.sqlite3"));
    const auto olderPlayedAt = playedAt(2, 9, 30);
    const auto latestPlayedAt = playedAt(1, 9, 30);

    {
        const auto connectionName = uniqueConnectionName();
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            database.setDatabaseName(databasePath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral(
                "CREATE TABLE link_playback_history ("
                "id TEXT PRIMARY KEY, playback_url TEXT NOT NULL, played_date TEXT NOT NULL, played_at TEXT NOT NULL)")));
            query.prepare(QStringLiteral(
                "INSERT INTO link_playback_history (id, playback_url, played_date, played_at) "
                "VALUES (:id, :url, :date, :timestamp)"));
            const auto insertLink = [&](const QString& id, const QDateTime& playedAt) {
                query.bindValue(QStringLiteral(":id"), id);
                query.bindValue(QStringLiteral(":url"),
                                QStringLiteral("https://media.example.com/movie.mp4?token=abc%2F123"));
                query.bindValue(QStringLiteral(":date"), playedAt.toLocalTime().date().toString(Qt::ISODate));
                query.bindValue(QStringLiteral(":timestamp"), playedAt.toUTC().toString(Qt::ISODateWithMs));
                return query.exec();
            };
            QVERIFY(insertLink(QStringLiteral("legacy-link-older"), olderPlayedAt));
            QVERIFY(insertLink(QStringLiteral("legacy-link-latest"), latestPlayedAt));
            database.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    SessionRepository migratedRepository(uniqueConnectionName(), databasePath);
    QVERIFY(migratedRepository.initialize().has_value());
    const auto items = migratedRepository.loadPlaybackHistory(false, 0, 20, PlaybackHistorySource::Link);
    QVERIFY(items.has_value());
    QCOMPARE(items->size(), size_t { 1 });
    QCOMPARE(items->front().id, QStringLiteral("legacy-link-latest"));
    QCOMPARE(items->front().replayTarget,
             QStringLiteral("https://media.example.com/movie.mp4?token=abc%2F123"));

    QVERIFY(migratedRepository.deletePlaybackHistory(QStringLiteral("legacy-link-latest")).has_value());
    const auto linkItems = migratedRepository.loadLinkPlaybackHistory();
    QVERIFY(linkItems.has_value());
    QVERIFY(linkItems->empty());
}

void PlaybackHistoryTest::deduplicatesExistingGlobalHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("existing-duplicates.sqlite3"));
    const auto connectionName = uniqueConnectionName();

    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE playback_history ("
            "id TEXT PRIMARY KEY, source_type TEXT NOT NULL, service_id TEXT NOT NULL DEFAULT '', "
            "service_name TEXT NOT NULL DEFAULT '', replay_target TEXT NOT NULL, title TEXT NOT NULL, "
            "subtitle TEXT NOT NULL DEFAULT '', played_date TEXT NOT NULL, played_at TEXT NOT NULL, "
            "updated_at TEXT NOT NULL, position_seconds INTEGER NOT NULL DEFAULT 0, "
            "duration_seconds INTEGER NOT NULL DEFAULT 0, completed INTEGER NOT NULL DEFAULT 0, "
            "privacy_mode INTEGER NOT NULL DEFAULT 0)")));
        QVERIFY(query.exec(QStringLiteral(
            "INSERT INTO playback_history VALUES "
            "('duplicate-older', 'Emby', 'service-1', 'Server One', 'item-42', 'Older title', '', "
            "'%1', '%2', '%2', 120, 600, 0, 0), "
            "('duplicate-latest', 'Emby', 'service-1', 'Server One', 'item-42', 'Latest title', '', "
            "'%3', '%4', '%4', 240, 600, 0, 0)")
            .arg(dateAt(2), stampWithMsAt(2, 8, 0), dateAt(1), stampWithMsAt(1, 8, 0))));
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    SessionRepository repository(uniqueConnectionName(), databasePath);
    QVERIFY(repository.initialize().has_value());
    const auto items = repository.loadPlaybackHistory(true, 0, 20, PlaybackHistorySource::Emby);
    QVERIFY(items.has_value());
    QCOMPARE(items->size(), size_t { 1 });
    QCOMPARE(items->front().id, QStringLiteral("duplicate-latest"));
    QCOMPARE(items->front().title, QStringLiteral("Latest title"));
}

QTEST_GUILESS_MAIN(PlaybackHistoryTest)
#include "PlaybackHistoryTest.moc"
