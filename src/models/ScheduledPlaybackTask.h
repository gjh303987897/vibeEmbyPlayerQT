#pragma once

#include <QString>

struct ScheduledPlaybackTask {
    QString id;
    QString serverId;
    QString serverName;
    QString username;
    QString startTime { QStringLiteral("manual") };
    int durationMinutes { 90 };
    bool enabled { true };
    QString lastRunDate { QStringLiteral("") };
    bool privateMode { false };
};
