#include "services/scheduler/ScheduledPlaybackSchedule.h"

#include <QTime>
#include <QTimeZone>
#include <QStringList>

#include <algorithm>

namespace ScheduledPlaybackSchedule {
namespace {
constexpr int maximumSearchDays = 370;

bool matchesDate(const ScheduledPlaybackTask& task, const QDate& date)
{
    if (task.scheduleType == QLatin1String(dailyType)) {
        return true;
    }

    if (task.scheduleType == QLatin1String(weeklyType)) {
        return parseDays(task.scheduleDays, 1, 7).contains(date.dayOfWeek());
    }

    if (task.scheduleType == QLatin1String(monthlyType) ||
        task.scheduleType == QLatin1String(customMonthlyType)) {
        return parseDays(task.scheduleDays, 1, 31).contains(date.day());
    }

    return false;
}
}

bool isSupportedType(const QString& type)
{
    return type == QLatin1String(manualType) ||
           type == QLatin1String(dailyType) ||
           type == QLatin1String(weeklyType) ||
           type == QLatin1String(monthlyType) ||
           type == QLatin1String(customMonthlyType);
}

bool isAutomatic(const ScheduledPlaybackTask& task)
{
    return task.enabled && task.scheduleType != QLatin1String(manualType) &&
           isSupportedType(task.scheduleType);
}

QList<int> parseDays(const QString& value, int minimum, int maximum)
{
    QList<int> days;
    for (const auto& part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const auto day = part.trimmed().toInt(&ok);
        if (ok && day >= minimum && day <= maximum && !days.contains(day)) {
            days.push_back(day);
        }
    }
    std::ranges::sort(days);
    return days;
}

QString serializeDays(QList<int> days, int minimum, int maximum)
{
    days.erase(std::remove_if(days.begin(), days.end(), [minimum, maximum](int day) {
        return day < minimum || day > maximum;
    }), days.end());
    std::ranges::sort(days);
    const auto uniqueEnd = std::ranges::unique(days).begin();
    days.erase(uniqueEnd, days.end());

    QStringList values;
    values.reserve(days.size());
    for (const auto day : days) {
        values.push_back(QString::number(day));
    }
    return values.join(QLatin1Char(','));
}

std::optional<QDateTime> nextOccurrence(const ScheduledPlaybackTask& task,
                                        const QDateTime& after)
{
    if (!after.isValid() || !isAutomatic(task)) {
        return std::nullopt;
    }

    const auto startTime = QTime::fromString(task.startTime, QStringLiteral("HH:mm"));
    if (!startTime.isValid()) {
        return std::nullopt;
    }

    for (auto dayOffset = 0; dayOffset <= maximumSearchDays; ++dayOffset) {
        const auto date = after.date().addDays(dayOffset);
        if (!matchesDate(task, date)) {
            continue;
        }
        if (task.lastRunDate == date.toString(Qt::ISODate)) {
            continue;
        }

        const QDateTime candidate(date, startTime, after.timeZone());
        if (candidate.isValid() && candidate > after) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::optional<QDateTime> firstOccurrenceBetween(const ScheduledPlaybackTask& task,
                                                const QDateTime& after,
                                                const QDateTime& through)
{
    if (!through.isValid() || through <= after) {
        return std::nullopt;
    }
    const auto occurrence = nextOccurrence(task, after);
    return occurrence && *occurrence <= through ? occurrence : std::nullopt;
}

}
