#pragma once

#include <QString>

struct IptvChannel {
    QString id;
    QString playlistId;
    QString name;
    QString groupName;
    QString logoUrl;
    QString streamUrl;
    int sortOrder { 0 };
};
