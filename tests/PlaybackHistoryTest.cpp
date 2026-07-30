#include "database/SessionRepository.h"
#include "viewmodels/PlaybackHistoryListModel.h"

#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

class PlaybackHistoryTest final : public QObject {
    Q_OBJECT

private slots:
    void persistsFiltersUpdatesAndDeletesRecords();
    void migratesExistingLinkHistory();
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
                                   QStringLiteral("2026-07-28T08:00:00Z"));
    const auto newer = historyItem(QStringLiteral("newer"),
                                   PlaybackHistorySource::Emby,
                                   QStringLiteral("item-42"),
                                   QStringLiteral("2026-07-29T09:30:00Z"));
    const auto privateItem = historyItem(QStringLiteral("private"),
                                         PlaybackHistorySource::Link,
                                         QStringLiteral("https://example.com/private.m3u8"),
                                         QStringLiteral("2026-07-30T10:00:00Z"),
                                         true);
    QVERIFY(repository.savePlaybackHistory(older).has_value());
    QVERIFY(repository.savePlaybackHistory(newer).has_value());
    QVERIFY(repository.savePlaybackHistory(privateItem).has_value());

    const auto normalItems = repository.loadPlaybackHistory(false, 0, 20);
    QVERIFY(normalItems.has_value());
    QCOMPARE(normalItems->size(), size_t { 2 });
    QCOMPARE(normalItems->front().id, QStringLiteral("newer"));

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
    QCOMPARE(secondPage->front().id, QStringLiteral("newer"));

    QVERIFY(repository.updatePlaybackHistoryProgress(QStringLiteral("newer"),
                                                     315,
                                                     600,
                                                     true,
                                                     QDateTime::fromString(QStringLiteral("2026-07-29T09:36:00Z"), Qt::ISODate))
                .has_value());
    const auto updated = repository.loadPlaybackHistory(true, 0, 20, PlaybackHistorySource::Emby);
    QVERIFY(updated.has_value());
    QCOMPARE(updated->front().positionSeconds, qint64 { 315 });
    QCOMPARE(updated->front().durationSeconds, qint64 { 600 });
    QVERIFY(updated->front().completed);

    PlaybackHistoryListModel model;
    model.setItems(*allItems);
    QCOMPARE(model.count(), 3);
    QVERIFY(model.itemById(QStringLiteral("newer")) != nullptr);

    QVERIFY(repository.deletePlaybackHistory(QStringLiteral("newer")).has_value());
    const auto afterDelete = repository.loadPlaybackHistory(true, 0, 20);
    QVERIFY(afterDelete.has_value());
    QCOMPARE(afterDelete->size(), size_t { 2 });
}

void PlaybackHistoryTest::migratesExistingLinkHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto databasePath = directory.filePath(QStringLiteral("migration.sqlite3"));
    const auto playedAt = QDateTime::fromString(QStringLiteral("2026-07-29T09:30:00Z"), Qt::ISODate);

    {
        SessionRepository repository(uniqueConnectionName(), databasePath);
        QVERIFY(repository.initialize().has_value());
        QVERIFY(repository.saveLinkPlaybackHistory(LinkPlaybackHistoryItem {
            .id = QStringLiteral("legacy-link"),
            .playbackUrl = QUrl(QStringLiteral("https://media.example.com/movie.mp4?token=abc%2F123")),
            .playedDate = playedAt.toLocalTime().date(),
            .playedAt = playedAt,
        }).has_value());
    }

    SessionRepository migratedRepository(uniqueConnectionName(), databasePath);
    QVERIFY(migratedRepository.initialize().has_value());
    const auto items = migratedRepository.loadPlaybackHistory(false, 0, 20, PlaybackHistorySource::Link);
    QVERIFY(items.has_value());
    QCOMPARE(items->size(), size_t { 1 });
    QCOMPARE(items->front().id, QStringLiteral("legacy-link"));
    QCOMPARE(items->front().replayTarget,
             QStringLiteral("https://media.example.com/movie.mp4?token=abc%2F123"));

    QVERIFY(migratedRepository.deletePlaybackHistory(QStringLiteral("legacy-link")).has_value());
    const auto linkItems = migratedRepository.loadLinkPlaybackHistory();
    QVERIFY(linkItems.has_value());
    QVERIFY(linkItems->empty());
}

QTEST_GUILESS_MAIN(PlaybackHistoryTest)
#include "PlaybackHistoryTest.moc"
