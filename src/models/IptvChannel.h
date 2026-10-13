#pragma once

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <vector>

struct IptvStreamVariant final {
    QString url;
    int width { 0 };
    int height { 0 };
    qint64 bitrate { 0 };
    QString codec;
    QDateTime failedAt;
    QString probeState;
};

// Information belongs to a particular route, not to the merged channel.
// HLS variants retain their actual URLs so selecting a quality also affects playback.
struct IptvStreamInfo final {
    QString url;
    bool available { false };
    int width { 0 };
    int height { 0 };
    qint64 bitrate { 0 };
    QString codec;
    QString state;
    QDateTime checkedAt;
    std::vector<IptvStreamVariant> variants;
    QDateTime qualityCheckedAt;
};

struct IptvChannel {
    QString id;
    QString playlistId;
    QString name;
    QString groupName;
    QString logoUrl;
    QString streamUrl;
    int sortOrder { 0 };
    // The first URL is kept in streamUrl for compatibility with existing
    // callers. Additional URLs are retained as ordered fallback sources.
    QString tvgId;
    QStringList streamUrls;
    int width { 0 };
    int height { 0 };
    qint64 bitrate { 0 };
    QString codec;
    QString probeState;
    std::vector<IptvStreamInfo> streamInfo;
};
