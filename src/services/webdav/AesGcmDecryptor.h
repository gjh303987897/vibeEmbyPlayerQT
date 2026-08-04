#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <expected>

namespace AesGcmDecryptor {

struct EncryptedTsSegment final {
    QByteArray bytes;
    QByteArray key;
};

std::expected<QByteArray, QString> decryptTsSegment(QByteArrayView encryptedSegment,
                                                    QByteArrayView key);
std::expected<QByteArray, QString> encryptTsSegment(QByteArrayView plaintext,
                                                    QByteArrayView key,
                                                    QByteArrayView iv);
std::expected<EncryptedTsSegment, QString> encryptTsSegment(QByteArrayView plaintext);
std::expected<QByteArray, QString> secureRandomBytes(qsizetype size);

}
