#pragma once

#include "models/MediaItem.h"
#include "models/MediaLibrary.h"
#include "models/ServerConfig.h"
#include "models/UserSession.h"
#include "network/NetworkClient.h"
#include "services/media/MediaServiceClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QUrl>

#include <expected>
#include <vector>

class MediaServerClientBase : public MediaServiceClient {
    Q_OBJECT

public:
    explicit MediaServerClientBase(NetworkClient& networkClient, QObject* parent = nullptr);

protected:
    static QUrl makeUrl(const QString& baseUrl, const QString& path);
    static QString normalizeBaseUrl(QString baseUrl);
    static MediaLibrary parseLibrary(const QJsonObject& object, const QString& baseUrl, const QString& token);
    static MediaItem parseItem(const QJsonObject& object, const QString& baseUrl, const QString& token);
    static std::vector<MediaLibrary> parseLibraries(const QByteArray& body, const QString& baseUrl, const QString& token);
    static std::vector<MediaItem> parseItems(const QByteArray& body, const QString& baseUrl, const QString& token);
    static std::expected<MediaItem, NetworkError> parseItemDetails(const QByteArray& body, const QString& baseUrl, const QString& token);
    static QString primaryImageUrl(const QString& baseUrl, const QString& itemId, const QString& imageTag, const QString& token, int width);
    static QString backdropImageUrl(const QString& baseUrl, const QString& itemId, const QString& imageTag, const QString& token, int width);
    static PlaybackUrlResult streamUrl(const UserSession& session,
                                       const MediaItem& item,
                                       const QString& mediaSourceId,
                                       const QString& playSessionId);
    void fetchPlaybackUrlWithScheme(const QString& authScheme,
                                    const UserSession& session,
                                    const MediaItem& item,
                                    PlaybackUrlCallback callback);
    void postPlaybackReport(const QString& authScheme, const UserSession& session, const QString& path, const PlaybackReport& report);
    static QHash<QByteArray, QByteArray> authHeaders(const QString& scheme, const QString& token = {});

    NetworkClient& m_networkClient;
};
