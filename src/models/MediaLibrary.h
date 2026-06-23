#pragma once

#include <QString>

struct MediaLibrary {
    QString id;
    QString name;
    QString collectionType;
    QString itemType;
    QString imageTag;
    QString imageUrl;
    int childCount { 0 };
};
