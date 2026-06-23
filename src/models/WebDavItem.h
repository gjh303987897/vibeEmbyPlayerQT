#pragma once

#include <QString>
#include <QUrl>

struct WebDavItem {
    QString name;
    QUrl url;
    QString relativePath;
    QString contentType;
    QString lastModified;
    qint64 size { -1 };
    bool directory { false };
    bool playable { false };
};
