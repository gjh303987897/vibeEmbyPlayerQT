#include "services/emby/EmbyRecommendationCache.h"

#include <QDateTime>
#include <QtTest>

#include <span>
#include <vector>

namespace {
MediaItem recommendationItem(const QString& id,
                             const QString& name,
                             const QString& genres)
{
    MediaItem item;
    item.id = id;
    item.name = name;
    item.itemType = QStringLiteral("Series");
    item.genres = genres;
    return item;
}
}

class EmbyRecommendationCacheTest final : public QObject {
    Q_OBJECT

private slots:
    void serializesWithoutPersistingAccessTokens();
    void rejectsInvalidPayloads();
    void filtersExcludedGenresAndHonorsLimit();
    void detectsRefreshByLocalCalendarDate();
};

void EmbyRecommendationCacheTest::serializesWithoutPersistingAccessTokens()
{
    auto item = recommendationItem(QStringLiteral("series-1"),
                                   QStringLiteral("Example Show"),
                                   QStringLiteral("Drama, Sci-Fi"));
    item.imageUrl = QStringLiteral("https://emby.example/Items/series-1/Images/Primary?api_key=old-token&width=800");
    item.backdropImageUrls = {
        QStringLiteral("https://emby.example/Items/series-1/Images/Backdrop?X-Emby-Token=old-token"),
    };

    const std::vector<MediaItem> items { item };
    const auto payload = EmbyRecommendationCache::serialize(
        std::span<const MediaItem>(items.data(), items.size()));
    QVERIFY(!payload.contains("old-token"));

    const auto decoded = EmbyRecommendationCache::deserialize(payload, QStringLiteral("new-token"));
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->size(), size_t { 1 });
    QCOMPARE(decoded->front().id, QStringLiteral("series-1"));
    QCOMPARE(decoded->front().genres, QStringLiteral("Drama, Sci-Fi"));
    QVERIFY(decoded->front().imageUrl.contains(QStringLiteral("api_key=new-token")));
    QVERIFY(decoded->front().imageUrl.contains(QStringLiteral("width=800")));
    QVERIFY(decoded->front().backdropImageUrls.front().contains(QStringLiteral("api_key=new-token")));
}

void EmbyRecommendationCacheTest::rejectsInvalidPayloads()
{
    const auto decoded = EmbyRecommendationCache::deserialize(
        QByteArrayLiteral(R"({"items":[]})"), QStringLiteral("token"));
    QVERIFY(!decoded.has_value());
    QVERIFY(!decoded.error().isEmpty());
}

void EmbyRecommendationCacheTest::filtersExcludedGenresAndHonorsLimit()
{
    const std::vector<MediaItem> items {
        recommendationItem(QStringLiteral("one"), QStringLiteral("One"), QStringLiteral("Drama, Sci-Fi")),
        recommendationItem(QStringLiteral("two"), QStringLiteral("Two"), QStringLiteral("Comedy")),
        recommendationItem(QStringLiteral("three"), QStringLiteral("Three"), QStringLiteral("Documentary")),
    };

    const auto filtered = EmbyRecommendationCache::filtered(
        std::span<const MediaItem>(items.data(), items.size()),
        QStringList { QStringLiteral(" drama ") },
        1);
    QCOMPARE(filtered.size(), size_t { 1 });
    QCOMPARE(filtered.front().id, QStringLiteral("two"));
}

void EmbyRecommendationCacheTest::detectsRefreshByLocalCalendarDate()
{
    const auto now = QDateTime::currentDateTime();
    QVERIFY(EmbyRecommendationCache::refreshedToday(now, now.toLocalTime().date()));
    QVERIFY(!EmbyRecommendationCache::refreshedToday(now.addDays(-1), now.toLocalTime().date()));
    QVERIFY(!EmbyRecommendationCache::refreshedToday({}, now.toLocalTime().date()));
}

QTEST_MAIN(EmbyRecommendationCacheTest)
#include "EmbyRecommendationCacheTest.moc"
