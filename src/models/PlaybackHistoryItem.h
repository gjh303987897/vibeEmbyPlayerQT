#pragma once

#include <QDate>
#include <QDateTime>
#include <QString>

enum class PlaybackHistorySource {
    Emby,
    Jellyfin,
    Iptv,
    WebDav,
    Local,
    Link,
    Unknown,
};

inline QString playbackHistorySourceToString(PlaybackHistorySource source)
{
    switch (source) {
    case PlaybackHistorySource::Emby:
        return QStringLiteral("Emby");
    case PlaybackHistorySource::Jellyfin:
        return QStringLiteral("Jellyfin");
    case PlaybackHistorySource::Iptv:
        return QStringLiteral("IPTV");
    case PlaybackHistorySource::WebDav:
        return QStringLiteral("WebDAV");
    case PlaybackHistorySource::Local:
        return QStringLiteral("Local");
    case PlaybackHistorySource::Link:
        return QStringLiteral("Link");
    case PlaybackHistorySource::Unknown:
        break;
    }
    return QStringLiteral("Unknown");
}

inline PlaybackHistorySource playbackHistorySourceFromString(const QString& value)
{
    if (value.compare(QStringLiteral("Emby"), Qt::CaseInsensitive) == 0) {
        return PlaybackHistorySource::Emby;
    }
    if (value.compare(QStringLiteral("Jellyfin"), Qt::CaseInsensitive) == 0) {
        return PlaybackHistorySource::Jellyfin;
    }
    if (value.compare(QStringLiteral("IPTV"), Qt::CaseInsensitive) == 0) {
        return PlaybackHistorySource::Iptv;
    }
    if (value.compare(QStringLiteral("WebDAV"), Qt::CaseInsensitive) == 0) {
        return PlaybackHistorySource::WebDav;
    }
    if (value.compare(QStringLiteral("Local"), Qt::CaseInsensitive) == 0) {
        return PlaybackHistorySource::Local;
    }
    if (value.compare(QStringLiteral("Link"), Qt::CaseInsensitive) == 0) {
        return PlaybackHistorySource::Link;
    }
    return PlaybackHistorySource::Unknown;
}

struct PlaybackHistoryItem {
    QString id;
    PlaybackHistorySource source { PlaybackHistorySource::Unknown };
    QString serviceId;
    QString serviceName;
    QString replayTarget;
    QString title;
    QString subtitle;
    QString displayTarget;
    QDate playedDate;
    QDateTime playedAt;
    QDateTime updatedAt;
    qint64 positionSeconds { 0 };
    qint64 durationSeconds { 0 };
    bool completed { false };
    bool privacyMode { false };
    bool available { true };
};
