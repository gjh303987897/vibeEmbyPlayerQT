#include "cli/CliEngine.h"
#include "cli/CliRuntime.h"
#include "cli/CliCheckpoint.h"
#include "services/management/ServiceOperations.h"
#include "services/management/WorkflowSettings.h"
#include "services/credentials/CredentialStore.h"
#include "services/emby/EmbyClient.h"
#include "services/jellyfin/JellyfinClient.h"
#include "services/iptv/IptvParser.h"
#include "services/smb/SmbClient.h"
#include "services/webdav/WebDavDownloadPlanner.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QNetworkReply>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>

namespace {
QString responsiveFileHash(const QString& path)
{
    return Cli::awaitFuture(QtConcurrent::run([path] { return Cli::fileHash(path); }));
}

QJsonObject cardJson(const ServiceCard& card)
{
    const auto& server = card.server;
    bool credentials = false;
    if ((server.serviceType == ServiceType::WebDAV || server.serviceType == ServiceType::SMB) && CredentialStore::isAvailable()) {
        const auto saved = CredentialStore::loadPassword(server.id);
        credentials = saved && saved->has_value();
    }
    return {{"id",server.id}, {"name",server.name}, {"type",serviceTypeToString(server.serviceType)},
        {"address",server.serviceType == ServiceType::IPTV ? server.baseUrl :
            QUrl(server.baseUrl).adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment).toString()},
        {"username",server.username}, {"hasSavedSession",server.serviceType != ServiceType::IPTV && card.hasSession},
        {"hasSavedCredentials",credentials},
        {"private",server.privateMode}, {"autoLogin",server.autoLogin},
        {"trustSelfSigned",server.trustSelfSignedCertificate}};
}
QJsonObject itemJson(const WebDavItem& item)
{
    return {{"name",item.name}, {"directory",item.directory}, {"size",static_cast<double>(item.size)}};
}
std::expected<bool, Cli::Error> boolean(const QString& value)
{
    if (value == "true") return true;
    if (value == "false") return false;
    return std::unexpected(Cli::Error {Cli::ExitCode::Usage, QStringLiteral("Expected true or false")});
}
bool contained(const QUrl& root, const QUrl& item)
{
    auto path = root.path(QUrl::FullyEncoded);
    if (!path.endsWith('/')) path += '/';
    return root.scheme() == item.scheme() && root.host() == item.host() && root.port() == item.port() &&
        item.userInfo().isEmpty() && (item.path(QUrl::FullyEncoded).startsWith(path) ||
            item.path(QUrl::FullyEncoded) + '/' == path);
}

bool publishDownload(const QString& staged, const QString& destination, bool overwrite)
{
    if (!overwrite) return QFile::rename(staged,destination);
    QFile input(staged);
    QSaveFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return false;
    while (!input.atEnd()) {
        if (CliConsole::pollCancellation()) return false;
        const auto block = input.read(1024 * 1024);
        if (block.isEmpty() || output.write(block) != block.size()) return false;
    }
    return output.commit();
}

void transferProgress(TransferManager& manager,const QString& label)
{
    QObject::connect(&manager,&TransferManager::taskProgress,&manager,
        [label,last = -1](const QString&,qint64 done,qint64 total) mutable {
            const auto percent = total > 0 ? static_cast<int>(100.0 * done / total) : -1;
            if (percent >= 0 && percent != last) {
                last = percent;
                CliConsole::progress(QStringLiteral("%1: %2%").arg(label).arg(percent));
            }
        });
}

Cli::ExitCode transferFailureCode(const QString& error)
{
    if (error == "HTTP 401" || error == "HTTP 403") return Cli::ExitCode::Authentication;
    if (error == "HTTP 404") return Cli::ExitCode::NotFound;
    if (error == "HTTP 405" || error == "HTTP 409" || error == "HTTP 412") return Cli::ExitCode::Conflict;
    return Cli::ExitCode::Failed;
}
}

Cli::Engine::Engine(QString profile) : m_profile(std::move(profile)) {}
Cli::Result Cli::Engine::transferControl(const QString& action)
{
    if (action == "cancel") { CliConsole::canceled.store(true); CliConsole::transferPaused.store(false); }
    if (m_smbRunning) {
        if (action == "pause") CliConsole::transferPaused.store(true);
        if (action == "resume") CliConsole::transferPaused.store(false);
        return success(QStringLiteral("SMB task control requested"));
    }
    if (!m_activeTransfer) return action == "cancel" ? success(QStringLiteral("Cancellation requested")) :
        failure(ExitCode::Conflict,"The current operation cannot be paused at this stage");
    auto* model = m_activeTransfer->tasks();
    for (int i = 0; i < model->count(); ++i) {
        const auto id = model->data(model->index(i),TransferTaskListModel::IdRole).toString();
        if (action == "pause") m_activeTransfer->pauseTask(id);
        else if (action == "resume") m_activeTransfer->resumeTask(id);
        else if (action == "cancel") m_activeTransfer->cancelTask(id);
    }
    return success(QStringLiteral("Task control requested"));
}
QString Cli::Engine::databasePath() const
{ return m_profile.isEmpty() ? QString() : QDir(m_profile).filePath("vibeplayer.sqlite3"); }
QString Cli::Engine::storagePath() const
{ return m_profile.isEmpty() ? QString() : QDir(m_profile).filePath("tssl"); }
QString Cli::Engine::prompt() const
{
    auto name = m_serviceName;
    for (auto& ch : name) if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) ch = QLatin1Char(' ');
    return m_serviceId.isEmpty() ? QStringLiteral("vibe> ") : QStringLiteral("vibe[%1:%2]> ").arg(name,m_directory);
}

