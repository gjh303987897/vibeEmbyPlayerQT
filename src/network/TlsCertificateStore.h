#pragma once

#include <QString>

#include <expected>

namespace TlsCertificateStore {

std::expected<QString, QString> ensureSystemCaBundle();

}
