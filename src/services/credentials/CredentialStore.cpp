#include "services/credentials/CredentialStore.h"
#include "services/credentials/NativeCredentialStore.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <wincred.h>
#endif

#include <QByteArray>
#include <QScopeGuard>

namespace {
QString targetName(const QString& key)
{
    return QStringLiteral("vibePlayerQT/WebDAV/%1").arg(key);
}

QString secretTargetName(const QString& key)
{
    return QStringLiteral("vibePlayerQT/Secret/%1").arg(key);
}

}

bool CredentialStore::isAvailable()
{
#ifdef Q_OS_WIN
    return true;
#elif defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    // Capability, not a guarantee that the user's keyring is currently unlocked.
    return true;
#else
    return false;
#endif
}

std::expected<void, QString> CredentialStore::savePassword(const QString& key,
                                                           const QString& username,
                                                           const QString& password)
{
    if (key.isEmpty() || key.contains(QChar::Null) || password.contains(QChar::Null))
        return std::unexpected(QStringLiteral("Invalid system credential input"));
#ifdef Q_OS_WIN
    const auto target = targetName(key).toStdWString();
    const auto user = username.toStdWString();
    auto secret = password.toUtf8();
    const auto wipe = qScopeGuard([&] { SecureZeroMemory(secret.data(), secret.size()); });

    CREDENTIALW credential {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.UserName = const_cast<LPWSTR>(user.c_str());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.constData()));

    if (!CredWriteW(&credential, 0)) {
        return std::unexpected(QStringLiteral("Unable to save password to Windows Credential Manager"));
    }
    return {};
#else
    Q_UNUSED(username)
    const auto result = NativeCredentialStore::execute(NativeCredentialStore::Operation::Write, targetName(key), password);
    if (!result) return std::unexpected(result.error());
    return {};
#endif
}

std::expected<std::optional<QString>, QString> CredentialStore::loadPassword(const QString& key)
{
#ifdef Q_OS_WIN
    const auto target = targetName(key).toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const auto error = GetLastError();
        if (error == ERROR_NOT_FOUND) {
            return std::optional<QString> {};
        }
        return std::unexpected(QStringLiteral("Unable to read password from Windows Credential Manager"));
    }

    const auto password = QString::fromUtf8(reinterpret_cast<const char*>(credential->CredentialBlob),
                                           credential->CredentialBlobSize);
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return std::optional<QString> { password };
#else
    return NativeCredentialStore::execute(NativeCredentialStore::Operation::Read, targetName(key));
#endif
}

std::expected<void, QString> CredentialStore::deletePassword(const QString& key)
{
#ifdef Q_OS_WIN
    const auto target = targetName(key).toStdWString();
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        const auto error = GetLastError();
        if (error != ERROR_NOT_FOUND) {
            return std::unexpected(QStringLiteral("Unable to delete password from Windows Credential Manager"));
        }
    }
    return {};
#else
    const auto result = NativeCredentialStore::execute(NativeCredentialStore::Operation::Remove, targetName(key));
    if (!result) return std::unexpected(result.error());
    return {};
#endif
}

std::expected<void, QString> CredentialStore::saveSecret(const QString& key, const QString& secret)
{
    if (key.isEmpty() || key.contains(QChar::Null) || secret.contains(QChar::Null))
        return std::unexpected(QStringLiteral("Invalid system credential input"));
#ifdef Q_OS_WIN
    const auto target = secretTargetName(key).toStdWString();
    auto bytes = secret.toUtf8();
    const auto wipe = qScopeGuard([&] { SecureZeroMemory(bytes.data(), bytes.size()); });
    CREDENTIALW credential {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(bytes.constData()));
    if (!CredWriteW(&credential, 0)) {
        return std::unexpected(QStringLiteral("Unable to save secret to Windows Credential Manager"));
    }
    return {};
#else
    const auto result = NativeCredentialStore::execute(NativeCredentialStore::Operation::Write, secretTargetName(key), secret);
    if (!result) return std::unexpected(result.error());
    return {};
#endif
}

std::expected<std::optional<QString>, QString> CredentialStore::loadSecret(const QString& key)
{
#ifdef Q_OS_WIN
    const auto target = secretTargetName(key).toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        if (GetLastError() == ERROR_NOT_FOUND) {
            return std::optional<QString> {};
        }
        return std::unexpected(QStringLiteral("Unable to read secret from Windows Credential Manager"));
    }
    const auto result = QString::fromUtf8(reinterpret_cast<const char*>(credential->CredentialBlob),
                                         credential->CredentialBlobSize);
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return std::optional<QString> { result };
#else
    return NativeCredentialStore::execute(NativeCredentialStore::Operation::Read, secretTargetName(key));
#endif
}

std::expected<void, QString> CredentialStore::deleteSecret(const QString& key)
{
#ifdef Q_OS_WIN
    const auto target = secretTargetName(key).toStdWString();
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
        return std::unexpected(QStringLiteral("Unable to delete secret from Windows Credential Manager"));
    }
    return {};
#else
    const auto result = NativeCredentialStore::execute(NativeCredentialStore::Operation::Remove, secretTargetName(key));
    if (!result) return std::unexpected(result.error());
    return {};
#endif
}
