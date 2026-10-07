#pragma once
#include <QString>
#include <QUrl>
#include <atomic>
#include <expected>

struct S3Credentials { QString accessKey; QString secretKey; QString sessionToken; };
namespace S3CredentialProvider {
// Explicit providers avoid surprising metadata requests on ordinary desktops.
std::expected<S3Credentials,QString> load(const QString& provider, const QString& profile = {},
                                        std::atomic_bool* cancel = nullptr);
std::expected<S3Credentials,QString> assumeRole(const S3Credentials& source, const QString& roleArn,
    const QString& region, const QString& sessionName = "vibePlayerQT-cli", const QString& externalId = {},
    std::atomic_bool* cancel = nullptr, QUrl endpoint = {});
}
