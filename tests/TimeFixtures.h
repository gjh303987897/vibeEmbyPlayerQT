#pragma once

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QTime>

// Relative-date helpers for fixtures that write playback history.
//
// SessionRepository::pruneOldHistory() deletes every row whose played_date is older than
// QDate::currentDate() minus (historyRetentionDays() - 1), and repository.initialize() runs that prune.
// Fixtures with hard-coded calendar dates therefore stop working the moment they age out of the retention
// window - every read comes back empty while the writes still succeed, which reads like a database bug.
// Anchor the fixtures to "now" instead, and keep the day offsets small (0..2).
//
// Timestamps are built in local time and converted to UTC: SessionRepository derives played_date from the
// local-time date of playedAt, which is also the reference the prune cutoff uses, so this keeps the stored
// bucket on the intended day while preserving the "....T..:..:..Z" shape the fixtures already used.
namespace TimeFixtures {

inline QDateTime playedAt(int daysAgo, int hour, int minute = 0)
{
    return QDateTime(QDate::currentDate().addDays(-daysAgo), QTime(hour, minute)).toUTC();
}

inline QString stampAt(int daysAgo, int hour, int minute = 0)
{
    return playedAt(daysAgo, hour, minute).toString(Qt::ISODate);
}

// Same instant in the millisecond ISO form ("....T..:..:..000Z") used by the raw SQL fixtures.
inline QString stampWithMsAt(int daysAgo, int hour, int minute = 0)
{
    return playedAt(daysAgo, hour, minute).toString(Qt::ISODateWithMs);
}

inline QDate dayAt(int daysAgo)
{
    return QDate::currentDate().addDays(-daysAgo);
}

inline QString dateAt(int daysAgo)
{
    return dayAt(daysAgo).toString(Qt::ISODate);
}

}
