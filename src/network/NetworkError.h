#pragma once

#include <QString>

enum class NetworkErrorKind {
    InvalidUrl,
    Transport,
    Timeout,
    Ssl,
    Http,
    Parse,
    CertificateRejected
};

struct NetworkError {
    NetworkErrorKind kind { NetworkErrorKind::Transport };
    QString message;
    int httpStatus { 0 };
};
