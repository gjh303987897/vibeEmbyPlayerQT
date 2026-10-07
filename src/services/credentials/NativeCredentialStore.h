#pragma once

#include <QString>
#include <expected>
#include <optional>

// Platform APIs stay behind CredentialStore; callers never choose a plaintext fallback.
namespace NativeCredentialStore {
enum class Operation { Read, Write, Remove };
std::expected<std::optional<QString>, QString> execute(Operation operation, const QString& target,
                                                      const QString& secret = {});
}
