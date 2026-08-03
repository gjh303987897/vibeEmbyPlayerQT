#include "services/webdav/AesGcmDecryptor.h"

#include <QLibrary>
#include <QtGlobal>

#include <limits>
#include <memory>

namespace AesGcmDecryptor {
namespace {
struct EvpCipherContext;
struct EvpCipher;

class OpenSslEvp final {
public:
    using NewContext = EvpCipherContext* (*)();
    using FreeContext = void (*)(EvpCipherContext*);
    using Aes256Gcm = const EvpCipher* (*)();
    using DecryptInit = int (*)(EvpCipherContext*, const EvpCipher*, void*, const unsigned char*, const unsigned char*);
    using ContextControl = int (*)(EvpCipherContext*, int, int, void*);
    using DecryptUpdate = int (*)(EvpCipherContext*, unsigned char*, int*, const unsigned char*, int);
    using DecryptFinal = int (*)(EvpCipherContext*, unsigned char*, int*);

    OpenSslEvp()
    {
#if defined(Q_OS_WIN)
        const QStringList candidates {
            QStringLiteral("libcrypto-3-x64"),
            QStringLiteral("libcrypto-3"),
            QStringLiteral("libcrypto-1_1-x64"),
            QStringLiteral("libcrypto"),
        };
#elif defined(Q_OS_MACOS)
        const QStringList candidates {
            QStringLiteral("libcrypto.3.dylib"),
            QStringLiteral("libcrypto.1.1.dylib"),
            QStringLiteral("crypto"),
        };
#else
        const QStringList candidates {
            QStringLiteral("libcrypto.so.3"),
            QStringLiteral("libcrypto.so.1.1"),
            QStringLiteral("crypto"),
        };
#endif
        for (const auto& candidate : candidates) {
            m_library.setFileName(candidate);
            if (m_library.load() && resolveFunctions()) {
                return;
            }
            m_library.unload();
        }
    }

    bool available() const
    {
        return m_library.isLoaded() && newContext && freeContext && aes256Gcm && decryptInit &&
               contextControl && decryptUpdate && decryptFinal;
    }

    NewContext newContext { nullptr };
    FreeContext freeContext { nullptr };
    Aes256Gcm aes256Gcm { nullptr };
    DecryptInit decryptInit { nullptr };
    ContextControl contextControl { nullptr };
    DecryptUpdate decryptUpdate { nullptr };
    DecryptFinal decryptFinal { nullptr };

private:
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
        return available();
    }

    QLibrary m_library;
};

constexpr qsizetype ivBytes = 16;
constexpr qsizetype tagBytes = 16;
// These are the stable public EVP control values used by OpenSSL's GCM API.
constexpr int setIvLengthControl = 0x9;
constexpr int setTagControl = 0x11;
}

std::expected<QByteArray, QString> decryptTsSegment(QByteArrayView encryptedSegment, QByteArrayView key)
{
    if (key.size() != 32) {
        return std::unexpected(QStringLiteral("AES-256-GCM requires a 32-byte key"));
    }
    if (encryptedSegment.size() <= ivBytes + tagBytes) {
        return std::unexpected(QStringLiteral("Encrypted TS segment is too short"));
    }
    const auto ciphertextBytes = encryptedSegment.size() - ivBytes - tagBytes;
    if (ciphertextBytes > std::numeric_limits<int>::max()) {
        return std::unexpected(QStringLiteral("Encrypted TS segment exceeds the EVP size limit"));
    }

    static OpenSslEvp api;
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

}
