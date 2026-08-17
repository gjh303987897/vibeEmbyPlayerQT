#include "services/emby/EmbyClient.h"

#include "utils/AppLogger.h"
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

QStringList genreNamesFromArray(const QJsonArray& values)
{
    QStringList genres;
    for (const auto& value : values) {
        const auto name = value.isString()
            ? value.toString().trimmed()
            : jsonStringAny(value.toObject(), { QStringLiteral("Name"), QStringLiteral("name") }).trimmed();
        if (!name.isEmpty() && !genres.contains(name, Qt::CaseInsensitive)) {
            genres.append(name);
        }
    }
    std::sort(genres.begin(), genres.end(), [](const QString& left, const QString& right) {
        return left.localeAwareCompare(right) < 0;
    });
    return genres;
}

std::expected<QStringList, NetworkError> parseFilterGenres(const QByteArray& body)
{
    const auto root = parseObject(body);
    if (!root) {
        return std::unexpected(root.error());
    }

    const auto values = root->value(QStringLiteral("Genres"))
                            .toArray(root->value(QStringLiteral("genres")).toArray());
    return genreNamesFromArray(values);
}

std::expected<QStringList, NetworkError> parseGenreItems(const QByteArray& body)
{
    const auto root = parseObject(body);
    if (!root) {
        return std::unexpected(root.error());
    }
    return genreNamesFromArray(root->value(QStringLiteral("Items"))
                                   .toArray(root->value(QStringLiteral("items")).toArray()));
}

void addSuggestedSeriesFields(QUrlQuery& query)
{
    query.addQueryItem(QStringLiteral("Fields"),
                       QStringLiteral("PrimaryImageAspectRatio,Overview,Genres,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,BackdropImageTags,ParentId"));
    query.addQueryItem(QStringLiteral("EnableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("ImageTypeLimit"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("EnableImageTypes"), QStringLiteral("Primary,Backdrop"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
}

void keepSeriesItems(std::vector<MediaItem>& items)
{
    std::erase_if(items, [](const MediaItem& item) {
        return item.itemType.compare(QStringLiteral("Series"), Qt::CaseInsensitive) != 0;
    });
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

        parseLibrariesAsync(result->body, session.server.baseUrl, session.accessToken,
                            std::move(callback));
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

        parseItemsAsync(result->body, session.server.baseUrl, session.accessToken,
                        std::move(callback));
    });
}

void EmbyClient::searchVideoItems(const UserSession& session,
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

    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Users/%1/Items").arg(session.userId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("SearchTerm"), normalizedTerm);
    query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Movie,Series,Video"));
    query.addQueryItem(QStringLiteral("StartIndex"), QString::number(std::max(0, startIndex)));
    query.addQueryItem(QStringLiteral("Limit"), QString::number(std::max(1, limit)));
    query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("SortName"));
    query.addQueryItem(QStringLiteral("SortOrder"), QStringLiteral("Ascending"));
    query.addQueryItem(QStringLiteral("Fields"), QStringLiteral("PrimaryImageAspectRatio"));
    query.addQueryItem(QStringLiteral("EnableImages"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("ImageTypeLimit"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("EnableImageTypes"), QStringLiteral("Primary"));
    query.addQueryItem(QStringLiteral("EnableUserData"), QStringLiteral("true"));
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
        parseItemsAsync(result->body, session.server.baseUrl, session.accessToken,
                        std::move(callback));
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

        parseItemsAsync(result->body, session.server.baseUrl, session.accessToken,
                        std::move(callback));
    });
}

void EmbyClient::fetchSuggestedSeries(const UserSession& session,
                                      int limit,
                                      std::function<void(ItemResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Users/%1/Suggestions").arg(session.userId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Series"));
    query.addQueryItem(QStringLiteral("Limit"), QString::number(std::max(1, limit)));
    addSuggestedSeriesFields(query);
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);
    m_networkClient.get(url,
                        headers,
                        session.server.trustSelfSignedCertificate,
                        [this, session, limit, callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }
        parseItemsAsync(result->body, session.server.baseUrl, session.accessToken,
                        [this, session, limit, callback = std::move(callback)](ItemResult parsed) mutable {
            if (!parsed) {
                callback(std::unexpected(parsed.error()));
                return;
            }

            auto items = std::move(*parsed);
            keepSeriesItems(items);
            if (!items.empty()) {
                callback(std::move(items));
                return;
            }

            AppLogger::warning(QStringLiteral("recommendations"),
                               QStringLiteral("Emby Suggestions returned no Series items; using the user-items fallback for %1")
                                   .arg(QUrl(session.server.baseUrl).host()));
            fetchSuggestedSeriesFallback(session, limit, std::move(callback));
        });
    });
}

void EmbyClient::fetchSeriesGenres(const UserSession& session, std::function<void(GenreResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Items/Filters"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("UserId"), session.userId);
    query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Series"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);
    m_networkClient.get(url,
                        headers,
                        session.server.trustSelfSignedCertificate,
                        [this, session, callback = std::move(callback)](NetworkResult result) mutable {
        if (result) {
            auto genres = parseFilterGenres(result->body);
            if (genres && !genres->isEmpty()) {
                callback(std::move(genres));
                return;
            }
        }

        AppLogger::warning(QStringLiteral("recommendations"),
                           QStringLiteral("Emby item filters did not provide series genres; using the genres endpoint for %1")
                               .arg(QUrl(session.server.baseUrl).host()));
        fetchSeriesGenresFallback(session, std::move(callback));
    });
}

void EmbyClient::fetchSeriesGenresFallback(const UserSession& session,
                                           std::function<void(GenreResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Genres"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("UserId"), session.userId);
    query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Series"));
    query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("SortName"));
    query.addQueryItem(QStringLiteral("SortOrder"), QStringLiteral("Ascending"));
    url.setQuery(query);

    const auto headers = authHeaders(QStringLiteral("Emby"), session.accessToken);
    m_networkClient.get(url,
                        headers,
                        session.server.trustSelfSignedCertificate,
                        [callback = std::move(callback)](NetworkResult result) mutable {
        if (!result) {
            callback(std::unexpected(result.error()));
            return;
        }
        callback(parseGenreItems(result->body));
    });
}

void EmbyClient::fetchSuggestedSeriesFallback(const UserSession& session,
                                              int limit,
                                              std::function<void(ItemResult)> callback)
{
    auto url = makeUrl(session.server.baseUrl, QStringLiteral("/Users/%1/Items").arg(session.userId));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Series"));
    query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("Random"));
    query.addQueryItem(QStringLiteral("Limit"), QString::number(std::max(1, limit)));
    addSuggestedSeriesFields(query);
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
        parseItemsAsync(result->body,
                        session.server.baseUrl,
                        session.accessToken,
                        [callback = std::move(callback)](ItemResult parsed) mutable {
            if (!parsed) {
                callback(std::unexpected(parsed.error()));
                return;
            }
            auto items = std::move(*parsed);
            keepSeriesItems(items);
            callback(std::move(items));
        });
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
        parseItemsAsync(result->body, session.server.baseUrl, session.accessToken,
                        std::move(callback));
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

        parseItemsAsync(result->body, session.server.baseUrl, session.accessToken,
                        std::move(callback));
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
    query.addQueryItem(QStringLiteral("EnableImageTypes"), QStringLiteral("Primary,Backdrop,Logo"));
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

        parseItemsAsync(result->body, session.server.baseUrl, session.accessToken,
                        std::move(callback));
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
    query.addQueryItem(QStringLiteral("EnableImageTypes"), QStringLiteral("Primary,Backdrop,Logo"));
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
