#pragma once

#include <QList>
#include <QNetworkReply>
#include <QSslError>
#include <algorithm>

namespace SelfSignedTlsPolicy {
// A private CA or incomplete chain can report issuer/trust errors rather than
// SelfSignedCertificate. Only waive chain trust for explicitly opted-in servers;
// hostname, expiry, revocation, signature and missing-certificate checks remain.
// Error objects retain their certificates for per-reply exceptions.
inline bool allows(const QList<QSslError>& errors, bool enabled)
{
    return enabled && !errors.isEmpty() && std::ranges::all_of(errors, [](const QSslError& error) {
        switch (error.error()) {
        case QSslError::SelfSignedCertificate:
        case QSslError::SelfSignedCertificateInChain:
        case QSslError::UnableToGetIssuerCertificate:
        case QSslError::UnableToGetLocalIssuerCertificate:
        case QSslError::UnableToVerifyFirstCertificate:
        case QSslError::CertificateUntrusted:
            return true;
        default:
            return false;
        }
    });
}

inline void apply(QNetworkReply* reply, const QList<QSslError>& errors, bool enabled)
{
    if (allows(errors, enabled)) reply->ignoreSslErrors(errors);
}
}