std::expected<QStringList, Cli::Error> Cli::Engine::expand(const QStringList& words) const
{
    if (words.isEmpty()) return words;
    static const QStringList shortCommands {"ls","mkdir","upload","download","stat","remove","move","copy","rename"};
    if (!shortCommands.contains(words.front())) return words;
    if (m_serviceId.isEmpty()) return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Select a WebDAV service with use first")});
    auto result = QStringList {"webdav", words.front(), m_serviceId};
    auto args = words.mid(1);
    if (words.front() == "ls" && args.isEmpty()) args += m_directory;
    int positional = 0;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i].startsWith("--")) {
            if (!QStringList {"--json","--private","--yes","--overwrite","--parents","--resume","--background"}.contains(args[i]) &&
                !args[i].contains('=') && i + 1 < args.size()) ++i;
            continue;
        }
        const bool remote = ((words.front() == "upload" && positional == 1) ||
                            (words.front() != "upload" && positional == 0) ||
                            (QStringList {"copy","move","rename"}.contains(words.front()) && positional == 1));
        if (remote && !args[i].startsWith('/') && !args[i].startsWith("--"))
            args[i] = m_directory + '/' + args[i];
        ++positional;
    }
    return result + args;
}

std::expected<QString, Cli::Error> Cli::Engine::secret(const Invocation& invocation, const QString& option,
                                                       const QString& prompt)
{
    if (invocation.has(option)) {
        const auto name = invocation.option(option);
        if (name.isEmpty() || !qEnvironmentVariableIsSet(name.toUtf8().constData()))
            return std::unexpected(Error {ExitCode::Authentication, QStringLiteral("The credential environment variable is not set")});
        return qEnvironmentVariable(name.toUtf8().constData());
    }
    if (!CliConsole::interactive()) return std::unexpected(Error {ExitCode::Authentication,
        QStringLiteral("Credential required; supply --%1 NAME for non-interactive execution").arg(option)});
    const auto value = CliConsole::readLine(prompt, true);
    if (CliConsole::pollCancellation()) return std::unexpected(Error {ExitCode::Canceled, QStringLiteral("Canceled")});
    if (!value) return std::unexpected(Error {ExitCode::Authentication, QStringLiteral("Credential input ended")});
    return *value;
}

std::expected<QString, Cli::Error> Cli::Engine::password(const Invocation& invocation, const ServerConfig& server)
{
    if ((server.serviceType == ServiceType::WebDAV || server.serviceType == ServiceType::SMB) && !invocation.has("password-env") && CredentialStore::isAvailable()) {
        auto saved = CredentialStore::loadPassword(server.id);
        if (!saved) return std::unexpected(Error {ExitCode::Authentication, saved.error()});
        if (*saved) return **saved;
    }
    return secret(invocation, "password-env", QStringLiteral("Password: "));
}

std::expected<void, Cli::Error> Cli::Engine::unlock(const Invocation& invocation, SessionRepository& repository)
{
    if (m_private) return {};
    auto pin = secret(invocation, "pin-env", QStringLiteral("Privacy PIN: "));
    if (!pin) return std::unexpected(pin.error());
    if (!ServiceOperations::verifyPrivacyPin(repository, *pin))
        return std::unexpected(Error {ExitCode::Authentication, QStringLiteral("Privacy PIN is incorrect or not configured")});
    m_private = true;
    m_pin = *pin;
    return {};
}

std::expected<ServiceCard, Cli::Error> Cli::Engine::resolve(SessionRepository& repository, const QString& target,
                                                          bool webdavOnly)
{
    const auto cards = repository.loadServiceCards(m_private);
    if (!cards) return std::unexpected(Error {ExitCode::Failed, cards.error()});
    std::optional<ServiceCard> found;
    for (const auto& card : *cards) if (card.server.id == target) { found = card; break; }
    if (!found) for (const auto& card : *cards) if (card.server.name == target) {
        if (found) return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Service name is ambiguous; specify its complete ID")});
        found = card;
    }
    if (!found) return std::unexpected(Error {ExitCode::NotFound, QStringLiteral("Service not found in the accessible services")});
    if (webdavOnly && found->server.serviceType != ServiceType::WebDAV)
        return std::unexpected(Error {ExitCode::Usage, QStringLiteral("This command requires a WebDAV service")});
    if (found->server.serviceType != ServiceType::IPTV) {
        if (auto valid = ServiceOperations::validate(found->server); !valid)
            return std::unexpected(Error {ExitCode::Usage, valid.error()});
    }
    return *found;
}

bool Cli::Engine::confirm(const Invocation& invocation, const QString& prompt)
{
    if (invocation.has("yes")) return true;
    if (!CliConsole::interactive()) return false;
    return CliConsole::readLine(prompt + QStringLiteral(" [y/N]: ")).value_or(QString()).toLower() == "y";
}

