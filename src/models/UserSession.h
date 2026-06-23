#pragma once

#include "models/ServerConfig.h"

#include <QDateTime>
#include <QString>

struct UserSession {
    ServerConfig server;
    QString userId;
    QString username;
    QString accessToken;
    QDateTime createdAt;
};
