#pragma once

#include <QString>

struct ScheduledPlaybackTask {
    QString id;
    QString serverId;
    QString serverName;
    QString username;
    QString scheduleType { QStringLiteral("manual") };
    QString startTime { QStringLiteral("manual") };
    QString scheduleDays;
    int durationMinutes { 90 };
    bool enabled { true };
    QString lastRunDate { QStringLiteral("") };
    QString createdAt;
    bool privateMode { false };
};
