#pragma once
#include "models/ServerConfig.h"
#include <QString>
#include <expected>

namespace SmbClient {
// Windows delegates authentication and SMB negotiation to the native redirector.
std::expected<void,QString> connect(const ServerConfig& server, const QString& password);
std::expected<QString,QString> path(const ServerConfig& server, const QString& virtualPath);
bool hasLinkedAncestor(const QString& path);
std::expected<void,QString> move(const QString& source, const QString& destination, bool overwrite);
}
