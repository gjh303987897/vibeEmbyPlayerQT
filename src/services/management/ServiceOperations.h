#pragma once

#include "database/SessionRepository.h"

namespace ServiceOperations {
QString cardId(const QString& endpoint, ServiceType type, const QString& username);
QString iptvId(const QString& source);
std::expected<void, QString> validate(const ServerConfig& server);
std::expected<void, QString> save(SessionRepository& repository, const ServerConfig& server);
std::expected<void, QString> savePlaylist(SessionRepository& repository, const ServerConfig& server,
                                        const QString& source, const QString& storageRoot = {}, const QUrl& origin = {});
std::expected<void, QString> remove(SessionRepository& repository, const QString& id,
                                   bool deleteLocalData = false, bool deleteCredentials = true);
bool verifyPrivacyPin(const SessionRepository& repository, const QString& pin);
}
