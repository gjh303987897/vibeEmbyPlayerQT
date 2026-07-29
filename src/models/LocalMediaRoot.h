#pragma once

#include <QString>

struct LocalMediaRoot {
    QString id;
    QString name;
    QString path;
    int sortOrder { 0 };
    bool available { false };
};
