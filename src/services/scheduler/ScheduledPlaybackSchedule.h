#pragma once

#include "models/ScheduledPlaybackTask.h"

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace ScheduledPlaybackSchedule {

inline constexpr auto manualType = "manual";
inline constexpr auto dailyType = "daily";
inline constexpr auto weeklyType = "weekly";
inline constexpr auto monthlyType = "monthly";
inline constexpr auto customMonthlyType = "custom_monthly";

bool isSupportedType(const QString& type);
bool isAutomatic(const ScheduledPlaybackTask& task);
QList<int> parseDays(const QString& value, int minimum, int maximum);
QString serializeDays(QList<int> days, int minimum, int maximum);
std::optional<QDateTime> nextOccurrence(const ScheduledPlaybackTask& task,
                                        const QDateTime& after);
std::optional<QDateTime> firstOccurrenceBetween(const ScheduledPlaybackTask& task,
                                                const QDateTime& after,
                                                const QDateTime& through);

}
