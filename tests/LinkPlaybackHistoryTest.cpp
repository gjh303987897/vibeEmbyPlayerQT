#include "database/SessionRepository.h"
#include "viewmodels/LinkPlaybackHistoryListModel.h"

#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

class LinkPlaybackHistoryTest final : public QObject {
    Q_OBJECT

private slots:
    void persistsOrdersAndDeletesIndividualRecords();
    void storesLinkPlaybackTrafficInDailyUsage();
};

namespace {
QString uniqueConnectionName()
{
    return QStringLiteral("link-history-test-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

LinkPlaybackHistoryItem historyItem(const QString& id,
                                    const QString& url,
                                    const QString& date,
                                    const QString& timestamp)
{
    return LinkPlaybackHistoryItem {
        .id = id,
        .playbackUrl = QUrl(url, QUrl::StrictMode),
        .playedDate = QDate::fromString(date, Qt::ISODate),
        .playedAt = QDateTime::fromString(timestamp, Qt::ISODate),
    };
}
}

void LinkPlaybackHistoryTest::persistsOrdersAndDeletesIndividualRecords()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SessionRepository repository(uniqueConnectionName(), directory.filePath(QStringLiteral("history.sqlite3")));
    QVERIFY(repository.initialize().has_value());

    const auto older = historyItem(QStringLiteral("older"),
                                   QStringLiteral("https://media.example.com/older.mp4"),
                                   QStringLiteral("2026-07-28"),
                                   QStringLiteral("2026-07-28T08:00:00Z"));
    const auto newer = historyItem(QStringLiteral("newer"),
                                   QStringLiteral("https://media.example.com/live/index.m3u8?token=abc%2F123"),
                                   QStringLiteral("2026-07-29"),
                                   QStringLiteral("2026-07-29T09:30:00Z"));
    QVERIFY(repository.saveLinkPlaybackHistory(older).has_value());
    QVERIFY(repository.saveLinkPlaybackHistory(newer).has_value());

    const auto loaded = repository.loadLinkPlaybackHistory();
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->size(), size_t { 2 });
    QCOMPARE(loaded->at(0).id, QStringLiteral("newer"));
    QCOMPARE(loaded->at(0).playedDate, QDate(2026, 7, 29));
    QCOMPARE(loaded->at(0).playbackUrl.query(QUrl::FullyEncoded), QStringLiteral("token=abc%2F123"));
    QCOMPARE(loaded->at(1).id, QStringLiteral("older"));

    LinkPlaybackHistoryListModel model;
    model.setItems(*loaded);
    QCOMPARE(model.count(), 2);
    QVERIFY(model.itemById(QStringLiteral("newer")) != nullptr);

    QVERIFY(repository.deleteLinkPlaybackHistory(QStringLiteral("newer")).has_value());
    const auto afterDelete = repository.loadLinkPlaybackHistory();
    QVERIFY(afterDelete.has_value());
    QCOMPARE(afterDelete->size(), size_t { 1 });
    QCOMPARE(afterDelete->front().id, QStringLiteral("older"));
}

void LinkPlaybackHistoryTest::storesLinkPlaybackTrafficInDailyUsage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SessionRepository repository(uniqueConnectionName(), directory.filePath(QStringLiteral("usage.sqlite3")));
    QVERIFY(repository.initialize().has_value());

    const ServerConfig linkSource {
        .id = QStringLiteral("builtin-link-playback"),
        .name = QStringLiteral("Link Playback"),
        .serviceType = ServiceType::Link,
    };
    QVERIFY(repository.addDailyUsage(linkSource, false, 0, 8192, 0, 0, 0).has_value());

    const auto stats = repository.loadDailyUsageStats(false);
    QVERIFY(stats.has_value());
    QCOMPARE(stats->size(), size_t { 1 });
    QCOMPARE(stats->front().serviceId, QStringLiteral("builtin-link-playback"));
    QCOMPARE(stats->front().serviceType, QStringLiteral("Link"));
    QCOMPARE(stats->front().networkBytesIn, qint64 { 8192 });
}

QTEST_GUILESS_MAIN(LinkPlaybackHistoryTest)
#include "LinkPlaybackHistoryTest.moc"
