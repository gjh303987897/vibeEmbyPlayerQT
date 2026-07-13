#pragma once

#include <QString>
#include <QtGlobal>

struct DailyUsageStat {
    QString date;
    QString serviceId;
    QString serviceName;
    QString serviceType;
    qint64 watchSeconds { 0 };
    qint64 networkBytesIn { 0 };
    qint64 networkBytesOut { 0 };
    qint64 keepAliveNetworkBytesIn { 0 };
    qint64 keepAliveNetworkBytesOut { 0 };
    bool privacyMode { false };
};

enum class NetworkTrafficCategory {
    Normal,
    KeepAlive,
};
