#include "services/webdav/AesGcmDecryptor.h"

#include <QtGlobal>

#if defined(Q_OS_MACOS)
#include <openssl/evp.h>
#include <openssl/rand.h>
#else
#include <QLibrary>
#include <QStringList>
#endif

#include <algorithm>
#include <limits>
#include <memory>

namespace AesGcmDecryptor {
namespace {
#if defined(Q_OS_MACOS)
using EvpCipherContext = EVP_CIPHER_CTX;
using EvpCipher = EVP_CIPHER;
using EvpEngine = ENGINE;
#else
struct EvpCipherContext;
struct EvpCipher;
struct EvpEngine;
#endif

class OpenSslEvp final {
public:
    using NewContext = EvpCipherContext* (*)();
    using FreeContext = void (*)(EvpCipherContext*);
    using Aes256Gcm = const EvpCipher* (*)();
    using DecryptInit = int (*)(EvpCipherContext*, const EvpCipher*, EvpEngine*, const unsigned char*, const unsigned char*);
    using ContextControl = int (*)(EvpCipherContext*, int, int, void*);
    using DecryptUpdate = int (*)(EvpCipherContext*, unsigned char*, int*, const unsigned char*, int);
    using DecryptFinal = int (*)(EvpCipherContext*, unsigned char*, int*);
    using EncryptInit = int (*)(EvpCipherContext*, const EvpCipher*, EvpEngine*, const unsigned char*, const unsigned char*);
    using EncryptUpdate = int (*)(EvpCipherContext*, unsigned char*, int*, const unsigned char*, int);
    using EncryptFinal = int (*)(EvpCipherContext*, unsigned char*, int*);
    using RandomBytes = int (*)(unsigned char*, int);

    OpenSslEvp()
    {
#if defined(Q_OS_MACOS)
        newContext = &EVP_CIPHER_CTX_new;
        freeContext = &EVP_CIPHER_CTX_free;
        aes256Gcm = &EVP_aes_256_gcm;
        decryptInit = &EVP_DecryptInit_ex;
        contextControl = &EVP_CIPHER_CTX_ctrl;
        decryptUpdate = &EVP_DecryptUpdate;
        decryptFinal = &EVP_DecryptFinal_ex;
        encryptInit = &EVP_EncryptInit_ex;
        encryptUpdate = &EVP_EncryptUpdate;
        encryptFinal = &EVP_EncryptFinal_ex;
        randomBytes = &RAND_bytes;
#elif defined(Q_OS_WIN)
        const QStringList candidates {
            QStringLiteral("libcrypto-3-x64"),
            QStringLiteral("libcrypto-3"),
            QStringLiteral("libcrypto-1_1-x64"),
            QStringLiteral("libcrypto"),
        };
#else
        const QStringList candidates {
            QStringLiteral("libcrypto.so.3"),
            QStringLiteral("libcrypto.so.1.1"),
            QStringLiteral("crypto"),
        };
#endif
#if !defined(Q_OS_MACOS)
        for (const auto& candidate : candidates) {
            m_library.setFileName(candidate);
            if (m_library.load() && resolveFunctions()) {
                return;
            }
            m_library.unload();
        }
#endif
    }

    bool available() const
    {
        const auto functionsAvailable = newContext && freeContext && aes256Gcm && decryptInit &&
                                        contextControl && decryptUpdate && decryptFinal && encryptInit &&
                                        encryptUpdate && encryptFinal && randomBytes;
#if defined(Q_OS_MACOS)
        return functionsAvailable;
#else
        return m_library.isLoaded() && functionsAvailable;
#endif
    }

    NewContext newContext { nullptr };
    FreeContext freeContext { nullptr };
    Aes256Gcm aes256Gcm { nullptr };
    DecryptInit decryptInit { nullptr };
    ContextControl contextControl { nullptr };
    DecryptUpdate decryptUpdate { nullptr };
    DecryptFinal decryptFinal { nullptr };
    EncryptInit encryptInit { nullptr };
    EncryptUpdate encryptUpdate { nullptr };
    EncryptFinal encryptFinal { nullptr };
    RandomBytes randomBytes { nullptr };

private:
#if !defined(Q_OS_MACOS)
    template<typename Function>
    Function resolve(const char* name)
    {
        return reinterpret_cast<Function>(m_library.resolve(name));
    }

    bool resolveFunctions()
    {
        newContext = resolve<NewContext>("EVP_CIPHER_CTX_new");
        freeContext = resolve<FreeContext>("EVP_CIPHER_CTX_free");
        aes256Gcm = resolve<Aes256Gcm>("EVP_aes_256_gcm");
        decryptInit = resolve<DecryptInit>("EVP_DecryptInit_ex");
        contextControl = resolve<ContextControl>("EVP_CIPHER_CTX_ctrl");
        decryptUpdate = resolve<DecryptUpdate>("EVP_DecryptUpdate");
        decryptFinal = resolve<DecryptFinal>("EVP_DecryptFinal_ex");
        encryptInit = resolve<EncryptInit>("EVP_EncryptInit_ex");
        encryptUpdate = resolve<EncryptUpdate>("EVP_EncryptUpdate");
        encryptFinal = resolve<EncryptFinal>("EVP_EncryptFinal_ex");
        randomBytes = resolve<RandomBytes>("RAND_bytes");
        return available();
    }

