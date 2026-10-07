#pragma once
#include <QString>
#include <QByteArray>
#include <expected>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dpapi.h>
#endif

namespace SessionTokenCipher {
inline const QString prefix = QStringLiteral("dpapi:v1:");

inline std::expected<QString, QString> protect(const QString& token)
{
#ifdef Q_OS_WIN
    auto bytes = token.toUtf8();
    DATA_BLOB input { static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data()) };
    DATA_BLOB output {};
    const auto ok = CryptProtectData(&input, L"vibePlayerQT session", nullptr, nullptr, nullptr,
                                    CRYPTPROTECT_UI_FORBIDDEN, &output);
    bytes.fill('\0');
    if (!ok) return std::unexpected(QStringLiteral("Unable to protect the saved session with Windows DPAPI"));
    const auto encrypted = QByteArray(reinterpret_cast<const char*>(output.pbData), output.cbData).toBase64();
    LocalFree(output.pbData);
    return prefix + QString::fromLatin1(encrypted);
#else
    // Existing non-Windows storage remains compatible until a native keychain
    // backend is available. Windows encrypted sessions cannot be imported here.
    return token;
#endif
}

inline std::expected<QString, QString> unprotect(const QString& stored)
{
    if (!stored.startsWith(prefix)) return stored; // Legacy read-only compatibility.
#ifdef Q_OS_WIN
    auto decoded = QByteArray::fromBase64Encoding(stored.mid(prefix.size()).toLatin1(),
                                                 QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) return std::unexpected(QStringLiteral("Saved session data is invalid; log in again"));
    DATA_BLOB input { static_cast<DWORD>(decoded.decoded.size()), reinterpret_cast<BYTE*>(decoded.decoded.data()) };
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
}
