#include "services/credentials/CredentialStore.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <wincred.h>
#endif

#include <QByteArray>

namespace {
QString targetName(const QString& key)
{
    return QStringLiteral("vibePlayerQT/WebDAV/%1").arg(key);
}

QString unavailableMessage()
{
    return QStringLiteral("System credential store is not available on this platform");
}
}

bool CredentialStore::isAvailable()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

std::expected<void, QString> CredentialStore::savePassword(const QString& key,
                                                           const QString& username,
                                                           const QString& password)
{
#ifdef Q_OS_WIN
    const auto target = targetName(key).toStdWString();
    const auto user = username.toStdWString();
    const auto secret = password.toUtf8();

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
    Q_UNUSED(key)
    Q_UNUSED(username)
    Q_UNUSED(password)
    return std::unexpected(unavailableMessage());
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

    const QByteArray secret(reinterpret_cast<const char*>(credential->CredentialBlob),
                            static_cast<int>(credential->CredentialBlobSize));
    const auto password = QString::fromUtf8(secret);
    CredFree(credential);
    return std::optional<QString> { password };
#else
    Q_UNUSED(key)
    return std::unexpected(unavailableMessage());
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
    Q_UNUSED(key)
    return std::unexpected(unavailableMessage());
#endif
}
