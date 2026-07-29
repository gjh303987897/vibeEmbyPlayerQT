#pragma once

#include <QDateTime>
#include <QString>

struct LocalMediaItem {
    QString name;
    QString path;
    QDateTime lastModified;
    qint64 size { 0 };
    bool directory { false };
};
