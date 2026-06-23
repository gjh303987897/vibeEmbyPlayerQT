#pragma once

#include "models/ServerConfig.h"

#include <QString>

struct ServiceCard {
    ServerConfig server;
    bool hasSession { false };
    QString lastUsedAt;
};
