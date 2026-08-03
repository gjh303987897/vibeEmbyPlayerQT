#pragma once

#include <QByteArrayView>
#include <QString>

#include <expected>

namespace HlsManifestValidator {

std::expected<void, QString> validate(QByteArrayView manifest,
                                      const QString& manifestPath = {});

}
