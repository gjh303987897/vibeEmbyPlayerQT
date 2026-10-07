#pragma once
#include "services/webdav/TransferManager.h"
#include <atomic>
#include <expected>

namespace WebDavUploadPlanner {
using Requests = std::vector<TransferManager::TaskRequest>;
std::expected<Requests, QString> plan(const QString& source, const QUrl& target,
                                     const std::atomic_bool& canceled);
}
