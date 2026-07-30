#pragma once

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QUrl>

struct LinkPlaybackHistoryItem {
    QString id;
    QUrl playbackUrl;
    QString displayName;
    QString displayAddress;
    QDate playedDate;
    QDateTime playedAt;
    bool privacyMode { false };
};
