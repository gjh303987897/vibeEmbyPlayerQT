#include "services/emby/EmbyClient.h"

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

EmbyClient::EmbyClient(NetworkClient& networkClient, QObject* parent)
    : MediaServerClientBase(networkClient, parent)
{
}

void EmbyClient::login(const ServerConfig& server,
                       const QString& username,
                       const QString& password,
                       std::function<void(LoginResult)> callback)
{
    const auto url = makeUrl(server.baseUrl, QStringLiteral("/Users/AuthenticateByName"));
    const auto headers = authHeaders(QStringLiteral("Emby"));
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

        const auto user = root->value(QStringLiteral("User")).toObject();
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
            .username = name.isEmpty() ? QStringLiteral("Emby User") : name,
            .accessToken = token,
            .createdAt = QDateTime::currentDateTimeUtc(),
        });
    });
}

void EmbyClient::fetchLibraries(const UserSession& session, std::function<void(LibraryResult)> callback)
{
    const auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Users/%1/Views").arg(session.userId));
    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);

    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseLibraries(result->body, session.server.baseUrl, session.accessToken));
    });
}

void EmbyClient::fetchLibraryItems(const UserSession& session,
                                   const MediaLibrary& library,
                                   const QString& parentId,
                                   int startIndex,
                                   int limit,
                                   std::function<void(ItemResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Users/%1/Items").arg(session.userId));
    QUrlQuery query;
    const auto effectiveParentId = parentId.isEmpty() ? library.id : parentId;
    query.addQueryItem(QStringLiteral("ParentId"), effectiveParentId);
    query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("StartIndex"), QString::number(startIndex));
    query.addQueryItem(QStringLiteral("Limit"), QString::number(limit));
    query.addQueryItem(QStringLiteral("Fields"), QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,DateCreated,SeriesPrimaryImageTag,ParentId"));
    query.addQueryItem(QStringLiteral("EnableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));

    if (effectiveParentId == library.id && library.collectionType.compare(QStringLiteral("movies"), Qt::CaseInsensitive) == 0) {
        query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Movie"));
    } else if (effectiveParentId == library.id && library.collectionType.compare(QStringLiteral("tvshows"), Qt::CaseInsensitive) == 0) {
        query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Series"));
    }

    url.setQuery(query);
    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);

    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseItems(result->body, session.server.baseUrl, session.accessToken));
    });
}

void EmbyClient::fetchContinueWatching(const UserSession& session, int limit, std::function<void(ItemResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Users/%1/Items").arg(session.userId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("Filters"), QStringLiteral("IsResumable"));
    query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Movie,Episode"));
    query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("DatePlayed"));
    query.addQueryItem(QStringLiteral("SortOrder"), QStringLiteral("Descending"));
    query.addQueryItem(QStringLiteral("Limit"), QString::number(limit));
    query.addQueryItem(QStringLiteral("Fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,People,DateCreated,RunTimeTicks,SeriesPrimaryImageTag,ParentId"));
    query.addQueryItem(QStringLiteral("EnableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);
    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseItems(result->body, session.server.baseUrl, session.accessToken));
    });
}

void EmbyClient::fetchRandomPlayableItems(const UserSession& session,
                                          int limit,
                                          std::function<void(ItemResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Users/%1/Items").arg(session.userId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("Filters"), QStringLiteral("IsNotFolder"));
    query.addQueryItem(QStringLiteral("MediaTypes"), QStringLiteral("Video"));
    query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Movie,Episode"));
    query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("Random"));
    query.addQueryItem(QStringLiteral("Limit"), QString::number(std::max(1, limit)));
    query.addQueryItem(QStringLiteral("Fields"), QStringLiteral("RunTimeTicks,SeriesPrimaryImageTag,ParentId"));
    query.addQueryItem(QStringLiteral("EnableImages"), QStringLiteral("false"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("false"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);
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

void EmbyClient::fetchSeriesSeasons(const UserSession& session,
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
    query.addQueryItem(QStringLiteral("UserId"), session.userId);
    query.addQueryItem(QStringLiteral("Fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,SeriesPrimaryImageTag"));
    query.addQueryItem(QStringLiteral("EnableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);
    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseItems(result->body, session.server.baseUrl, session.accessToken));
    });
}

void EmbyClient::fetchSeasonEpisodes(const UserSession& session,
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
    query.addQueryItem(QStringLiteral("UserId"), session.userId);
    query.addQueryItem(QStringLiteral("SeasonId"), seasonId);
    query.addQueryItem(QStringLiteral("Fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,People,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,BackdropImageTags,SeriesPrimaryImageTag,ParentId"));
    query.addQueryItem(QStringLiteral("EnableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("SortName"));
    query.addQueryItem(QStringLiteral("SortOrder"), QStringLiteral("Ascending"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);
    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        callback(parseItems(result->body, session.server.baseUrl, session.accessToken));
    });
}

void EmbyClient::fetchItemDetails(const UserSession& session,
                                  const QString& itemId,
                                  std::function<void(std::expected<MediaItem, NetworkError>)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Users/%1/Items").arg(session.userId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("Ids"), itemId);
    query.addQueryItem(QStringLiteral("Fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,People,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,BackdropImageTags,SeriesPrimaryImageTag,ParentId"));
    query.addQueryItem(QStringLiteral("EnableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);
    m_networkClient.get(url, headers, session.server.trustSelfSignedCertificate, [session, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }

        const auto items = parseItems(result->body, session.server.baseUrl, session.accessToken);
        if (items.empty()) {
            callback(std::unexpected(parseError(QStringLiteral("Media item was not found"))));
            return;
        }
        callback(items.front());
    });
}

void EmbyClient::fetchPlaybackUrl(const UserSession& session, const MediaItem& item, PlaybackUrlCallback callback)
{
    fetchPlaybackUrlWithScheme(QStringLiteral("Emby"), session, item, std::move(callback));
}

void EmbyClient::reportPlaybackStart(const UserSession& session, const PlaybackReport& report)
{
    postPlaybackReport(QStringLiteral("Emby"), session, QStringLiteral("/Sessions/Playing"), report);
}

void EmbyClient::reportPlaybackProgress(const UserSession& session, const PlaybackReport& report)
{
    postPlaybackReport(QStringLiteral("Emby"), session, QStringLiteral("/Sessions/Playing/Progress"), report);
}

void EmbyClient::reportPlaybackStopped(const UserSession& session, const PlaybackReport& report)
{
    postPlaybackReport(QStringLiteral("Emby"), session, QStringLiteral("/Sessions/Playing/Stopped"), report);
}
