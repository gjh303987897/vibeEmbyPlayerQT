#include "database/SessionTokenCipher.h"
#include "services/credentials/CredentialStore.h"
#include <QByteArray>
#include <QUuid>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dpapi.h>
#endif

namespace SessionTokenCipher {
bool isProtected(const QString& stored)
{
    return stored.startsWith(prefix) || stored.startsWith(credentialPrefix);
}

bool isCredentialReference(const QString& stored)
{
    if (!stored.startsWith(credentialPrefix)) return false;
    const auto id = stored.mid(credentialPrefix.size());
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
}

std::expected<QString, QString> protect(const QString& token, Protection protection)
{
#ifdef Q_OS_WIN
    if (protection == Protection::PlatformDefault) {
        auto bytes = token.toUtf8();
        DATA_BLOB input {static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
        DATA_BLOB output {};
        const auto ok = CryptProtectData(&input, L"vibePlayerQT session", nullptr, nullptr, nullptr,
                                        CRYPTPROTECT_UI_FORBIDDEN, &output);
        bytes.fill('\0');
        if (!ok) return std::unexpected(QStringLiteral("Unable to protect the saved session with Windows DPAPI"));
        const auto encrypted = QByteArray(reinterpret_cast<const char*>(output.pbData), output.cbData).toBase64();
        LocalFree(output.pbData);
        return prefix + QString::fromLatin1(encrypted);
    }
#else
    Q_UNUSED(protection)
#endif
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto result = CredentialStore::saveSecret(QStringLiteral("Session/") + id, token);
    if (!result) return std::unexpected(result.error());
    return credentialPrefix + id;
}

std::expected<QString, QString> unprotect(const QString& stored)
{
    if (stored.startsWith(credentialPrefix)) {
        if (!isCredentialReference(stored))
            return std::unexpected(QStringLiteral("Saved session reference is invalid; log in again"));
        const auto result = CredentialStore::loadSecret(QStringLiteral("Session/") + stored.mid(credentialPrefix.size()));
        if (!result) return std::unexpected(result.error());
        if (!result->has_value())
            return std::unexpected(QStringLiteral("Saved session is missing from the system credential store; log in again"));
        return **result;
    }
    if (!stored.startsWith(prefix)) return stored; // Legacy read-only compatibility.
#ifdef Q_OS_WIN
    auto decoded = QByteArray::fromBase64Encoding(stored.mid(prefix.size()).toLatin1(),
                                                 QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) return std::unexpected(QStringLiteral("Saved session data is invalid; log in again"));
    DATA_BLOB input {static_cast<DWORD>(decoded.decoded.size()), reinterpret_cast<BYTE*>(decoded.decoded.data())};
    DATA_BLOB output {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return std::unexpected(QStringLiteral("Saved session cannot be unlocked by this Windows account; log in again"));
    const auto token = QString::fromUtf8(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return token;
#else
    return std::unexpected(QStringLiteral("Windows protected session requires its original Windows account"));
#endif
}

std::expected<void, QString> discard(const QString& stored)
{
    if (!stored.startsWith(credentialPrefix)) return {};
    if (!isCredentialReference(stored)) return std::unexpected(QStringLiteral("Invalid session credential reference"));
    return CredentialStore::deleteSecret(QStringLiteral("Session/") + stored.mid(credentialPrefix.size()));
}
}
