#pragma once

#include "models/MediaItem.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QStringList>

#include <expected>
#include <span>
#include <vector>

class EmbyRecommendationCache final {
public:
    static QByteArray serialize(std::span<const MediaItem> items);
    static std::expected<std::vector<MediaItem>, QString> deserialize(const QByteArray& payload,
                                                                      const QString& accessToken);
    static std::vector<MediaItem> filtered(std::span<const MediaItem> items,
                                           const QStringList& excludedGenres,
                                           qsizetype limit);
    static bool refreshedToday(const QDateTime& refreshedAt,
                               const QDate& localDate = QDate::currentDate());

private:
    EmbyRecommendationCache() = delete;
};
