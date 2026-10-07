#pragma once
#include <QString>
#include <expected>

namespace SessionTokenCipher {
enum class Protection { PlatformDefault, SystemCredentials };
inline const QString prefix = QStringLiteral("dpapi:v1:");
inline const QString credentialPrefix = QStringLiteral("keychain:v1:");
bool isProtected(const QString& stored);
bool isCredentialReference(const QString& stored);
std::expected<QString, QString> protect(const QString& token, Protection protection = Protection::PlatformDefault);
std::expected<QString, QString> unprotect(const QString& stored);
std::expected<void, QString> discard(const QString& stored);
}
