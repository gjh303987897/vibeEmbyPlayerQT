#pragma once
#include "services/encryptedhls/EncryptedHlsPackager.h"
#include <QVariantMap>
#include <expected>

namespace WorkflowSettings {
QVariantMap snapshot();
std::expected<void, QString> set(const QString& key, const QString& value);
std::expected<void, QString> validate(const QString& key, QVariant& value);
void applyPackaging(const QVariantMap& values, EncryptedHlsPackageRequest& request);
QString defaultOutputDirectory();
}