std::expected<QUrl, Cli::Error> Cli::Engine::remoteUrl(const ServerConfig& server, const QString& input) const
{
    QStringList segments;
    for (const auto& segment : input.split('/', Qt::SkipEmptyParts)) {
        if (segment == ".") continue;
        if (segment == "..") {
            if (segments.isEmpty()) return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Path escapes the service root")});
            segments.removeLast();
        } else {
            if (segment.contains('\\') || std::ranges::any_of(segment, [](QChar ch) { return ch.unicode() < 0x20; }))
                return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Invalid remote path")});
            segments += segment;
        }
    }
    QUrl url(server.baseUrl);
    auto path = url.path(QUrl::FullyEncoded);
    if (!path.endsWith('/')) path += '/';
    for (int i = 0; i < segments.size(); ++i) {
        if (i) path += '/';
        path += QString::fromLatin1(QUrl::toPercentEncoding(segments[i]));
    }
    url.setPath(path, QUrl::StrictMode);
    return url;
}

Cli::Result Cli::Engine::execute(const Invocation& invocation)
{
    CliConsole::pollCancellation();
    CliConsole::canceled.store(false);
    if (invocation.command == "help") return success(help(invocation.arguments.join(' ')));
    if (invocation.command == "version") return success(QStringLiteral(VIBEPLAYER_VERSION));
    const bool write = invocation.command == "service add" || invocation.command == "service update" ||
        invocation.command == "service login" || invocation.command == "service remove" ||
        invocation.command == "service logout" || invocation.command == "service move";
    SessionRepository repository("cli-" + QUuid::createUuid().toString(QUuid::WithoutBraces), databasePath(),
                                 write ? SessionRepository::OpenMode::ReadWrite : SessionRepository::OpenMode::ReadOnly);
    const bool wasPrivate = m_private;
    if (invocation.has("private")) {
        if (auto result = unlock(invocation, repository); !result) return failure(result.error());
    }
    // Per-command private access does not silently unlock a whole interactive session.
    struct RestorePrivacy { bool& state; bool previous; ~RestorePrivacy() { state = previous; } };
    RestorePrivacy privacyScope {m_private, wasPrivate};
    if (invocation.has("background") || invocation.command.startsWith("task ") || invocation.command == "daemon stop")
        return tasks(invocation,repository);
    if (invocation.command == "privacy unlock") {
        auto result = unlock(invocation, repository);
        if (!result) return failure(result.error());
        privacyScope.previous = true;
        return success(QStringLiteral("Privacy mode unlocked for this session"));
    }
    if (invocation.command == "privacy lock") {
        m_pin.clear();
        privacyScope.previous = false;
        m_serviceId.clear(); m_serviceName.clear(); m_directory = "/";
        return success(QStringLiteral("Privacy mode locked"));
    }
    if (invocation.command == "use") {
        auto card = resolve(repository, invocation.arguments.front(), true);
        if (!card) return failure(card.error());
        m_serviceId = card->server.id; m_serviceName = card->server.name; m_directory = "/";
        return success(cardJson(*card));
    }
    if (invocation.command == "privacy set" || invocation.command == "privacy change" || invocation.command == "privacy remove") {
        auto result = privacy(invocation,repository);
        if (result.code == ExitCode::Success) privacyScope.previous = false;
        return result;
    }
    if (invocation.command == "pwd") return success(m_directory);
    if (invocation.command == "cd") {
        if (m_serviceId.isEmpty()) return failure(ExitCode::Usage, "Select a service with use first");
        auto card = resolve(repository, m_serviceId, true);
        if (!card) return failure(card.error());
        const auto path = invocation.arguments.front().startsWith('/') ? invocation.arguments.front() :
            m_directory + '/' + invocation.arguments.front();
        auto url = remoteUrl(card->server, path);
        if (!url) return failure(url.error());
        auto credential = password(invocation, card->server);
        if (!credential) return failure(credential.error());
        WebDavClient client;
        auto result = await<WebDavItemResult>([&](auto done) { client.statItem(card->server, *credential, *url, done); },
                                              [&] { client.cancelRequests(); });
        if (!result) return networkFailure(result.error());
        if (!result->directory) return failure(ExitCode::Usage, "The path is not a directory");
        m_directory = QDir::cleanPath('/' + path);
        return success(m_directory);
    }
    if (invocation.command.startsWith("config ")) {
        auto values = WorkflowSettings::snapshot();
        if (invocation.command == "config list") return success(QJsonObject::fromVariantMap(values));
        const auto key = invocation.arguments.front();
        if (!values.contains(key)) return failure(ExitCode::Usage, "Unknown or unsupported configuration key");
        if (invocation.command == "config set") {
            auto result = WorkflowSettings::set(key, invocation.arguments[1]);
            if (!result) return failure(ExitCode::Usage, result.error());
            values = WorkflowSettings::snapshot();
        }
        return success(QJsonObject {{key, QJsonValue::fromVariant(values.value(key))}});
    }
    if (write) if (auto result = repository.initialize(); !result) return failure(ExitCode::Failed, result.error());
    if (invocation.command.startsWith("service ")) return service(invocation, repository);
    if (invocation.command.startsWith("webdav ")) return webdav(invocation, repository);
    if (invocation.command.startsWith("tssl ")) return tssl(invocation, repository);
    if (invocation.command.startsWith("video ")) return video(invocation, repository);
    if (invocation.command.startsWith("media ") || invocation.command == "play" || invocation.command.startsWith("player "))
        return media(invocation,repository);
    if (invocation.command.startsWith("smb ")) return smb(invocation,repository);
    return failure(ExitCode::Usage, "Command is available only from the interactive shell");
}

