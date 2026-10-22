#include "cli/CliIpc.h"
#include "cli/CliRuntime.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLocalServer>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

QString Cli::socketName(const QString& kind, const QString& profile)
{
    const auto identity = profile.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) :
                          QDir(profile).absolutePath();
    return "vibe-" + kind + '-' + QString::fromLatin1(QCryptographicHash::hash(
        identity.toUtf8(),QCryptographicHash::Sha256).toHex().left(24));
}
QJsonObject Cli::resultObject(const Result& result)
{
    return {{"code",static_cast<int>(result.code)},{"data",result.data},{"message",result.message}};
}
Cli::Result Cli::rpc(const QString& kind, const QString& profile, const QJsonObject& request, bool start)
{
    QLocalSocket socket;
    const auto name = socketName(kind,profile);
    socket.connectToServer(name);
    if (!socket.waitForConnected(200) && start) {
        QProcess process;
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
        QStringList arguments;
        QString program;
        if (kind == "player" || kind == "schedule") program = QDir(QCoreApplication::applicationDirPath()).filePath("vibePlayerPlayback"
#ifdef Q_OS_WIN
            ".exe"
#endif
        );
        else { program = QCoreApplication::applicationFilePath(); arguments = {"daemon","run"}; }
        if (kind == "schedule") arguments += "--schedule-host";
        if (!profile.isEmpty()) arguments += QStringList {"--data-dir",QDir(profile).absolutePath()};
#ifdef Q_OS_WIN
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
            // Detached hosts must not keep a parent's redirected pipes alive.
            args->inheritHandles = false;
            args->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
            args->startupInfo->hStdInput = nullptr;
            args->startupInfo->hStdOutput = nullptr;
            args->startupInfo->hStdError = nullptr;
            args->flags |= CREATE_NO_WINDOW;
            args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            args->startupInfo->wShowWindow = SW_HIDE;
        });
#endif
        process.setProgram(program); process.setArguments(arguments);
        if (!process.startDetached()) return failure(ExitCode::Failed,"Unable to start the background host");
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 5000) {
            socket.abort(); socket.connectToServer(name);
            if (socket.waitForConnected(100)) break;
            QThread::msleep(25);
        }
    }
    if (socket.state() != QLocalSocket::ConnectedState) return failure(ExitCode::NotFound,"The background host is not running");
    const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(bytes) != bytes.size() || (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(2000)))
        return failure(ExitCode::Failed,"Unable to send the host request");
    QByteArray response;
    QElapsedTimer timer; timer.start();
    while (!response.contains('\n') && timer.elapsed() < 60000) {
        if (CliConsole::pollCancellation()) return failure(ExitCode::Canceled,"Canceled");
        if (socket.bytesAvailable() || socket.waitForReadyRead(100)) response += socket.readAll();
        else if (socket.state() == QLocalSocket::UnconnectedState) break;
        if (response.size() > 4 * 1024 * 1024) return failure(ExitCode::Failed,"Host response is too large");
    }
    QJsonParseError error;
    const auto result = QJsonDocument::fromJson(response.left(response.indexOf('\n')),&error).object();
    if (error.error != QJsonParseError::NoError || !result.contains("code"))
        return failure(ExitCode::Failed,"Invalid or missing host response");
    return {static_cast<ExitCode>(result.value("code").toInt()),result.value("data"),result.value("message").toString()};
}

void Cli::serveRequests(QLocalServer& server, std::function<Result(const QJsonObject&)> handler)
{
    QObject::connect(&server,&QLocalServer::newConnection,&server,[&server,handler] {
        while (auto* socket = server.nextPendingConnection()) {
            QObject::connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
            auto buffer = std::make_shared<QByteArray>();
            auto answered = std::make_shared<bool>(false);
            const QPointer<QLocalSocket> guard(socket);
            auto read = [guard,buffer,answered,handler] {
                if (!guard || *answered) return;
                *buffer += guard->readAll();
                if (buffer->size() > 1024 * 1024) { guard->abort(); return; }
                if (!buffer->contains('\n')) return;
                *answered = true;
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(buffer->left(buffer->indexOf('\n')),&error);
                const auto result = error.error == QJsonParseError::NoError && document.isObject()
                    ? handler(document.object()) : failure(ExitCode::Usage,"Invalid host request");
                if (guard) {
                    guard->write(QJsonDocument(resultObject(result)).toJson(QJsonDocument::Compact) + '\n');
                    guard->flush();
                    guard->disconnectFromServer();
                }
            };
            QObject::connect(socket,&QLocalSocket::readyRead,socket,read);
            // A local peer can send before the readyRead connection is installed.
            QTimer::singleShot(0,socket,read);
        }
    });
}
