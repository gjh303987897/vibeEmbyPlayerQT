#pragma once
#include "cli/CliCommand.h"
#include "database/SessionRepository.h"
#include "services/webdav/WebDavClient.h"
#include "services/webdav/WebDavUploadPlanner.h"
#include "services/webdav/TsslStore.h"
#include <QVariantMap>

namespace Cli {
class Engine final {
public:
    explicit Engine(QString profile = {});
    Result execute(const Invocation& invocation);
    QString prompt() const;
    std::expected<QStringList, Error> expand(const QStringList& words) const;
    Result transferControl(const QString& action);
    bool privateAccess() const { return m_private; }
    void setCheckpointDirectory(QString path) { m_checkpointDirectory = std::move(path); }
    void setProgressObserver(std::function<void(qint64,qint64)> observer) { m_progressObserver = std::move(observer); }

private:
    Result executeCommand(const Invocation& invocation);
    std::expected<ServiceCard, Error> resolve(SessionRepository& repository, const QString& target, bool webdav = false);
    std::expected<QString, Error> password(const Invocation& invocation, const ServerConfig& server);
    std::expected<QString, Error> secret(const Invocation& invocation, const QString& option, const QString& prompt);
    std::expected<void, Error> unlock(const Invocation& invocation, SessionRepository& repository);
    std::expected<QUrl, Error> remoteUrl(const ServerConfig& server, const QString& path) const;
    bool confirm(const Invocation& invocation, const QString& prompt);
    Result service(const Invocation& invocation, SessionRepository& repository);
    Result webdav(const Invocation& invocation, SessionRepository& repository);
    Result tssl(const Invocation& invocation, SessionRepository& repository);
    Result video(const Invocation& invocation, SessionRepository& repository);
    Result privacy(const Invocation& invocation, SessionRepository& repository);
    Result media(const Invocation& invocation, SessionRepository& repository);
    Result startPlayback(const Invocation& invocation, const QJsonObject& request);
    Result smb(const Invocation& invocation, SessionRepository& repository);
    Result tasks(const Invocation& invocation, SessionRepository& repository);
    Result transferUpload(const Invocation& invocation, const ServerConfig& server, const QString& password,
                          const QString& source, const QUrl& target);
    QString databasePath() const;
    QString storagePath() const;
    QString m_profile;
    QString m_pin;
    bool m_private {false};
    QString m_serviceId;
    QString m_serviceName;
    QString m_directory {QStringLiteral("/")};
    QPointer<TransferManager> m_activeTransfer;
    QString m_checkpointDirectory;
    bool m_smbRunning {false};
    std::function<void(qint64,qint64)> m_progressObserver;
};
}
