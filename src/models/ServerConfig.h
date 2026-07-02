#pragma once

#include "models/ServiceType.h"

#include <QString>

struct ServerConfig {
    QString id;
    QString name;
    QString baseUrl;
    QString username;
    ServiceType serviceType { ServiceType::Emby };
    bool trustSelfSignedCertificate { false };
    bool autoLogin { true };
    bool privateMode { false };
};