Cli::Result Cli::Engine::service(const Invocation& invocation, SessionRepository& repository)
{
    if (invocation.command == "service list") {
        auto cards = repository.loadServiceCards(m_private);
        if (!cards) return failure(ExitCode::Failed, cards.error());
        QJsonArray data;
        for (const auto& card : *cards) data += cardJson(card);
        return success(data);
    }
    if (invocation.command == "service add" || invocation.command == "service update") {
        ServerConfig server;
        std::optional<ServerConfig> previous;
        if (invocation.command == "service update") {
            auto card = resolve(repository, invocation.arguments.front());
            if (!card) return failure(card.error());
            server = card->server; previous = server;
        } else {
            const auto type = invocation.option("type").toLower();
            if (type != "emby" && type != "jellyfin" && type != "webdav" && type != "iptv" && type != "smb")
                return failure(ExitCode::Usage, "--type must be emby, jellyfin, webdav, smb or iptv");
            server.serviceType = serviceTypeFromString(type);
            server.privateMode = m_private;
        }
        if (invocation.has("name")) server.name = invocation.option("name").trimmed();
        if (invocation.has("username")) server.username = invocation.option("username").trimmed();
        if (invocation.has("url")) server.baseUrl = invocation.option("url").trimmed();
        if (invocation.has("trust-self-signed")) {
            auto value = boolean(invocation.option("trust-self-signed"));
            if (!value) return failure(value.error());
            server.trustSelfSignedCertificate = *value;
        }
        if (invocation.has("auto-login")) {
            auto value = boolean(invocation.option("auto-login"));
            if (!value) return failure(value.error());
            server.autoLogin = *value;
        }
        if (invocation.has("private-mode")) {
            auto value = boolean(invocation.option("private-mode"));
            if (!value) return failure(value.error());
            if (*value && !m_private) return failure(ExitCode::Authentication, "Unlock privacy mode before moving a service into it");
            server.privateMode = *value;
        }
        QString playlist = invocation.option("playlist", server.baseUrl);
        const QUrl origin(playlist);
        const bool remotePlaylist = server.serviceType == ServiceType::IPTV &&
            (origin.scheme() == "http" || origin.scheme() == "https");
        QTemporaryDir playlistStaging;
        if (server.serviceType == ServiceType::IPTV) {
            if (!previous && !invocation.has("playlist")) return failure(ExitCode::Usage, "IPTV requires --playlist FILE");
            server.id = previous ? previous->id : ServiceOperations::iptvId(remotePlaylist ? origin.toString() : QFileInfo(playlist).absoluteFilePath());
            if (remotePlaylist) {
                if (origin.host().isEmpty() || !origin.userInfo().isEmpty() || !playlistStaging.isValid())
                    return failure(ExitCode::Usage,"Invalid IPTV playlist URL");
                QNetworkAccessManager manager;
                QNetworkRequest request(origin);
                request.setTransferTimeout(30000);
                auto* reply = manager.get(request);
                auto payload = await<std::expected<QByteArray,QString>>([&](auto done) {
                    QObject::connect(reply,&QNetworkReply::readyRead,reply,[reply] {
                        if (reply->bytesAvailable() > 16 * 1024 * 1024) reply->abort();
                    });
                    QObject::connect(reply,&QNetworkReply::finished,reply,[reply,done] {
                        const auto ok = reply->error() == QNetworkReply::NoError;
                        done(ok ? std::expected<QByteArray,QString>(reply->readAll()) :
                                  std::unexpected(QStringLiteral("IPTV playlist download failed")));
                    });
                },[&] { reply->abort(); });
                if (!payload) return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Failed,payload.error());
                playlist = QDir(playlistStaging.path()).filePath("playlist.m3u");
                QFile file(playlist);
                if (!file.open(QIODevice::WriteOnly) || file.write(*payload) != payload->size())
                    return failure(ExitCode::Failed,"Cannot stage IPTV playlist");
            }
        } else {
            if (server.serviceType == ServiceType::WebDAV) {
                QUrl url(server.baseUrl);
                if (!url.path().endsWith('/')) url.setPath(url.path() + '/');
                server.baseUrl = url.toString();
            }
            if (!previous || invocation.has("url") || invocation.has("username"))
                server.id = ServiceOperations::cardId(server.baseUrl, server.serviceType, server.username);
        }
        if (auto valid = ServiceOperations::validate(server); !valid) return failure(ExitCode::Usage, valid.error());
        const auto cards = repository.loadAllServiceCards();
        if (!cards) return failure(ExitCode::Failed, cards.error());
        for (const auto& card : *cards) if (card.server.id == server.id && (!previous || server.id != previous->id))
            return failure(ExitCode::Conflict, "A service with this endpoint and username already exists");
        auto saved = [&]() -> std::expected<void,QString> {
            if (server.serviceType == ServiceType::IPTV && previous && !invocation.has("playlist")) {
                auto storedPlaylist = repository.loadIptvPlaylist(server.id);
                auto channels = repository.loadIptvChannels(server.id);
                if (!storedPlaylist) return std::unexpected(storedPlaylist.error());
                if (!channels) return std::unexpected(channels.error());
                if (!*storedPlaylist) return std::unexpected(QStringLiteral("Stored IPTV playlist is missing"));
                auto metadata = **storedPlaylist;
                metadata.name = server.name;
                return repository.saveIptvPlaylist(server,metadata,*channels);
            }
            return server.serviceType == ServiceType::IPTV
            ? ServiceOperations::savePlaylist(repository, server, playlist,
                m_profile.isEmpty() ? QString() : QDir(m_profile).filePath("iptv"),remotePlaylist ? origin : QUrl()) : ServiceOperations::save(repository, server);
        }();
        if (!saved) return failure(ExitCode::Failed, saved.error());
        if (previous && previous->id != server.id) {
            auto removed = ServiceOperations::remove(repository, previous->id);
            if (!removed) return failure(ExitCode::Partial, removed.error());
        }
        const auto stored = repository.loadServiceCards(m_private);
        if (!stored) return failure(ExitCode::Failed,stored.error());
        for (const auto& card : *stored) if (card.server.id == server.id) return success(cardJson(card));
        return failure(ExitCode::Failed,"Saved service could not be read back");
    }
    auto card = resolve(repository, invocation.arguments.front());
    if (!card) return failure(card.error());
    const auto server = card->server;
    if (invocation.command == "service show") return success(cardJson(*card));
    if (invocation.command == "service move") {
        bool ok = false; const int index = invocation.arguments[1].toInt(&ok);
        if (!ok || index < 0) return failure(ExitCode::Usage,"Index must be a nonnegative integer");
        auto result = repository.moveServerTo(server.id,index,server.privateMode);
        return result ? success(cardJson(*card)) : failure(ExitCode::Failed,result.error());
    }
    if (invocation.command == "service logout") {
        if (CredentialStore::isAvailable()) {
            auto removed = CredentialStore::deletePassword(server.id);
            if (!removed) return failure(ExitCode::Failed,removed.error());
        }
        auto removed = repository.clearServerSession(server.id);
        return removed ? success(QStringLiteral("Saved authentication removed")) : failure(ExitCode::Failed,removed.error());
    }
    if (invocation.command == "service remove") {
        if (!confirm(invocation, QStringLiteral("Remove this service?"))) return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Usage, "Removal requires --yes or interactive confirmation");
        auto result = ServiceOperations::remove(repository, server.id);
        return result ? success(QJsonObject {{"removed",server.id}}) : failure(ExitCode::Failed, result.error());
    }
    if (server.serviceType == ServiceType::IPTV) {
        auto channels = repository.loadIptvChannels(server.id);
        return channels ? success(QJsonObject {{"connected",true},{"channels",static_cast<int>(channels->size())}})
                        : failure(ExitCode::Failed,channels.error());
    }
    if (server.serviceType == ServiceType::SMB) {
        auto credential = password(invocation,server);
        if (!credential) return failure(credential.error());
        auto result = awaitFuture(QtConcurrent::run([server,credential = *credential] { return SmbClient::connect(server,credential); }));
        if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
        if (!result) return failure(ExitCode::Authentication,result.error());
        if (invocation.command == "service login" && CredentialStore::isAvailable()) {
            auto saved = CredentialStore::savePassword(server.id,server.username,*credential);
            if (!saved) return failure(ExitCode::Failed,saved.error());
        }
        return success(QJsonObject {{"connected",true},{"credentialsSaved",invocation.command == "service login" && CredentialStore::isAvailable()}});
    }
    if (server.serviceType == ServiceType::WebDAV) {
        auto credential = password(invocation, server);
        if (!credential) return failure(credential.error());
        WebDavClient client;
        auto response = await<WebDavListResult>([&](auto done) { client.listDirectory(server, *credential, QUrl(server.baseUrl), done); },
                                                [&] { client.cancelRequests(); });
        if (!response) return networkFailure(response.error());
        if (invocation.command == "service login" && CredentialStore::isAvailable()) {
            auto saved = CredentialStore::savePassword(server.id, server.username, *credential);
            if (!saved) return failure(ExitCode::Failed, saved.error());
        }
        return success(QJsonObject {{"connected",true}, {"credentialsSaved",invocation.command == "service login" && CredentialStore::isAvailable()}});
    }
    NetworkClient network;
    EmbyClient emby(network);
    JellyfinClient jellyfin(network);
    MediaServiceClient& client = server.serviceType == ServiceType::Emby ? static_cast<MediaServiceClient&>(emby) :
                                                                          static_cast<MediaServiceClient&>(jellyfin);
    if (invocation.command == "service login") {
        auto credential = password(invocation, server);
        if (!credential) return failure(credential.error());
        auto response = await<LoginResult>([&](auto done) { client.login(server, server.username, *credential, done); },
                                           [&] { network.cancelRequests(); });
        if (!response) return networkFailure(response.error());
        auto saved = repository.saveSession(*response);
        if (!saved) return failure(ExitCode::Failed, saved.error());
        return success(QJsonObject {{"connected",true}, {"sessionSaved",true}});
    }
    const auto session = repository.loadSession(server.id);
    if (!session) return failure(ExitCode::Failed, session.error());
    if (!*session) return failure(ExitCode::Authentication, "No saved session; run service login first");
    auto response = await<LibraryResult>([&](auto done) { client.fetchLibraries(**session, done); },
                                         [&] { network.cancelRequests(); });
    return response ? success(QJsonObject {{"connected",true}, {"libraries",static_cast<int>(response->size())}})
                    : networkFailure(response.error());
}

