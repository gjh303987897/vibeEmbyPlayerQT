#include "services/emby/EmbyRecommendationCache.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace {
QString sanitizedUrl(const QString& value)
{
    if (value.isEmpty()) {
        return {};
    }

    QUrl url(value);
    if (!url.isValid()) {
        return {};
    }

    QUrlQuery originalQuery(url);
    QUrlQuery sanitizedQuery;
    for (const auto& [key, queryValue] : originalQuery.queryItems(QUrl::FullyDecoded)) {
        if (key.compare(QStringLiteral("api_key"), Qt::CaseInsensitive) == 0
            || key.compare(QStringLiteral("X-Emby-Token"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        sanitizedQuery.addQueryItem(key, queryValue);
    }
    url.setQuery(sanitizedQuery);
    return url.toString(QUrl::FullyEncoded);
}

QString authenticatedUrl(const QString& value, const QString& accessToken)
{
    const auto sanitized = sanitizedUrl(value);
    if (sanitized.isEmpty() || accessToken.isEmpty()) {
        return sanitized;
    }

    QUrl url(sanitized);
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("api_key"), accessToken);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

QJsonArray sanitizedUrls(const QStringList& urls)
{
    QJsonArray result;
    for (const auto& url : urls) {
        const auto sanitized = sanitizedUrl(url);
        if (!sanitized.isEmpty()) {
            result.append(sanitized);
        }
    }
    return result;
}

QJsonObject itemToJson(const MediaItem& item)
{
    return {
        { QStringLiteral("id"), item.id },
        { QStringLiteral("parentId"), item.parentId },
        { QStringLiteral("name"), item.name },
        { QStringLiteral("itemType"), item.itemType },
        { QStringLiteral("productionYear"), item.productionYear },
        { QStringLiteral("seriesId"), item.seriesId },
        { QStringLiteral("seriesName"), item.seriesName },
        { QStringLiteral("seriesImageTag"), item.seriesImageTag },
        { QStringLiteral("seriesImageUrl"), sanitizedUrl(item.seriesImageUrl) },
        { QStringLiteral("childCount"), item.childCount },
        { QStringLiteral("overview"), item.overview },
        { QStringLiteral("imageTag"), item.imageTag },
        { QStringLiteral("imageUrl"), sanitizedUrl(item.imageUrl) },
        { QStringLiteral("logoImageUrl"), sanitizedUrl(item.logoImageUrl) },
        { QStringLiteral("backdropImageUrl"), sanitizedUrl(item.backdropImageUrl) },
        { QStringLiteral("backdropImageUrls"), sanitizedUrls(item.backdropImageUrls) },
        { QStringLiteral("communityRating"), item.communityRating },
        { QStringLiteral("officialRating"), item.officialRating },
        { QStringLiteral("runTime"), item.runTime },
        { QStringLiteral("runTimeTicks"), item.runTimeTicks },
        { QStringLiteral("genres"), item.genres },
    };
}

QStringList authenticatedUrls(const QJsonArray& values, const QString& accessToken)
{
    QStringList result;
    result.reserve(values.size());
    for (const auto& value : values) {
        const auto url = authenticatedUrl(value.toString(), accessToken);
        if (!url.isEmpty()) {
            result.append(url);
        }
    }
    return result;
}

MediaItem itemFromJson(const QJsonObject& object, const QString& accessToken)
{
    MediaItem item;
    item.id = object.value(QStringLiteral("id")).toString();
    item.parentId = object.value(QStringLiteral("parentId")).toString();
    item.name = object.value(QStringLiteral("name")).toString();
    item.itemType = object.value(QStringLiteral("itemType")).toString();
    item.productionYear = object.value(QStringLiteral("productionYear")).toString();
    item.seriesId = object.value(QStringLiteral("seriesId")).toString();
    item.seriesName = object.value(QStringLiteral("seriesName")).toString();
    item.seriesImageTag = object.value(QStringLiteral("seriesImageTag")).toString();
    item.seriesImageUrl = authenticatedUrl(object.value(QStringLiteral("seriesImageUrl")).toString(), accessToken);
    item.childCount = object.value(QStringLiteral("childCount")).toInt();
    item.overview = object.value(QStringLiteral("overview")).toString();
    item.imageTag = object.value(QStringLiteral("imageTag")).toString();
    item.imageUrl = authenticatedUrl(object.value(QStringLiteral("imageUrl")).toString(), accessToken);
    item.logoImageUrl = authenticatedUrl(object.value(QStringLiteral("logoImageUrl")).toString(), accessToken);
    item.backdropImageUrl = authenticatedUrl(object.value(QStringLiteral("backdropImageUrl")).toString(), accessToken);
    item.backdropImageUrls = authenticatedUrls(object.value(QStringLiteral("backdropImageUrls")).toArray(), accessToken);
    item.communityRating = object.value(QStringLiteral("communityRating")).toString();
    item.officialRating = object.value(QStringLiteral("officialRating")).toString();
    item.runTime = object.value(QStringLiteral("runTime")).toString();
    item.runTimeTicks = object.value(QStringLiteral("runTimeTicks")).toInteger();
    item.genres = object.value(QStringLiteral("genres")).toString();
    return item;
}
}

QByteArray EmbyRecommendationCache::serialize(std::span<const MediaItem> items)
{
    QJsonArray serializedItems;
    for (const auto& item : items) {
        serializedItems.append(itemToJson(item));
    }
    return QJsonDocument(serializedItems).toJson(QJsonDocument::Compact);
}

std::expected<std::vector<MediaItem>, QString> EmbyRecommendationCache::deserialize(
    const QByteArray& payload,
    const QString& accessToken)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return std::unexpected(QStringLiteral("Invalid Emby recommendation cache: %1").arg(error.errorString()));
    }

    std::vector<MediaItem> items;
    items.reserve(static_cast<size_t>(document.array().size()));
    for (const auto& value : document.array()) {
        if (!value.isObject()) {
            continue;
        }
        auto item = itemFromJson(value.toObject(), accessToken);
        if (!item.id.isEmpty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

std::vector<MediaItem> EmbyRecommendationCache::filtered(std::span<const MediaItem> items,
                                                          const QStringList& excludedGenres,
                                                          qsizetype limit)
{
    QSet<QString> excluded;
    for (const auto& genre : excludedGenres) {
        excluded.insert(genre.trimmed().toCaseFolded());
    }

    std::vector<MediaItem> result;
    if (limit > 0) {
        result.reserve(static_cast<size_t>(std::min<qsizetype>(limit, static_cast<qsizetype>(items.size()))));
    }
    for (const auto& item : items) {
        const auto genres = item.genres.split(QLatin1Char(','), Qt::SkipEmptyParts);
        const auto isExcluded = std::ranges::any_of(genres, [&excluded](const QString& genre) {
            return excluded.contains(genre.trimmed().toCaseFolded());
        });
        if (isExcluded) {
            continue;
        }
        result.push_back(item);
        if (limit > 0 && static_cast<qsizetype>(result.size()) >= limit) {
            break;
        }
    }
    return result;
}

bool EmbyRecommendationCache::refreshedToday(const QDateTime& refreshedAt, const QDate& localDate)
{
    return refreshedAt.isValid() && refreshedAt.toLocalTime().date() == localDate;
}
