#include "services/jellyfin/JellyfinClient.h"

#include "utils/JsonUtils.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

#include <algorithm>
#include <expected>
#include <utility>

namespace {
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
}

JellyfinClient::JellyfinClient(NetworkClient& networkClient, QObject* parent)
    : MediaServerClientBase(networkClient, parent)
{
}

void JellyfinClient::login(const ServerConfig& server,
                           const QString& username,
                           const QString& password,
                           std::function<void(LoginResult)> callback)
{
    const auto url = makeUrl(server.baseUrl, QStringLiteral("/Users/AuthenticateByName"));
    const auto headers = authHeaders(QStringLiteral("MediaBrowser"));
    const QJsonObject body {
        { QStringLiteral("Username"), username },
        { QStringLiteral("Pw"), password },
    };

    m_networkClient.postJson(url, headers, body, server.trustSelfSignedCertificate, [server, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        const auto root = parseObject(result->body);
        if (!root) {
            callback(std::unexpected(root.error()));
            return;
        }

        const auto user = root->value(QStringLiteral("User")).toObject(root->value(QStringLiteral("user")).toObject());
        const auto token = jsonStringAny(*root, { QStringLiteral("AccessToken"), QStringLiteral("accessToken") });
        const auto userId = jsonStringAny(user, { QStringLiteral("Id"), QStringLiteral("id") });
        const auto name = jsonStringAny(user, { QStringLiteral("Name"), QStringLiteral("name") });

        if (token.isEmpty() || userId.isEmpty()) {
            callback(std::unexpected(parseError(QStringLiteral("Authentication response is missing token or user id"))));
            return;
        }

        callback(UserSession {
            .server = server,
            .userId = userId,
            .username = name.isEmpty() ? QStringLiteral("Jellyfin User") : name,
            .accessToken = token,
            .createdAt = QDateTime::currentDateTimeUtc(),
        });
    });
}

void JellyfinClient::fetchLibraries(const UserSession& session, std::function<void(LibraryResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/UserViews"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), session.userId);
    query.addQueryItem(QStringLiteral("includeHidden"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("includeExternalContent"), QStringLiteral("false"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("MediaBrowser"), session.accessToken);

    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseLibraries(result->body, session.server.baseUrl, session.accessToken));
    });
}