Cli::Result Cli::Engine::transferUpload(const Invocation& invocation, const ServerConfig& server,
                                       const QString& credential, const QString& source, const QUrl& target)
{
    if (!QFileInfo::exists(source)) return failure(ExitCode::NotFound, "The upload source does not exist");
    auto plan = awaitFuture(QtConcurrent::run([source, target] {
        return WebDavUploadPlanner::plan(source, target, CliConsole::canceled);
    }));
    if (!plan) return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Failed, plan.error());
    int completed = 0, failed = 0;
    QString firstError;
    for (const auto& request : *plan) {
        if (CliConsole::pollCancellation()) break;
        bool ok = false;
        QString message;
        const auto key = request.remoteUrl.toString();
        const auto record = checkpoint(m_checkpointDirectory,key);
        if (!record.isEmpty()) {
            WebDavClient client;
            auto current = await<WebDavItemResult>([&](auto done) { client.statItem(server,credential,request.remoteUrl,done); },
                                                   [&] { client.cancelRequests(); });
            if (current && ((request.directory && current->directory) ||
                (!request.directory && !current->etag.isEmpty() && current->etag == record.value("etag").toString() &&
                 !record.value("hash").toString().isEmpty() &&
                 responsiveFileHash(request.localPath) == record.value("hash").toString()))) { ++completed; continue; }
        }
        if (request.directory) {
            WebDavClient client;
            auto result = await<WebDavVoidResult>([&](auto done) { client.createDirectory(server, credential, request.remoteUrl, done); },
                                                  [&] { client.cancelRequests(); });
            ok = result.has_value();
            if (!ok && result.error().httpStatus == 405 && invocation.has("overwrite")) {
                auto info = await<WebDavItemResult>([&](auto done) { client.statItem(server, credential, request.remoteUrl, done); },
                                                   [&] { client.cancelRequests(); });
                ok = info && info->directory;
            }
            if (!ok) {
                const auto error = result ? failure(ExitCode::Failed, "Unable to create directory") : networkFailure(result.error());
                return failure(CliConsole::pollCancellation() ? ExitCode::Canceled :
                    completed ? ExitCode::Partial : error.code, error.message,
                    QJsonObject {{"completed",completed}, {"failed",1}});
            }
        } else {
            TransferManager manager;
            m_activeTransfer = &manager;
            if (m_progressObserver) QObject::connect(&manager,&TransferManager::taskProgress,&manager,
                [&](const QString&,qint64 done,qint64 total) { m_progressObserver(done,total); });
            transferProgress(manager,QStringLiteral("Upload"));
            auto result = await<std::pair<bool,QString>>([&](auto done) {
                QObject::connect(&manager, &TransferManager::taskFinished, &manager,
                    [done](const QString&, bool success, const QString& text) { done({success,text}); });
                manager.enqueueUpload(server, credential, request.localPath, request.remoteUrl, request.totalBytes,
                    invocation.has("overwrite") ? TransferManager::OverwritePolicy::Allow : TransferManager::OverwritePolicy::Refuse);
            }, [&] {
                const auto* model = manager.tasks();
                for (int i = 0; i < model->count(); ++i)
                    manager.cancelTask(model->data(model->index(i), TransferTaskListModel::IdRole).toString());
            });
            ok = result.first; message = result.second;
        }
        if (ok) ++completed;
        else { ++failed; if (firstError.isEmpty()) firstError = message; }
        if (ok && !m_checkpointDirectory.isEmpty()) {
            WebDavClient client;
            auto current = await<WebDavItemResult>([&](auto done) { client.statItem(server,credential,request.remoteUrl,done); },
                                                   [&] { client.cancelRequests(); });
            if (!saveCheckpoint(m_checkpointDirectory,key,QJsonObject {{"directory",request.directory},
                {"etag",current ? current->etag : QString()},{"hash",request.directory ? QString() : responsiveFileHash(request.localPath)}}))
                return failure(ExitCode::Failed,"Unable to save transfer checkpoint");
        }
        CliConsole::progress(QStringLiteral("Upload: %1 / %2").arg(completed + failed).arg(plan->size()));
    }
    const QJsonObject summary {{"completed",completed}, {"failed",failed}};
    if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled, "Canceled", summary);
    return failed ? failure(completed ? ExitCode::Partial : transferFailureCode(firstError), firstError, summary) : success(summary);
}

