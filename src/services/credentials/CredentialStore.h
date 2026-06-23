#pragma once

#include <QString>

#include <expected>
#include <optional>

class CredentialStore final {
public:
    static bool isAvailable();
    static std::expected<void, QString> savePassword(const QString& key, const QString& username, const QString& password);
    static std::expected<std::optional<QString>, QString> loadPassword(const QString& key);
    static std::expected<void, QString> deletePassword(const QString& key);
};
