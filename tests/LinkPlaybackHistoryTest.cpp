#include "database/SessionRepository.h"
#include "viewmodels/LinkPlaybackHistoryListModel.h"
#include "TimeFixtures.h"

#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace TimeFixtures;

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
                                    const QString& timestamp,
                                    bool privacyMode = false)
{
    return LinkPlaybackHistoryItem {
        .id = id,
        .playbackUrl = QUrl(url, QUrl::StrictMode),
        .playedDate = QDate::fromString(date, Qt::ISODate),
        .playedAt = QDateTime::fromString(timestamp, Qt::ISODate),
        .privacyMode = privacyMode,
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
                                   dateAt(2),
                                   stampAt(2, 8, 0));
    const auto newer = historyItem(QStringLiteral("newer"),
                                   QStringLiteral("https://media.example.com/live/index.m3u8?token=abc%2F123"),
                                   dateAt(1),
                                   stampAt(1, 9, 30));
    const auto privateItem = historyItem(QStringLiteral("private"),
                                         QStringLiteral("https://media.example.com/private.m3u8"),
                                         dateAt(0),
                                         stampAt(0, 10, 0),
                                         true);
    const auto latestSameLink = historyItem(QStringLiteral("latest-same-link"),
                                            QStringLiteral("https://media.example.com/live/index.m3u8?token=abc%2F123"),
                                            dateAt(1),
                                            stampAt(1, 11, 0));
    QVERIFY(repository.saveLinkPlaybackHistory(older).has_value());
    QVERIFY(repository.saveLinkPlaybackHistory(newer).has_value());
    QVERIFY(repository.saveLinkPlaybackHistory(latestSameLink).has_value());
    QVERIFY(repository.saveLinkPlaybackHistory(privateItem).has_value());

    const auto loaded = repository.loadLinkPlaybackHistory();
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->size(), size_t { 2 });
    QCOMPARE(loaded->at(0).id, QStringLiteral("latest-same-link"));
    QCOMPARE(loaded->at(0).playedDate, dayAt(1));
    QCOMPARE(loaded->at(0).playbackUrl.query(QUrl::FullyEncoded), QStringLiteral("token=abc%2F123"));
    QCOMPARE(loaded->at(1).id, QStringLiteral("older"));

    const auto withPrivate = repository.loadLinkPlaybackHistory(true);
    QVERIFY(withPrivate.has_value());
    QCOMPARE(withPrivate->size(), size_t { 3 });
    QCOMPARE(withPrivate->front().id, QStringLiteral("private"));
    QVERIFY(withPrivate->front().privacyMode);

    LinkPlaybackHistoryListModel model;
    model.setItems(*loaded);
    QCOMPARE(model.count(), 2);
    QVERIFY(model.itemById(QStringLiteral("latest-same-link")) != nullptr);

    QVERIFY(repository.deleteLinkPlaybackHistory(QStringLiteral("latest-same-link")).has_value());
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
