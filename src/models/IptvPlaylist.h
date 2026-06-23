#pragma once

#include <QString>

struct IptvPlaylist {
    QString id;
    QString serviceId;
    QString name;
    QString sourceType;
    QString sourcePath;
    QString importedPath;
    QString importedAt;
};
