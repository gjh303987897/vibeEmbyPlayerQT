#pragma once

#include "services/media/MediaServerClientBase.h"

class JellyfinClient final : public MediaServerClientBase {
    Q_OBJECT

public:
    explicit JellyfinClient(NetworkClient& networkClient, QObject* parent = nullptr);

    void login(const ServerConfig& server,
               const QString& username,
               const QString& password,
               std::function<void(LoginResult)> callback) override;

    void fetchLibraries(const UserSession& session, std::function<void(LibraryResult)> callback) override;

    void fetchLibraryItems(const UserSession& session,
                           const MediaLibrary& library,
                           const QString& parentId,
                           int startIndex,
                           int limit,
                           std::function<void(ItemResult)> callback) override;

    void searchVideoItems(const UserSession& session,
                          const QString& searchTerm,
                          int startIndex,
                          int limit,
                          std::function<void(ItemResult)> callback) override;

    void fetchContinueWatching(const UserSession& session, int limit, std::function<void(ItemResult)> callback) override;

    void fetchSeriesSeasons(const UserSession& session,
                            const QString& seriesId,
                            std::function<void(ItemResult)> callback) override;

    void fetchSeasonEpisodes(const UserSession& session,
                             const QString& seriesId,
                             const QString& seasonId,
                             std::function<void(ItemResult)> callback) override;

    void fetchItemDetails(const UserSession& session,
                          const QString& itemId,
                          std::function<void(std::expected<MediaItem, NetworkError>)> callback) override;

    void fetchPlaybackUrl(const UserSession& session, const MediaItem& item, PlaybackUrlCallback callback) override;
    void reportPlaybackStart(const UserSession& session, const PlaybackReport& report) override;
    void reportPlaybackProgress(const UserSession& session, const PlaybackReport& report) override;
    void reportPlaybackStopped(const UserSession& session, const PlaybackReport& report) override;
};
