#pragma once

#include <QString>

enum class NetworkErrorKind {
    InvalidUrl,
    Transport,
    Timeout,
    Canceled,
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
