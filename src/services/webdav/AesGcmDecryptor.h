#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <expected>

namespace AesGcmDecryptor {

std::expected<QByteArray, QString> decryptTsSegment(QByteArrayView encryptedSegment,
                                                    QByteArrayView key);

}