    QLibrary m_library;
#endif
};

constexpr qsizetype ivBytes = 16;
constexpr qsizetype tagBytes = 16;
// These are the stable public EVP control values used by OpenSSL's GCM API.
constexpr int setIvLengthControl = 0x9;
constexpr int getTagControl = 0x10;
constexpr int setTagControl = 0x11;

OpenSslEvp& openssl()
{
    static OpenSslEvp api;
    return api;
}
}

std::expected<QByteArray, QString> decryptAuthenticatedData(QByteArrayView encryptedSegment,
                                                            QByteArrayView key,
                                                            QByteArrayView authenticatedData)
{
    if (key.size() != 32) {
        return std::unexpected(QStringLiteral("AES-256-GCM requires a 32-byte key"));
    }
    if (encryptedSegment.size() <= ivBytes + tagBytes) {
        return std::unexpected(QStringLiteral("Encrypted TS segment is too short"));
    }
    const auto ciphertextBytes = encryptedSegment.size() - ivBytes - tagBytes;
    if (ciphertextBytes > std::numeric_limits<int>::max() ||
        authenticatedData.size() > std::numeric_limits<int>::max()) {
        return std::unexpected(QStringLiteral("Encrypted TS segment exceeds the EVP size limit"));
    }

    auto& api = openssl();
    if (!api.available()) {
        return std::unexpected(QStringLiteral("OpenSSL EVP AES-256-GCM support is unavailable"));
    }

    std::unique_ptr<EvpCipherContext, OpenSslEvp::FreeContext> context(api.newContext(), api.freeContext);
    if (!context) {
        return std::unexpected(QStringLiteral("Unable to allocate an OpenSSL cipher context"));
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(encryptedSegment.data());
    const auto* keyBytes = reinterpret_cast<const unsigned char*>(key.data());
    const auto* iv = bytes;
    const auto* ciphertext = bytes + ivBytes;
    auto* tag = const_cast<unsigned char*>(bytes + ivBytes + ciphertextBytes);

    if (api.decryptInit(context.get(), api.aes256Gcm(), nullptr, nullptr, nullptr) != 1 ||
        api.contextControl(context.get(), setIvLengthControl, static_cast<int>(ivBytes), nullptr) != 1 ||
        api.decryptInit(context.get(), nullptr, nullptr, keyBytes, iv) != 1) {
        return std::unexpected(QStringLiteral("Unable to initialize AES-256-GCM decryption"));
    }

    int authenticatedBytes = 0;
    if (!authenticatedData.isEmpty() &&
        api.decryptUpdate(context.get(),
                          nullptr,
                          &authenticatedBytes,
                          reinterpret_cast<const unsigned char*>(authenticatedData.data()),
                          static_cast<int>(authenticatedData.size())) != 1) {
        return std::unexpected(QStringLiteral("AES-256-GCM authenticated data setup failed"));
    }

    QByteArray plaintext(ciphertextBytes + tagBytes, Qt::Uninitialized);
    auto* output = reinterpret_cast<unsigned char*>(plaintext.data());
    int produced = 0;
    if (api.decryptUpdate(context.get(),
                          output,
                          &produced,
                          ciphertext,
                          static_cast<int>(ciphertextBytes)) != 1 ||
        api.contextControl(context.get(), setTagControl, static_cast<int>(tagBytes), tag) != 1) {
        plaintext.fill('\0');
        return std::unexpected(QStringLiteral("AES-256-GCM segment decryption failed"));
    }

    int finalBytes = 0;
    if (api.decryptFinal(context.get(), output + produced, &finalBytes) != 1) {
        plaintext.fill('\0');
        return std::unexpected(QStringLiteral("AES-256-GCM authentication tag verification failed"));
    }
    plaintext.resize(produced + finalBytes);
    return plaintext;
}

std::expected<QByteArray, QString> decryptTsSegment(QByteArrayView encryptedSegment, QByteArrayView key)
{
    return decryptAuthenticatedData(encryptedSegment, key, {});
}

std::expected<QByteArray, QString> secureRandomBytes(qsizetype size)
{
    if (size <= 0 || size > std::numeric_limits<int>::max()) {
        return std::unexpected(QStringLiteral("Secure random byte count is outside the supported range"));
    }
    auto& api = openssl();
    if (!api.available()) {
        return std::unexpected(QStringLiteral("OpenSSL secure random support is unavailable"));
    }

    QByteArray bytes(size, Qt::Uninitialized);
    if (api.randomBytes(reinterpret_cast<unsigned char*>(bytes.data()), static_cast<int>(size)) != 1) {
        bytes.fill('\0');
        return std::unexpected(QStringLiteral("OpenSSL failed to generate secure random bytes"));
    }
    return bytes;
}

std::expected<QByteArray, QString> encryptAuthenticatedData(QByteArrayView plaintext,
                                                            QByteArrayView key,
                                                            QByteArrayView iv,
                                                            QByteArrayView authenticatedData)
{
    if (key.size() != 32) {
        return std::unexpected(QStringLiteral("AES-256-GCM requires a 32-byte key"));
    }
    if (iv.size() != ivBytes) {
        return std::unexpected(QStringLiteral("AES-256-GCM TS segments require a 16-byte IV"));
    }
    if (plaintext.isEmpty() || plaintext.size() > std::numeric_limits<int>::max() ||
        authenticatedData.size() > std::numeric_limits<int>::max()) {
        return std::unexpected(QStringLiteral("Plaintext TS segment is empty or exceeds the EVP size limit"));
    }

    auto& api = openssl();
    if (!api.available()) {
        return std::unexpected(QStringLiteral("OpenSSL EVP AES-256-GCM support is unavailable"));
    }
    std::unique_ptr<EvpCipherContext, OpenSslEvp::FreeContext> context(api.newContext(), api.freeContext);
    if (!context) {
        return std::unexpected(QStringLiteral("Unable to allocate an OpenSSL cipher context"));
    }

    const auto* keyBytes = reinterpret_cast<const unsigned char*>(key.data());
    const auto* ivBytesPointer = reinterpret_cast<const unsigned char*>(iv.data());
    if (api.encryptInit(context.get(), api.aes256Gcm(), nullptr, nullptr, nullptr) != 1 ||
        api.contextControl(context.get(), setIvLengthControl, static_cast<int>(iv.size()), nullptr) != 1 ||
        api.encryptInit(context.get(), nullptr, nullptr, keyBytes, ivBytesPointer) != 1) {
        return std::unexpected(QStringLiteral("Unable to initialize AES-256-GCM encryption"));
    }

    int authenticatedBytes = 0;
    if (!authenticatedData.isEmpty() &&
        api.encryptUpdate(context.get(),
                          nullptr,
                          &authenticatedBytes,
                          reinterpret_cast<const unsigned char*>(authenticatedData.data()),
                          static_cast<int>(authenticatedData.size())) != 1) {
        return std::unexpected(QStringLiteral("AES-256-GCM authenticated data setup failed"));
    }

    QByteArray encrypted(iv.size() + plaintext.size() + tagBytes, Qt::Uninitialized);
    std::copy(iv.begin(), iv.end(), encrypted.begin());
    auto* output = reinterpret_cast<unsigned char*>(encrypted.data() + iv.size());
    int produced = 0;
    if (api.encryptUpdate(context.get(),
                          output,
                          &produced,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        encrypted.fill('\0');
        return std::unexpected(QStringLiteral("AES-256-GCM segment encryption failed"));
    }
    int finalBytes = 0;
    if (api.encryptFinal(context.get(), output + produced, &finalBytes) != 1 ||
        produced + finalBytes != plaintext.size()) {
        encrypted.fill('\0');
        return std::unexpected(QStringLiteral("AES-256-GCM segment encryption finalization failed"));
    }
    auto* tag = encrypted.data() + iv.size() + plaintext.size();
    if (api.contextControl(context.get(), getTagControl, static_cast<int>(tagBytes), tag) != 1) {
        encrypted.fill('\0');
        return std::unexpected(QStringLiteral("Unable to read the AES-256-GCM authentication tag"));
    }
    return encrypted;
}

std::expected<QByteArray, QString> encryptTsSegment(QByteArrayView plaintext,
                                                    QByteArrayView key,
                                                    QByteArrayView iv)
{
    return encryptAuthenticatedData(plaintext, key, iv, {});
}

std::expected<EncryptedTsSegment, QString> encryptAuthenticatedData(
    QByteArrayView plaintext,
    QByteArrayView authenticatedData)
{
    auto key = secureRandomBytes(32);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto iv = secureRandomBytes(ivBytes);
    if (!iv) {
        key->fill('\0');
        return std::unexpected(iv.error());
    }
    auto encrypted = encryptAuthenticatedData(plaintext, *key, *iv, authenticatedData);
    iv->fill('\0');
    if (!encrypted) {
        key->fill('\0');
        return std::unexpected(encrypted.error());
    }
    return EncryptedTsSegment {
        .bytes = std::move(*encrypted),
        .key = std::move(*key),
    };
}

std::expected<EncryptedTsSegment, QString> encryptTsSegment(QByteArrayView plaintext)
{
    return encryptAuthenticatedData(plaintext, {});
}

}
