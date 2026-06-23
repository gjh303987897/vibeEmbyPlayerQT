#pragma once

#include <QString>

enum class ServiceType {
    Emby,
    Jellyfin,
    IPTV,
    WebDAV
};

inline QString serviceTypeToString(ServiceType type)
{
    switch (type) {
    case ServiceType::Emby:
        return QStringLiteral("Emby");
    case ServiceType::Jellyfin:
        return QStringLiteral("Jellyfin");
    case ServiceType::IPTV:
        return QStringLiteral("IPTV");
    case ServiceType::WebDAV:
        return QStringLiteral("WebDAV");
    }
    return QStringLiteral("Emby");
}

inline ServiceType serviceTypeFromString(const QString& value)
{
    if (value.compare(QStringLiteral("Jellyfin"), Qt::CaseInsensitive) == 0) {
        return ServiceType::Jellyfin;
    }
    if (value.compare(QStringLiteral("IPTV"), Qt::CaseInsensitive) == 0) {
        return ServiceType::IPTV;
    }
    if (value.compare(QStringLiteral("WebDAV"), Qt::CaseInsensitive) == 0) {
        return ServiceType::WebDAV;
    }
    return ServiceType::Emby;
}
