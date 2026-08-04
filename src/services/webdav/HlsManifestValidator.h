#pragma once

#include <QByteArrayView>
#include <QString>

#include <expected>

namespace HlsManifestValidator {

inline constexpr qsizetype m3u8sIdentifierLength = 4096;

std::expected<void, QString> validate(QByteArrayView manifest,
                                      const QString& manifestPath = {});
std::expected<QByteArray, QString> extractM3u8sIdentifier(QByteArrayView manifest);
std::expected<QByteArray, QString> insertM3u8sIdentifier(QByteArrayView manifest,
                                                        QByteArrayView identifier);

}