void JellyfinClient::fetchLibraryItems(const UserSession& session,
                                       const MediaLibrary& library,
                                       const QString& parentId,
                                       int startIndex,
                                       int limit,
                                       std::function<void(ItemResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Items"));
    QUrlQuery query;
    const auto effectiveParentId = parentId.isEmpty() ? library.id : parentId;
    query.addQueryItem(QStringLiteral("userId"), session.userId);
    query.addQueryItem(QStringLiteral("parentId"), effectiveParentId);
    query.addQueryItem(QStringLiteral("recursive"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("startIndex"), QString::number(startIndex));
    query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,DateCreated,SeriesPrimaryImageTag,ParentId"));
    query.addQueryItem(QStringLiteral("enableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("enableUserData"), QStringLiteral("true"));

    if (effectiveParentId == library.id && library.collectionType.compare(QStringLiteral("movies"), Qt::CaseInsensitive) == 0) {
        query.addQueryItem(QStringLiteral("includeItemTypes"), QStringLiteral("Movie"));
    } else if (effectiveParentId == library.id && library.collectionType.compare(QStringLiteral("tvshows"), Qt::CaseInsensitive) == 0) {
        query.addQueryItem(QStringLiteral("includeItemTypes"), QStringLiteral("Series"));
    }

    url.setQuery(query);
    const auto headers = authHeaders(QStringLiteral("MediaBrowser"), session.accessToken);

    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseItems(result->body, session.server.baseUrl, session.accessToken));
    });
}

void JellyfinClient::searchVideoItems(const UserSession& session,
                                      const QString& searchTerm,
                                      int startIndex,
                                      int limit,
                                      std::function<void(ItemResult)> callback)
{
    const auto normalizedTerm = searchTerm.trimmed();
    if (normalizedTerm.isEmpty()) {
        callback(std::unexpected(NetworkError {
            .kind = NetworkErrorKind::InvalidUrl,
            .message = QStringLiteral("Search term is required"),
        }));
        return;
    }

    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Items"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), session.userId);
    query.addQueryItem(QStringLiteral("searchTerm"), normalizedTerm);
    query.addQueryItem(QStringLiteral("recursive"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("includeItemTypes"), QStringLiteral("Movie,Series,Episode,Video"));
    query.addQueryItem(QStringLiteral("startIndex"), QString::number(std::max(0, startIndex)));
    query.addQueryItem(QStringLiteral("limit"), QString::number(std::max(1, limit)));
    query.addQueryItem(QStringLiteral("sortBy"), QStringLiteral("SortName"));
    query.addQueryItem(QStringLiteral("sortOrder"), QStringLiteral("Ascending"));
    query.addQueryItem(QStringLiteral("fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,BackdropImageTags,SeriesPrimaryImageTag,ParentId"));
    query.addQueryItem(QStringLiteral("enableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("enableUserData"), QStringLiteral("true"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("MediaBrowser"), session.accessToken);
    m_networkClient.get(url,
                        headers,
                        session.server.trustSelfSignedCertificate,
                        [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }
        callback(parseItems(result->body, session.server.baseUrl, session.accessToken));
    });
}

void JellyfinClient::fetchContinueWatching(const UserSession& session, int limit, std::function<void(ItemResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/UserItems/Resume"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), session.userId);
    query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    query.addQueryItem(QStringLiteral("includeItemTypes"), QStringLiteral("Movie,Episode"));
    query.addQueryItem(QStringLiteral("fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,People,DateCreated,RunTimeTicks,SeriesPrimaryImageTag,ParentId"));
    query.addQueryItem(QStringLiteral("enableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("enableUserData"), QStringLiteral("true"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("MediaBrowser"), session.accessToken);
    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseItems(result->body, session.server.baseUrl, session.accessToken));
    });
}

void JellyfinClient::fetchSeriesSeasons(const UserSession& session,
                                        const QString& seriesId,
                                        std::function<void(ItemResult)> callback)
{
    if (seriesId.isEmpty()) {
        callback(std::unexpected(NetworkError {
            .kind = NetworkErrorKind::InvalidUrl,
            .message = QStringLiteral("Series id is required"),
        }));
        return;
    }

    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Shows/%1/Seasons").arg(seriesId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), session.userId);
    query.addQueryItem(QStringLiteral("fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,SeriesPrimaryImageTag"));
    query.addQueryItem(QStringLiteral("enableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("enableUserData"), QStringLiteral("true"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("MediaBrowser"), session.accessToken);
    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseItems(result->body, session.server.baseUrl, session.accessToken));
    });
}

void JellyfinClient::fetchSeasonEpisodes(const UserSession& session,
                                         const QString& seriesId,
                                         const QString& seasonId,
                                         std::function<void(ItemResult)> callback)
{
    if (seriesId.isEmpty() || seasonId.isEmpty()) {
        callback(std::unexpected(NetworkError {
            .kind = NetworkErrorKind::InvalidUrl,
            .message = QStringLiteral("Series and season ids are required"),
        }));
        return;
    }

    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Shows/%1/Episodes").arg(seriesId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), session.userId);
    query.addQueryItem(QStringLiteral("seasonId"), seasonId);
    query.addQueryItem(QStringLiteral("fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,People,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,BackdropImageTags,SeriesPrimaryImageTag,ParentId"));
    query.addQueryItem(QStringLiteral("enableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("enableUserData"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("sortBy"), QStringLiteral("SortName"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("MediaBrowser"), session.accessToken);
    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseItems(result->body, session.server.baseUrl, session.accessToken));
    });
}

void JellyfinClient::fetchItemDetails(const UserSession& session,
                                      const QString& itemId,
                                      std::function<void(std::expected<MediaItem, NetworkError>)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Items/%1").arg(itemId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("userId"), session.userId);
    query.addQueryItem(QStringLiteral("fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,People,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,BackdropImageTags,SeriesPrimaryImageTag,ParentId"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("MediaBrowser"), session.accessToken);
    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseItemDetails(result->body, session.server.baseUrl, session.accessToken));
    });
}

void JellyfinClient::fetchPlaybackUrl(const UserSession& session, const MediaItem& item, PlaybackUrlCallback callback)
{
    fetchPlaybackUrlWithScheme(QStringLiteral("MediaBrowser"), session, item, std::move(callback));
}

void JellyfinClient::reportPlaybackStart(const UserSession& session, const PlaybackReport& report)
{
    postPlaybackReport(QStringLiteral("MediaBrowser"), session, QStringLiteral("/Sessions/Playing"), report);
}

void JellyfinClient::reportPlaybackProgress(const UserSession& session, const PlaybackReport& report)
{
    postPlaybackReport(QStringLiteral("MediaBrowser"), session, QStringLiteral("/Sessions/Playing/Progress"), report);
}

void JellyfinClient::reportPlaybackStopped(const UserSession& session, const PlaybackReport& report)
{
    postPlaybackReport(QStringLiteral("MediaBrowser"), session, QStringLiteral("/Sessions/Playing/Stopped"), report);
}
