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
std::expected<QByteArray, QString> decryptAuthenticatedData(QByteArrayView encryptedData,
                                                            QByteArrayView key,
                                                            QByteArrayView authenticatedData);
std::expected<QByteArray, QString> encryptTsSegment(QByteArrayView plaintext,
                                                    QByteArrayView key,
                                                    QByteArrayView iv);
std::expected<QByteArray, QString> encryptAuthenticatedData(QByteArrayView plaintext,
                                                            QByteArrayView key,
                                                            QByteArrayView iv,
                                                            QByteArrayView authenticatedData);
std::expected<EncryptedTsSegment, QString> encryptTsSegment(QByteArrayView plaintext);
std::expected<EncryptedTsSegment, QString> encryptAuthenticatedData(
    QByteArrayView plaintext,
    QByteArrayView authenticatedData);
std::expected<QByteArray, QString> secureRandomBytes(qsizetype size);

}
