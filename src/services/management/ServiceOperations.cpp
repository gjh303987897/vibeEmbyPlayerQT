#include "services/management/ServiceOperations.h"
#include "services/credentials/CredentialStore.h"
#include "services/iptv/IptvParser.h"
#include "services/iptv/IptvPlaylistStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QUrl>

namespace ServiceOperations {
QString cardId(const QString& endpoint, ServiceType type, const QString& username)
{
    return QString::fromLatin1(QCryptographicHash::hash(serviceTypeToString(type).toUtf8() + ':' +
        endpoint.trimmed().toUtf8() + ':' + username.trimmed().toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString iptvId(const QString& source)
{
    return QString::fromLatin1(QCryptographicHash::hash(QByteArray("IPTV:") + source.trimmed().toUtf8(),
                                                      QCryptographicHash::Sha256).toHex());
}

std::expected<void, QString> validate(const ServerConfig& server)
{
    if (server.id.isEmpty() || server.name.trimmed().isEmpty())
        return std::unexpected(QStringLiteral("Service ID and name are required"));
    if (server.serviceType == ServiceType::IPTV) return {};
    if (server.serviceType == ServiceType::SMB) {
        const QUrl url(server.baseUrl);
        if (url.scheme() != "smb" || url.host().isEmpty() || !url.userInfo().isEmpty() ||
            url.hasQuery() || url.hasFragment() || url.path().split('/',Qt::SkipEmptyParts).isEmpty())
            return std::unexpected(QStringLiteral("SMB requires smb://host/share/path without embedded credentials"));
        for (const auto& part : url.path().split('/',Qt::SkipEmptyParts))
            if (part == "." || part == ".." || part.contains('\\') || part.contains(':'))
                return std::unexpected(QStringLiteral("Invalid SMB root path"));
        return {};
    }
    if (server.serviceType != ServiceType::Emby && server.serviceType != ServiceType::Jellyfin &&
        server.serviceType != ServiceType::WebDAV)
        return std::unexpected(QStringLiteral("Unsupported service type"));
    const QUrl url(server.baseUrl);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) ||
        url.hasFragment())
        return std::unexpected(QStringLiteral("Enter an HTTP(S) service URL without embedded credentials or fragment"));
    if (server.username.trimmed().isEmpty()) return std::unexpected(QStringLiteral("Username is required"));
    return {};
}

std::expected<void, QString> save(SessionRepository& repository, const ServerConfig& server)
{
    if (auto result = validate(server); !result) return result;
    return repository.saveServer(server);
}

std::expected<void, QString> savePlaylist(SessionRepository& repository, const ServerConfig& server,
                                        const QString& source, const QString& storageRoot, const QUrl& origin)
{
    auto channels = IptvParser::parseFile(source);
    if (!channels) return std::unexpected(channels.error());
    auto path = IptvPlaylistStore::importFile(source, server.id, storageRoot);
    if (!path) return std::unexpected(path.error());
    auto stored = server;
    stored.baseUrl = *path;
    const auto playlistId = QStringLiteral("iptv-playlist-%1").arg(stored.id);
    for (auto& channel : *channels) {
        if (!origin.isEmpty()) {
            const auto stream = QUrl(channel.streamUrl);
            channel.streamUrl = stream.isLocalFile() ? origin.toString() : origin.resolved(stream).toString();
            channel.streamUrls = {channel.streamUrl};
            if (!channel.logoUrl.isEmpty()) channel.logoUrl = origin.resolved(QUrl(channel.logoUrl)).toString();
        } else if (channel.streamUrl == QUrl::fromLocalFile(QFileInfo(source).absoluteFilePath()).toString()) {
            channel.streamUrl = QUrl::fromLocalFile(*path).toString();
            channel.streamUrls = {channel.streamUrl};
        }
        if (channel.streamUrls.isEmpty()) channel.streamUrls = {channel.streamUrl};
        channel.playlistId = playlistId;
    }
    *channels = IptvParser::mergeChannels(std::move(*channels));
    for (auto& channel : *channels) channel.playlistId = playlistId;
    return repository.saveIptvPlaylist(stored, IptvPlaylist {
        .id = playlistId, .serviceId = stored.id,
        .name = stored.name, .sourceType = origin.isEmpty() ? QStringLiteral("LocalFile") : QStringLiteral("RemoteURL"),
        .sourcePath = origin.isEmpty() ? QFileInfo(source).absoluteFilePath() : origin.toString(), .importedPath = *path,
        .importedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate),
    }, *channels);
}

std::expected<void, QString> remove(SessionRepository& repository, const QString& id,
                                   bool deleteLocalData, bool deleteCredentials)
{
    if (deleteCredentials && CredentialStore::isAvailable()) {
        if (auto result = CredentialStore::deletePassword(id); !result) return result;
    }
    if (deleteCredentials && !deleteLocalData) {
        if (auto result = repository.clearServerSession(id); !result) return result;
    }
    return repository.deleteServer(id, deleteLocalData);
}

bool verifyPrivacyPin(const SessionRepository& repository, const QString& pin)
{
    const auto salt = repository.privacyPinSalt();
    const auto hash = repository.privacyPinHash();
    return !salt.isEmpty() && !hash.isEmpty() && hash == QString::fromLatin1(QCryptographicHash::hash(
        (salt + QLatin1Char(':') + pin).toUtf8(), QCryptographicHash::Sha256).toHex());
}
}
