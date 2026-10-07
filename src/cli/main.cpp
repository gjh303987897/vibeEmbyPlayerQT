#include "cli/CliCommand.h"
#include "cli/CliConsole.h"
#include "cli/CliEngine.h"
#include "cli/CliDaemon.h"
#include "cli/CliShell.h"
#include "utils/ErrorPresentation.h"
#include "utils/AppLogger.h"
#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QSettings>
#include <QStringConverter>
#include <QTextStream>
#include <QTimer>

namespace {
void messages(QtMsgType, const QMessageLogContext&, const QString& message)
{
    QTextStream stream(stderr);
    stream.setEncoding(QStringConverter::Utf8);
    stream << ErrorPresentation::sanitizeDetails(message) << '\n';
    stream.flush();
}

int run(const QStringList& arguments)
{
    auto parsed = Cli::parse(arguments);
    if (!parsed) {
        Cli::Invocation invalid;
        invalid.command = arguments.value(0);
        if (arguments.contains("--json")) invalid.flags.insert("json");
        Cli::writeResult(invalid, {parsed.error().code, {}, parsed.error().message});
        return static_cast<int>(parsed.error().code);
    }
    const auto profile = parsed->option("data-dir");
    if (!profile.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir(profile).absolutePath());
    }
    if (parsed->command == "gui") {
#ifdef Q_OS_WIN
        const auto program = QDir(QCoreApplication::applicationDirPath()).filePath("vibePlayerQT.exe");
#else
        const auto program = QDir(QCoreApplication::applicationDirPath()).filePath("vibePlayerQT");
#endif
        if (CliConsole::launchDesktop(program)) return 0;
        Cli::writeResult(*parsed, {Cli::ExitCode::Failed, {}, "Unable to start the desktop application"});
        return 1;
    }
    Cli::Engine engine(profile);
    if (parsed->command == "daemon run") return Cli::runDaemon(profile);
    if (parsed->command != "shell") {
        const auto result = engine.execute(*parsed);
        Cli::writeResult(*parsed, result);
        return static_cast<int>(result.code);
    }
    if (parsed->has("private")) {
        auto unlock = *parsed;
        unlock.command = "privacy unlock";
        const auto result = engine.execute(unlock);
        if (result.code != Cli::ExitCode::Success) {
            Cli::writeResult(unlock, result);
            return static_cast<int>(result.code);
        }
    }
    Cli::Shell shell(engine,*parsed);
    CliConsole::setLineEditor([&](const QString& prefix) { return shell.complete(prefix); },
                             [&] { return shell.history(); });
    int status = 0;
    while (const auto line = CliConsole::readLine(engine.prompt())) {
        if (line->trimmed().isEmpty()) continue;
        const auto [command,result] = shell.execute(*line);
        if (command.command == "exit") break;
        Cli::writeResult(command,result);
        if (result.code != Cli::ExitCode::Success) {
            status = static_cast<int>(result.code);
            if (parsed->has("fail-fast")) break;
        }
    }
    CliConsole::setLineEditor({},{});
    return CliConsole::interactive() ? 0 : status;
}
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName("vibePlayerQT");
    QCoreApplication::setOrganizationName("vibePlayerQT");
    QCoreApplication::setApplicationVersion(VIBEPLAYER_VERSION);
    qInstallMessageHandler(messages);
    AppLogger::setSanitizer([](const QString& message) { return ErrorPresentation::sanitizeDetails(message); });
    CliConsole::initialize();
    QTimer::singleShot(0, &application, [&] { application.exit(run(application.arguments().mid(1))); });
    return application.exec();
}
