#include "network/TlsCertificateStore.h"

#include <QDir>
#include <QSaveFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QStandardPaths>

#include <expected>

namespace TlsCertificateStore {
namespace {
std::expected<QString, QString> createSystemCaBundle()
{
    const auto certificates = QSslConfiguration::systemCaCertificates();
    if (certificates.isEmpty()) {
        return std::unexpected(QStringLiteral("The operating system did not provide any trusted CA certificates"));
    }

    QByteArray pemBundle;
    for (const auto& certificate : certificates) {
        const auto pem = certificate.toPem();
        if (pem.isEmpty()) {
            continue;
        }
        pemBundle.append(pem);
        if (!pemBundle.endsWith('\n')) {
            pemBundle.append('\n');
        }
    }
    if (pemBundle.isEmpty()) {
        return std::unexpected(QStringLiteral("The operating system CA certificates could not be encoded as PEM"));
    }

    const auto cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty()) {
        return std::unexpected(QStringLiteral("The application cache directory is unavailable"));
    }

    const auto certificateDirectory = QDir(cacheRoot).filePath(QStringLiteral("tls"));
    if (!QDir().mkpath(certificateDirectory)) {
        return std::unexpected(QStringLiteral("The TLS certificate cache directory could not be created"));
    }

    const auto bundlePath = QDir(certificateDirectory).filePath(QStringLiteral("system-ca-bundle.pem"));
    QSaveFile bundleFile(bundlePath);
    if (!bundleFile.open(QIODevice::WriteOnly)) {
        return std::unexpected(QStringLiteral("The system CA bundle could not be opened for writing: %1")
                                   .arg(bundleFile.errorString()));
    }
    if (bundleFile.write(pemBundle) != pemBundle.size()) {
        return std::unexpected(QStringLiteral("The system CA bundle could not be written: %1")
                                   .arg(bundleFile.errorString()));
    }
    if (!bundleFile.commit()) {
        return std::unexpected(QStringLiteral("The system CA bundle could not be committed: %1")
                                   .arg(bundleFile.errorString()));
    }

    return bundlePath;
}
}

std::expected<QString, QString> ensureSystemCaBundle()
{
    static const auto result = createSystemCaBundle();
    return result;
}

}
