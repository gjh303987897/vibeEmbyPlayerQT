#pragma once

#include "models/MediaItem.h"
#include "models/MediaLibrary.h"
#include "models/ServerConfig.h"
#include "models/UserSession.h"
#include "network/NetworkError.h"

#include <QObject>
#include <QList>
#include <QSslError>

#include <expected>
#include <functional>
#include <vector>

using LoginResult = std::expected<UserSession, NetworkError>;
using LibraryResult = std::expected<std::vector<MediaLibrary>, NetworkError>;
using ItemResult = std::expected<std::vector<MediaItem>, NetworkError>;

struct PlaybackRequest {
    QUrl url;
    double startSeconds { 0.0 };
    QString mediaSourceId;
    QString playSessionId;
};

struct PlaybackReport {
    QString itemId;
    QString mediaSourceId;
    QString playSessionId;
    qint64 positionTicks { 0 };
    bool paused { false };
};

using PlaybackUrlResult = std::expected<PlaybackRequest, NetworkError>;
using PlaybackUrlCallback = std::function<void(PlaybackUrlResult)>;

class MediaServiceClient : public QObject {
    Q_OBJECT

public:
    explicit MediaServiceClient(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    ~MediaServiceClient() override = default;

    virtual void login(const ServerConfig& server,
                       const QString& username,
                       const QString& password,
                       std::function<void(LoginResult)> callback) = 0;

    virtual void fetchLibraries(const UserSession& session, std::function<void(LibraryResult)> callback) = 0;

    virtual void fetchLibraryItems(const UserSession& session,
                                   const MediaLibrary& library,
                                   const QString& parentId,
                                   int startIndex,
                                   int limit,
                                   std::function<void(ItemResult)> callback) = 0;

    virtual void fetchContinueWatching(const UserSession& session, int limit, std::function<void(ItemResult)> callback) = 0;

    virtual void fetchSeriesSeasons(const UserSession& session,
                                    const QString& seriesId,
                                    std::function<void(ItemResult)> callback) = 0;

    virtual void fetchSeasonEpisodes(const UserSession& session,
                                     const QString& seriesId,
                                     const QString& seasonId,
                                     std::function<void(ItemResult)> callback) = 0;

    virtual void fetchItemDetails(const UserSession& session,
                                  const QString& itemId,
                                  std::function<void(std::expected<MediaItem, NetworkError>)> callback) = 0;

    virtual void fetchPlaybackUrl(const UserSession& session, const MediaItem& item, PlaybackUrlCallback callback) = 0;

    virtual void reportPlaybackStart(const UserSession& session, const PlaybackReport& report) = 0;
    virtual void reportPlaybackProgress(const UserSession& session, const PlaybackReport& report) = 0;
    virtual void reportPlaybackStopped(const UserSession& session, const PlaybackReport& report) = 0;

signals:
    void certificateConfirmationRequired(const QString& host, const QList<QSslError>& errors, std::function<void(bool)> reply);
};
