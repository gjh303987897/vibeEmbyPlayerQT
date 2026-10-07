#pragma once

#include <QList>
#include <QNetworkReply>
#include <QSslError>
#include <algorithm>

namespace SelfSignedTlsPolicy {
// Trusting a self-signed issuer does not waive hostname, expiry, revocation,
// signature, or missing-certificate checks. Error objects retain their certs.
inline bool allows(const QList<QSslError>& errors, bool enabled)
{
    return enabled && !errors.isEmpty() && std::ranges::all_of(errors, [](const QSslError& error) {
        return error.error() == QSslError::SelfSignedCertificate ||
               error.error() == QSslError::SelfSignedCertificateInChain;
    });
}

inline void apply(QNetworkReply* reply, const QList<QSslError>& errors, bool enabled)
{
    if (allows(errors, enabled)) reply->ignoreSslErrors(errors);
}
}