Cli::Result Cli::Engine::webdav(const Invocation& invocation, SessionRepository& repository)
{
    auto card = resolve(repository, invocation.arguments[0], true);
    if (!card) return failure(card.error());
    auto credential = password(invocation, card->server);
    if (!credential) return failure(credential.error());
    auto url = remoteUrl(card->server, invocation.command == "webdav upload" ?
        invocation.arguments.value(2) : invocation.arguments.value(1, "/"));
    if (!url) return failure(url.error());
    WebDavClient client;
    if (QStringList {"webdav remove","webdav copy","webdav move","webdav rename"}.contains(invocation.command)) {
        const auto root = QUrl(card->server.baseUrl).path();
        if (url->path().trimmed() == root || url->path() + '/' == root)
            return failure(ExitCode::Usage,"The service root cannot be modified");
        QUrl destination;
        if (invocation.command != "webdav remove") {
            auto target = remoteUrl(card->server,invocation.arguments[2]);
            if (!target) return failure(target.error());
            destination = *target;
            auto sourcePath = url->path(); if (!sourcePath.endsWith('/')) sourcePath += '/';
            if (destination == *url || destination.path().startsWith(sourcePath))
                return failure(ExitCode::Usage,"The destination must differ from the source and cannot be inside it");
        } else if (!confirm(invocation,"Delete this remote resource and its children?"))
            return failure(CliConsole::pollCancellation() ? ExitCode::Canceled : ExitCode::Usage,"Deletion requires --yes or confirmation");
        const auto operation = invocation.command == "webdav remove" ? WebDavClient::Mutation::Remove :
            invocation.command == "webdav copy" ? WebDavClient::Mutation::Copy : WebDavClient::Mutation::Move;
        auto result = await<WebDavVoidResult>([&](auto done) {
            client.mutate(card->server,*credential,operation,*url,destination,invocation.has("overwrite"),done);
        },[&] { client.cancelRequests(); });
        return result ? success(QJsonObject {{"source",invocation.arguments[1]},{"target",invocation.arguments.value(2)}})
                      : networkFailure(result.error());
    }
    if (invocation.command == "webdav ls") {
        if (!url->path().endsWith('/')) url->setPath(url->path() + '/');
        auto response = await<WebDavListResult>([&](auto done) { client.listDirectory(card->server, *credential, *url, done); },
                                                [&] { client.cancelRequests(); });
        if (!response) return networkFailure(response.error());
        QJsonArray data;
        for (const auto& item : *response) data += itemJson(item);
        return success(data);
    }
    if (invocation.command == "webdav mkdir") {
        if (url->path() == QUrl(card->server.baseUrl).path()) return failure(ExitCode::Conflict, "The service root already exists");
        if (!url->path().endsWith('/')) url->setPath(url->path() + '/');
        if (invocation.has("parents")) {
            auto parent = QUrl(card->server.baseUrl);
            const auto base = parent.path().endsWith('/') ? parent.path() : parent.path() + '/';
            for (const auto& part : url->path().mid(base.size()).split('/',Qt::SkipEmptyParts)) {
                auto child = WebDavClient::childDirectoryUrl(parent,part);
                if (!child) return failure(ExitCode::Usage,child.error());
                auto info = await<WebDavItemResult>([&](auto done) { client.statItem(card->server,*credential,*child,done); },
                                                    [&] { client.cancelRequests(); });
                if (info && !info->directory) return failure(ExitCode::Conflict,"An ancestor is a file");
                if (!info) {
                    if (info.error().httpStatus != 404) return networkFailure(info.error());
                    auto created = await<WebDavVoidResult>([&](auto done) { client.createDirectory(card->server,*credential,*child,done); },
                                                          [&] { client.cancelRequests(); });
                    if (!created) return networkFailure(created.error());
                }
                parent = *child;
            }
            return success(QJsonObject {{"created",invocation.arguments[1]}});
        }
        auto response = await<WebDavVoidResult>([&](auto done) { client.createDirectory(card->server, *credential, *url, done); },
                                                [&] { client.cancelRequests(); });
        return response ? success(QJsonObject {{"created",invocation.arguments[1]}}) : networkFailure(response.error());
    }
    if (invocation.command == "webdav upload") {
        auto target = remoteUrl(card->server, invocation.arguments[2]);
        if (!target) return failure(target.error());
        if (!invocation.has("overwrite") && checkpoint(m_checkpointDirectory,target->toString()).isEmpty()) {
            auto existing = await<WebDavItemResult>([&](auto done) { client.statItem(card->server, *credential, *target, done); },
                                                    [&] { client.cancelRequests(); });
            if (existing) return failure(ExitCode::Conflict, "The remote target already exists");
            if (existing.error().httpStatus != 404) return networkFailure(existing.error());
        }
        return transferUpload(invocation, card->server, *credential, invocation.arguments[1], *target);
    }
    auto response = await<WebDavItemResult>([&](auto done) { client.statItem(card->server, *credential, *url, done); },
                                            [&] { client.cancelRequests(); });
    if (!response) return networkFailure(response.error());
    if (invocation.command == "webdav stat") return success(itemJson(*response));
    const QFileInfo target(invocation.arguments[2]);
    struct RestoreCheckpoint { QString& state; QString previous; ~RestoreCheckpoint() { state = previous; } };
    RestoreCheckpoint checkpointScope {m_checkpointDirectory,m_checkpointDirectory};
    if (invocation.has("resume") && m_checkpointDirectory.isEmpty())
        m_checkpointDirectory = QDir(target.absolutePath()).filePath(".vibe-resume-" +
            QString::fromLatin1(QCryptographicHash::hash((card->server.id + url->toString() + target.absoluteFilePath()).toUtf8(),QCryptographicHash::Sha256).toHex().left(16)));
    const bool recovery = checkpoint(m_checkpointDirectory,"root").value("target").toString() == target.absoluteFilePath();
    if (target.isSymLink() || (target.exists() && !invocation.has("overwrite") && !recovery)) return failure(ExitCode::Conflict, "The local target already exists");
    if (target.exists() && target.isDir() != response->directory) return failure(ExitCode::Conflict,"Source and target types differ");
    if (!QFileInfo(target.absolutePath()).isDir()) return failure(ExitCode::Usage, "The target parent directory must exist");
    if (!contained(QUrl(card->server.baseUrl), response->url)) return failure(ExitCode::Failed, "Server returned a resource outside its configured root");
    WebDavDownloadPlanner planner(client);
    auto plan = await<WebDavDownloadPlanResult>([&](auto done) {
        planner.buildPlan(card->server, *credential, *response, target.absoluteFilePath(), done);
    }, [&] { client.cancelRequests(); });
    if (!plan) return networkFailure(plan.error());
    if (!saveCheckpoint(m_checkpointDirectory,"root",QJsonObject {{"target",target.absoluteFilePath()}}))
        return failure(ExitCode::Failed,"Unable to save download checkpoint");
    for (const auto& entry : plan->files)
        if (!contained(QUrl(card->server.baseUrl), entry.remoteUrl))
            return failure(ExitCode::Failed, "Server returned a resource outside its configured root");
    if (response->directory && !target.exists() && !QDir(target.absolutePath()).mkdir(target.fileName()))
        return failure(ExitCode::Conflict, "Unable to create the destination folder without overwriting");
    for (const auto& directory : plan->directories)
        if (!QDir().mkpath(directory)) return failure(ExitCode::Failed, "Unable to create a destination subdirectory");
    int completed = 0, failed = 0;
    QString error;
    auto errorCode = ExitCode::Failed;
    for (const auto& entry : plan->files) {
        if (CliConsole::pollCancellation()) break;
        const QFileInfo destination(entry.localPath);
        WebDavItem revision;
        if (!m_checkpointDirectory.isEmpty()) {
            auto info = await<WebDavItemResult>([&](auto done) { client.statItem(card->server,*credential,entry.remoteUrl,done); },
                                                [&] { client.cancelRequests(); });
            if (!info) return networkFailure(info.error());
            revision = *info;
        }
        const auto key = entry.remoteUrl.toString();
        const auto record = checkpoint(m_checkpointDirectory,key);
        if (destination.exists() && !record.isEmpty() && !revision.etag.isEmpty() &&
            record.value("etag").toString() == revision.etag && !record.value("hash").toString().isEmpty() &&
            record.value("hash").toString() == responsiveFileHash(entry.localPath)) {
            ++completed; continue;
        }
        if (destination.isSymLink() || (destination.exists() && !invocation.has("overwrite"))) {
            ++failed; error = "A destination appeared during download"; errorCode = ExitCode::Conflict; continue;
        }
        QTemporaryDir staging(QDir(destination.absolutePath()).filePath(".vibe-download-XXXXXX"));
        if (!staging.isValid()) return failure(ExitCode::Failed, "Unable to create download staging directory");
        TransferManager manager;
        m_activeTransfer = &manager;
        if (m_progressObserver) QObject::connect(&manager,&TransferManager::taskProgress,&manager,
            [&](const QString&,qint64 done,qint64 total) { m_progressObserver(done,total); });
        transferProgress(manager,QStringLiteral("Download"));
        const auto staged = m_checkpointDirectory.isEmpty() ? QDir(staging.path()).filePath("payload") :
            checkpointFile(m_checkpointDirectory,key) + ".part";
        QDir().mkpath(m_checkpointDirectory.isEmpty() ? staging.path() : m_checkpointDirectory);
        const auto previous = checkpoint(m_checkpointDirectory,"partial:" + key).value("etag").toString();
        if (previous != revision.etag || revision.etag.isEmpty() || revision.etag.startsWith("W/")) QFile::remove(staged);
        if (!saveCheckpoint(m_checkpointDirectory,"partial:" + key,QJsonObject {{"etag",revision.etag}}))
            return failure(ExitCode::Failed,"Unable to save partial transfer metadata");
        auto transfer = await<std::pair<bool,QString>>([&](auto done) {
            QObject::connect(&manager, &TransferManager::taskFinished, &manager,
                [done](const QString&, bool ok, const QString& text) { done({ok,text}); });
            manager.enqueueDownload(card->server, *credential, entry.remoteUrl, staged, entry.bytesTotal,
                                    !m_checkpointDirectory.isEmpty(),revision.etag.startsWith("W/") ? QString() : revision.etag);
        }, [&] {
            auto* model = manager.tasks();
            for (int i = 0; i < model->count(); ++i)
                manager.cancelTask(model->data(model->index(i), TransferTaskListModel::IdRole).toString());
        });
        if (transfer.first && awaitFuture(QtConcurrent::run([&] {
                return publishDownload(staged,entry.localPath,invocation.has("overwrite"));
            }))) {
            ++completed;
            if (!saveCheckpoint(m_checkpointDirectory,key,QJsonObject {{"etag",revision.etag},
                {"hash",m_checkpointDirectory.isEmpty() ? QString() : responsiveFileHash(entry.localPath)}}))
                return failure(ExitCode::Failed,"Unable to save completed transfer metadata");
            QFile::remove(staged);
        }
        else {
            ++failed; error = transfer.first ? "Destination exists or could not be published" : transfer.second;
            errorCode = transfer.first ? (QFileInfo::exists(entry.localPath) ? ExitCode::Conflict : ExitCode::Failed)
                                       : transferFailureCode(transfer.second);
        }
        CliConsole::progress(QStringLiteral("Download: %1 / %2").arg(completed + failed).arg(plan->files.size()));
    }
    const QJsonObject summary {{"completed",completed}, {"failed",failed}, {"target",target.absoluteFilePath()}};
    if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled, "Canceled", summary);
    return failed ? failure(completed ? ExitCode::Partial : errorCode, error, summary) : success(summary);
}
