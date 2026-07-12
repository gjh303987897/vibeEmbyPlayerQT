#include "services/media/MediaServerClientBase.h"

#include "models/MediaPerson.h"
#include "utils/JsonUtils.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QUrlQuery>

#include <algorithm>
#include <functional>
#include <utility>

namespace {
QString collectionTypeLabel(const QString& collectionType, const QString& itemType)
{
    if (!collectionType.isEmpty()) {
        return collectionType;
    }
    return itemType;
}

QString stringListFromArray(const QJsonArray& array)
{
    QStringList values;
    for (const auto& value : array) {
        const auto text = value.toString().trimmed();
        if (!text.isEmpty()) {
            values.push_back(text);
        }
    }
    return values.join(QStringLiteral(", "));
}

QString primaryImageTag(const QJsonObject& object)
{
    const auto directTag = jsonStringAny(object, {
        QStringLiteral("PrimaryImageTag"),
        QStringLiteral("primaryImageTag"),
    });
    if (!directTag.isEmpty()) {
        return directTag;
    }

    const auto imageTags = object.value(QStringLiteral("ImageTags"))
                               .toObject(object.value(QStringLiteral("imageTags")).toObject());
    return jsonStringAny(imageTags, { QStringLiteral("Primary"), QStringLiteral("primary") });
}

QString imageUrlForItem(const QString& baseUrl, const QString& itemId, const QString& imageTag, const QString& token, int width)
{
    if (baseUrl.isEmpty() || itemId.isEmpty() || imageTag.isEmpty()) {
        return {};
    }

    auto normalizedBaseUrl = baseUrl.trimmed();
    while (normalizedBaseUrl.endsWith(QLatin1Char('/'))) {
        normalizedBaseUrl.chop(1);
    }
    auto url = QUrl(normalizedBaseUrl + QStringLiteral("/Items/%1/Images/Primary").arg(itemId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("maxWidth"), QString::number(width));
    query.addQueryItem(QStringLiteral("quality"), QStringLiteral("90"));
    if (!imageTag.isEmpty()) {
        query.addQueryItem(QStringLiteral("tag"), imageTag);
    }
    if (!token.isEmpty()) {
        query.addQueryItem(QStringLiteral("api_key"), token);
    }
    url.setQuery(query);
    return url.toString();
}

std::vector<MediaPerson> peopleListFromArray(const QJsonArray& array, const QString& baseUrl, const QString& token)
{
    std::vector<MediaPerson> people;
    people.reserve(static_cast<size_t>(array.size()));
    for (const auto& value : array) {
        if (!value.isObject()) {
            continue;
        }
        const auto object = value.toObject();
        MediaPerson person;
        person.id = jsonStringAny(object, { QStringLiteral("Id"), QStringLiteral("id") });
        person.name = jsonStringAny(object, { QStringLiteral("Name"), QStringLiteral("name") });
        person.role = jsonStringAny(object, { QStringLiteral("Role"), QStringLiteral("role") });
        person.type = jsonStringAny(object, { QStringLiteral("Type"), QStringLiteral("type") });
        person.imageTag = primaryImageTag(object);
        person.imageUrl = imageUrlForItem(baseUrl, person.id, person.imageTag, token, 320);
        if (person.name.isEmpty()) {
            continue;
        }
        people.push_back(std::move(person));
    }
    return people;
}

QString peopleTextFromList(const std::vector<MediaPerson>& people)
{
    QStringList values;
    for (const auto& person : people) {
        values.push_back(person.role.isEmpty() ? person.name : QStringLiteral("%1 as %2").arg(person.name, person.role));
    }
    return values.join(QStringLiteral(", "));
}

QString runtimeFromTicks(qint64 ticks)
{
    if (ticks <= 0) {
        return {};
    }
    const auto minutes = ticks / 10'000'000 / 60;
    if (minutes <= 0) {
        return {};
    }
    return QStringLiteral("%1 min").arg(minutes);
}

QString firstBackdropTag(const QJsonObject& object)
{
    const auto tags = object.value(QStringLiteral("BackdropImageTags")).toArray(object.value(QStringLiteral("backdropImageTags")).toArray());
    if (tags.isEmpty()) {
        return {};
    }
    return tags.first().toString();
}

NetworkError parseError(const QString& message)
{
    return {
        .kind = NetworkErrorKind::Parse,
        .message = message,
    };
}

std::expected<QJsonObject, NetworkError> parseObject(const QByteArray& body)
{
    QJsonParseError parseErrorInfo;
    const auto document = QJsonDocument::fromJson(body, &parseErrorInfo);
    if (parseErrorInfo.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(parseError(QStringLiteral("Invalid JSON response")));
    }
    return document.object();
}

std::expected<std::pair<QString, QString>, NetworkError> parsePlaybackInfo(const QByteArray& body)
{
    const auto root = parseObject(body);
    if (!root) {
        return std::unexpected(root.error());
    }

    const auto playSessionId = jsonStringAny(*root, { QStringLiteral("PlaySessionId"), QStringLiteral("playSessionId") });
    const auto sources = root->value(QStringLiteral("MediaSources")).toArray(root->value(QStringLiteral("mediaSources")).toArray());
    if (sources.isEmpty() || !sources.first().isObject()) {
        return std::unexpected(parseError(QStringLiteral("Playback info did not include a media source")));
    }

    const auto source = sources.first().toObject();
    const auto mediaSourceId = jsonStringAny(source, { QStringLiteral("Id"), QStringLiteral("id") });
    if (mediaSourceId.isEmpty() || playSessionId.isEmpty()) {
        return std::unexpected(parseError(QStringLiteral("Playback info is missing MediaSourceId or PlaySessionId")));
    }

    return std::pair { mediaSourceId, playSessionId };
}
}

MediaServerClientBase::MediaServerClientBase(NetworkClient& networkClient, QObject* parent)
    : MediaServiceClient(parent)
    , m_networkClient(networkClient)
{
    connect(&m_networkClient,
            &NetworkClient::certificateConfirmationRequired,
            this,
            &MediaServiceClient::certificateConfirmationRequired);
}

QUrl MediaServerClientBase::makeUrl(const QString& baseUrl, const QString& path)
{
    const auto normalizedBaseUrl = normalizeBaseUrl(baseUrl);
    QUrl url(normalizedBaseUrl + path);
    return url;
}

QString MediaServerClientBase::normalizeBaseUrl(QString baseUrl)
{
    baseUrl = baseUrl.trimmed();
    while (baseUrl.endsWith(QLatin1Char('/'))) {
        baseUrl.chop(1);
    }
    return baseUrl;
}

MediaLibrary MediaServerClientBase::parseLibrary(const QJsonObject& object, const QString& baseUrl, const QString& token)
{
    MediaLibrary library;
    library.id = jsonStringAny(object, { QStringLiteral("Id"), QStringLiteral("id") });
    library.name = jsonStringAny(object, { QStringLiteral("Name"), QStringLiteral("name") });
    library.collectionType = jsonStringAny(object, { QStringLiteral("CollectionType"), QStringLiteral("collectionType") });
    library.itemType = collectionTypeLabel(library.collectionType, jsonStringAny(object, { QStringLiteral("Type"), QStringLiteral("type") }));
    library.imageTag = primaryImageTag(object);
    library.childCount = jsonIntAny(object, { QStringLiteral("ChildCount"), QStringLiteral("childCount") });
    library.imageUrl = primaryImageUrl(baseUrl, library.id, library.imageTag, token, 420);
    return library;
}

MediaItem MediaServerClientBase::parseItem(const QJsonObject& object, const QString& baseUrl, const QString& token)
{
    MediaItem item;
    item.id = jsonStringAny(object, { QStringLiteral("Id"), QStringLiteral("id") });
    item.parentId = jsonStringAny(object, { QStringLiteral("ParentId"), QStringLiteral("parentId") });
    item.name = jsonStringAny(object, { QStringLiteral("Name"), QStringLiteral("name") });
    item.itemType = jsonStringAny(object, { QStringLiteral("Type"), QStringLiteral("type") });
    item.folder = jsonBoolAny(object, { QStringLiteral("IsFolder"), QStringLiteral("isFolder") });
    item.productionYear = jsonStringAny(object, { QStringLiteral("ProductionYear"), QStringLiteral("productionYear") });
    item.seriesId = jsonStringAny(object, { QStringLiteral("SeriesId"), QStringLiteral("seriesId") });
    if (item.seriesId.isEmpty() && item.itemType.compare(QStringLiteral("Series"), Qt::CaseInsensitive) == 0) {
        item.seriesId = item.id;
    }
    item.seriesName = jsonStringAny(object, { QStringLiteral("SeriesName"), QStringLiteral("seriesName") });
    item.seriesImageTag = jsonStringAny(object, { QStringLiteral("SeriesPrimaryImageTag"), QStringLiteral("seriesPrimaryImageTag") });
    item.childCount = jsonIntAny(object, { QStringLiteral("ChildCount"), QStringLiteral("childCount") });
    item.overview = jsonStringAny(object, { QStringLiteral("Overview"), QStringLiteral("overview") });
    item.imageTag = primaryImageTag(object);
    item.imageUrl = primaryImageUrl(baseUrl, item.id, item.imageTag, token, 460);
    item.seriesImageUrl = primaryImageUrl(baseUrl, item.seriesId, item.seriesImageTag, token, 460);
    item.backdropImageUrl = backdropImageUrl(baseUrl, item.id, firstBackdropTag(object), token, 1280);
    const auto rating = jsonDoubleAny(object, { QStringLiteral("CommunityRating"), QStringLiteral("communityRating") });
    if (rating > 0) {
        item.communityRating = QString::number(rating, 'f', 1);
    }
    item.officialRating = jsonStringAny(object, { QStringLiteral("OfficialRating"), QStringLiteral("officialRating") });
    item.runTimeTicks = jsonInt64Any(object, { QStringLiteral("RunTimeTicks"), QStringLiteral("runTimeTicks") });
    item.runTime = runtimeFromTicks(item.runTimeTicks);
    item.genres = stringListFromArray(object.value(QStringLiteral("Genres")).toArray(object.value(QStringLiteral("genres")).toArray()));
    item.peopleList = peopleListFromArray(object.value(QStringLiteral("People")).toArray(object.value(QStringLiteral("people")).toArray()), baseUrl, token);
    item.people = peopleTextFromList(item.peopleList);
    item.seasonName = jsonStringAny(object, { QStringLiteral("SeasonName"), QStringLiteral("seasonName") });
    const auto indexNumber = jsonIntAny(object, { QStringLiteral("IndexNumber"), QStringLiteral("indexNumber") }, -1);
    const auto parentIndexNumber = jsonIntAny(object, { QStringLiteral("ParentIndexNumber"), QStringLiteral("parentIndexNumber") }, -1);
    item.indexNumber = indexNumber >= 0 ? QString::number(indexNumber) : QString {};
    item.parentIndexNumber = parentIndexNumber >= 0 ? QString::number(parentIndexNumber) : QString {};

    const auto userData = object.value(QStringLiteral("UserData")).toObject();
    const auto lowerUserData = object.value(QStringLiteral("userData")).toObject();
    const auto effectiveUserData = userData.isEmpty() ? lowerUserData : userData;
    item.played = jsonBoolAny(effectiveUserData, { QStringLiteral("Played"), QStringLiteral("played") });
    item.playbackPositionTicks = jsonInt64Any(effectiveUserData,
                                              { QStringLiteral("PlaybackPositionTicks"), QStringLiteral("playbackPositionTicks") });
    item.playedPercentage = jsonDoubleAny(effectiveUserData,
                                          { QStringLiteral("PlayedPercentage"), QStringLiteral("playedPercentage") });
    return item;
}

std::vector<MediaLibrary> MediaServerClientBase::parseLibraries(const QByteArray& body, const QString& baseUrl, const QString& token)
{
    const auto document = QJsonDocument::fromJson(body);
    const auto root = document.object();
    const auto items = root.value(QStringLiteral("Items")).toArray(root.value(QStringLiteral("items")).toArray());

    std::vector<MediaLibrary> libraries;
    libraries.reserve(static_cast<size_t>(items.size()));
    for (const auto& value : items) {
        if (value.isObject()) {
            libraries.push_back(parseLibrary(value.toObject(), baseUrl, token));
        }
    }
    return libraries;
}

std::vector<MediaItem> MediaServerClientBase::parseItems(const QByteArray& body, const QString& baseUrl, const QString& token)
{
    const auto document = QJsonDocument::fromJson(body);
    const auto root = document.object();
    const auto items = root.value(QStringLiteral("Items")).toArray(root.value(QStringLiteral("items")).toArray());

    std::vector<MediaItem> mediaItems;
    mediaItems.reserve(static_cast<size_t>(items.size()));
    for (const auto& value : items) {
        if (value.isObject()) {
            mediaItems.push_back(parseItem(value.toObject(), baseUrl, token));
        }
    }
    return mediaItems;
}

std::expected<MediaItem, NetworkError> MediaServerClientBase::parseItemDetails(const QByteArray& body, const QString& baseUrl, const QString& token)
{
    QJsonParseError parseErrorInfo;
    const auto document = QJsonDocument::fromJson(body, &parseErrorInfo);
    if (parseErrorInfo.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(parseError(QStringLiteral("Invalid JSON response")));
    }
    return parseItem(document.object(), baseUrl, token);
}

QString MediaServerClientBase::primaryImageUrl(const QString& baseUrl,
                                               const QString& itemId,
                                               const QString& imageTag,
                                               const QString& token,
                                               int width)
{
    if (baseUrl.isEmpty() || itemId.isEmpty() || imageTag.isEmpty()) {
        return {};
    }

    auto url = makeUrl(baseUrl, QStringLiteral("/Items/%1/Images/Primary").arg(itemId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("maxWidth"), QString::number(width));
    query.addQueryItem(QStringLiteral("quality"), QStringLiteral("90"));
    if (!imageTag.isEmpty()) {
        query.addQueryItem(QStringLiteral("tag"), imageTag);
    }
    if (!token.isEmpty()) {
        query.addQueryItem(QStringLiteral("api_key"), token);
    }
    url.setQuery(query);
    return url.toString();
}

QString MediaServerClientBase::backdropImageUrl(const QString& baseUrl,
                                                const QString& itemId,
                                                const QString& imageTag,
                                                const QString& token,
                                                int width)
{
    if (baseUrl.isEmpty() || itemId.isEmpty() || imageTag.isEmpty()) {
        return {};
    }

    auto url = makeUrl(baseUrl, QStringLiteral("/Items/%1/Images/Backdrop").arg(itemId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("maxWidth"), QString::number(width));
    query.addQueryItem(QStringLiteral("quality"), QStringLiteral("90"));
    query.addQueryItem(QStringLiteral("tag"), imageTag);
    if (!token.isEmpty()) {
        query.addQueryItem(QStringLiteral("api_key"), token);
    }
    url.setQuery(query);
    return url.toString();
}

PlaybackUrlResult MediaServerClientBase::streamUrl(const UserSession& session,
                                                   const MediaItem& item,
                                                   const QString& mediaSourceId,
                                                   const QString& playSessionId)
{
    if (session.server.baseUrl.isEmpty() ||
        item.id.isEmpty() ||
        session.accessToken.isEmpty() ||
        mediaSourceId.isEmpty() ||
        playSessionId.isEmpty()) {
        return std::unexpected(NetworkError {
            .kind = NetworkErrorKind::InvalidUrl,
            .message = QStringLiteral("Playback URL cannot be created from the current session"),
        });
    }

    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Videos/%1/stream").arg(item.id));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("static"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("MediaSourceId"), mediaSourceId);
    query.addQueryItem(QStringLiteral("PlaySessionId"), playSessionId);
    query.addQueryItem(QStringLiteral("api_key"), session.accessToken);
    url.setQuery(query);
    return PlaybackRequest {
        .url = url,
        .startSeconds = item.playbackPositionTicks > 0 ? static_cast<double>(item.playbackPositionTicks) / 10'000'000.0 : 0.0,
        .mediaSourceId = mediaSourceId,
        .playSessionId = playSessionId,
    };
}

void MediaServerClientBase::fetchPlaybackUrlWithScheme(const QString& authScheme,
                                                       const UserSession& session,
                                                       const MediaItem& item,
                                                       PlaybackUrlCallback callback)
{
    if (session.server.baseUrl.isEmpty() || item.id.isEmpty() || session.accessToken.isEmpty()) {
        callback(std::unexpected(NetworkError {
            .kind = NetworkErrorKind::InvalidUrl,
            .message = QStringLiteral("Playback URL cannot be created from the current session"),
        }));
        return;
    }

    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Items/%1/PlaybackInfo").arg(item.id));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("UserId"), session.userId);
    query.addQueryItem(QStringLiteral("IsPlayback"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("AutoOpenLiveStream"), QStringLiteral("true"));
    url.setQuery(query);

    const auto headers = authHeaders(authScheme, session.accessToken);
    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, item, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        const auto playbackInfo = parsePlaybackInfo(result->body);
        if (!playbackInfo) {
            callback(std::unexpected(playbackInfo.error()));
            return;
        }

        callback(streamUrl(session, item, playbackInfo->first, playbackInfo->second));
    });
}

void MediaServerClientBase::postPlaybackReport(const QString& authScheme,
                                               const UserSession& session,
                                               const QString& path,
                                               const PlaybackReport& report)
{
    if (session.server.baseUrl.isEmpty() || session.accessToken.isEmpty() || report.itemId.isEmpty()) {
        return;
    }

    const QJsonObject body {
        { QStringLiteral("ItemId"), report.itemId },
        { QStringLiteral("MediaSourceId"), report.mediaSourceId },
        { QStringLiteral("PlaySessionId"), report.playSessionId },
        { QStringLiteral("CanSeek"), true },
        { QStringLiteral("PlayMethod"), QStringLiteral("DirectPlay") },
        { QStringLiteral("PositionTicks"), static_cast<double>(std::max<qint64>(0, report.positionTicks)) },
        { QStringLiteral("IsPaused"), report.paused },
    };

    const auto url = makeUrl(session.server.baseUrl, path);
    const auto headers = authHeaders(authScheme, session.accessToken);
    m_networkClient.postJson(url, headers, body, session.server.trustSelfSignedCertificate, [](NetworkResult) {});
}

QHash<QByteArray, QByteArray> MediaServerClientBase::authHeaders(const QString& scheme, const QString& token)
{
    QHash<QByteArray, QByteArray> headers;
    auto auth = QStringLiteral("%1 Client=\"vibePlayerQT\", Device=\"Desktop\", DeviceId=\"vibe-player-qt-desktop\", Version=\"0.1.0\"")
                    .arg(scheme);
    if (!token.isEmpty()) {
        auth += QStringLiteral(", Token=\"%1\"").arg(token);
    }
    headers.insert("Authorization", auth.toUtf8());
    if (!token.isEmpty()) {
        headers.insert("X-Emby-Token", token.toUtf8());
    }
    return headers;
}
