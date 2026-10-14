#include "cli/CliCommand.h"
#include "cli/CliConsole.h"
#include "cli/CliOutput.h"
#include <QCommandLineParser>
#include <QStringConverter>
#include <QTextStream>

namespace {
struct Spec { int minimum; int maximum; QStringList options; QString usage; };
const QMap<QString, Spec>& specs()
{
    static const QMap<QString, Spec> value {
        {"service list", {0, 0, {}, "service list"}},
        {"service show", {1, 1, {}, "service show <service>"}},
        {"service add", {0, 0, {"type","name","url","username","playlist","auto-login","trust-self-signed"},
            "service add --type emby|jellyfin|webdav|smb|iptv --name NAME --url URL --username USER | --playlist FILE_OR_URL"}},
        {"service update", {1,1, {"name","url","username","playlist","auto-login","trust-self-signed","private-mode"},
            "service update <service> [--name NAME] [--url URL] [--username USER] [--playlist FILE_OR_URL]"}},
        {"service login", {1,1, {}, "service login <service> [--password-env NAME]"}},
        {"service check", {1,1, {}, "service check <service>"}},
        {"service remove", {1,1, {}, "service remove <service> [--yes]"}},
        {"service logout", {1,1, {}, "service logout <service>"}},
        {"service move", {2,2, {}, "service move <service> <index>"}},
        {"webdav ls", {1,2, {}, "webdav ls <service> [path]"}},
        {"webdav stat", {2,2, {}, "webdav stat <service> <path>"}},
        {"webdav mkdir", {2,2, {"parents"}, "webdav mkdir <service> <path> [--parents]"}},
        {"webdav remove", {2,2, {}, "webdav remove <service> <path> [--yes]"}},
        {"webdav copy", {3,3, {"overwrite"}, "webdav copy <service> <source> <target> [--overwrite]"}},
        {"webdav move", {3,3, {"overwrite"}, "webdav move <service> <source> <target> [--overwrite]"}},
        {"webdav rename", {3,3, {"overwrite"}, "webdav rename <service> <source> <target> [--overwrite]"}},
        {"webdav upload", {3,3, {"overwrite"}, "webdav upload <service> <source> <target> [--overwrite]"}},
        {"webdav download", {3,3, {"overwrite","resume"}, "webdav download <service> <source> <target> [--overwrite] [--resume]"}},
        {"tssl list", {0,0, {}, "tssl list"}},
        {"tssl show", {1,1, {}, "tssl show <root digest>"}},
        {"tssl import", {1,-1, {}, "tssl import <file...>"}},
        {"tssl export", {0,-1, {"all","output"}, "tssl export <digest...> --output DIR | <digest> <file> | --all --output DIR"}},
        {"tssl remove", {1,-1, {}, "tssl remove <digest...> [--yes]"}},
        {"tssl backup", {0,0, {"target","format","local-path","service","path","s3-endpoint","s3-bucket","s3-region","s3-prefix","s3-access-key"},
            "tssl backup [--target local|webdav|s3] [--format zip|files] [--local-path DIR | --service ID --path PATH]"}},
        {"tssl restore", {0,0, {"target","local-path","service","path","s3-endpoint","s3-bucket","s3-region","s3-prefix","s3-access-key"},
            "tssl restore [--target local|webdav|s3] [--local-path DIR | --service ID --path PATH]"}},
        {"video pack", {1,-1, {"output","webdav","path","container","video","audio","quality","jobs","ffmpeg","temp","keep-local","auto-codecs","auto-target"},
            "video pack <source...> [--output DIR | --webdav SERVICE --path PATH] [--container m3u8s|m3u8sp]"}},
        {"video restore", {1,-1, {"output","format","ffmpeg","service","temp"},
            "video restore <package or folder...> [--service WEBDAV] [--output DIR] [--format original|mkv|mp4]"}},
        {"config list", {0,0, {}, "config list"}},
        {"config get", {1,1, {}, "config get <key>"}},
        {"config set", {2,2, {}, "config set <key> <value>"}},
        {"shell", {0,0, {}, "shell"}},
        {"use", {1,1, {}, "use <service>"}},
        {"pwd", {0,0, {}, "pwd"}},
        {"cd", {1,1, {}, "cd <path>"}},
        {"privacy unlock", {0,0, {}, "privacy unlock [--pin-env NAME]"}},
        {"privacy lock", {0,0, {}, "privacy lock"}},
        {"privacy set", {0,0, {"new-pin-env","confirm-pin-env"}, "privacy set [--new-pin-env NAME --confirm-pin-env NAME]"}},
        {"privacy change", {0,0, {"new-pin-env","confirm-pin-env"}, "privacy change --pin-env NAME --new-pin-env NAME --confirm-pin-env NAME"}},
        {"privacy remove", {0,0, {}, "privacy remove [--pin-env NAME] --yes"}},
        {"media libraries", {1,1, {}, "media libraries <service>"}},
        {"media ls", {1,2, {"offset","limit"}, "media ls <service> [parent ID] [--offset N --limit N]"}},
        {"media search", {2,2, {"offset","limit"}, "media search <service> <query>"}},
        {"media show", {2,2, {}, "media show <service> <item ID>"}},
        {"media channels", {1,1, {}, "media channels <IPTV service>"}},
        {"play", {1,2, {"headless","start","wait"}, "play <file or URL> | play <service> <item ID/path/channel ID> [--wait]"}},
        {"player status", {0,0, {}, "player status"}},
        {"player pause", {0,0, {}, "player pause"}},
        {"player resume", {0,0, {}, "player resume"}},
        {"player stop", {0,0, {}, "player stop"}},
        {"player seek", {1,1, {}, "player seek <seconds>"}},
        {"player volume", {1,1, {}, "player volume <0..100>"}},
        {"player speed", {1,1, {}, "player speed <0.5..5>"}},
        {"player subtitle", {1,1, {}, "player subtitle <track row or subtitle file>"}},
        {"player audio", {1,1, {}, "player audio <track row>"}},
        {"task list", {0,0, {}, "task list"}},
        {"task show", {1,1, {}, "task show <ID>"}},
        {"task pause", {1,1, {}, "task pause <ID>"}},
        {"task resume", {1,1, {}, "task resume <ID>"}},
        {"task retry", {1,1, {}, "task retry <ID>"}},
        {"task cancel", {1,1, {}, "task cancel <ID>"}},
        {"task remove", {1,1, {}, "task remove <ID> --yes"}},
        {"daemon run", {0,0, {}, "daemon run"}},
        {"daemon stop", {0,0, {}, "daemon stop"}},
        {"history", {0,0, {}, "history [--clear]"}},
        {"complete", {0,1, {}, "complete [prefix]"}},
        {"smb ls", {1,2, {}, "smb ls <service> [path]"}},
        {"smb stat", {2,2, {}, "smb stat <service> <path>"}},
        {"smb mkdir", {2,2, {"parents"}, "smb mkdir <service> <path> [--parents]"}},
        {"smb upload", {3,3, {"overwrite"}, "smb upload <service> <local> <remote>"}},
        {"smb download", {3,3, {"overwrite"}, "smb download <service> <remote> <local>"}},
        {"smb remove", {2,2, {}, "smb remove <service> <path> --yes"}},
        {"smb move", {3,3, {"overwrite"}, "smb move <service> <source> <target>"}},
        {"smb copy", {3,3, {"overwrite"}, "smb copy <service> <source> <target>"}},
        {"exit", {0,0, {}, "exit"}},
    };
    return value;
}
const QStringList globalOptions {"json","private","pin-env","password-env","s3-secret-env","s3-session-token-env",
    "s3-credentials","s3-profile","s3-role-arn","s3-role-session-name","s3-external-id","yes","data-dir","background","fail-fast","clear"};
const QSet<QString> booleanOptions {"json","private","yes","overwrite","all","keep-local","parents","resume",
    "background","fail-fast","headless","clear","wait"};

}

std::expected<Cli::Invocation, Cli::Error> Cli::parse(const QStringList& input)
{
    QCommandLineParser parser;
    QSet<QString> optionNames;
    for (const auto& name : globalOptions) optionNames.insert(name);
    for (const auto& spec : specs()) for (const auto& name : spec.options) optionNames.insert(name);
    optionNames.insert("help");
    optionNames.insert("version");
    for (const auto& name : optionNames)
        parser.addOption(QCommandLineOption(name, name, booleanOptions.contains(name) || name == "help" ||
                                            name == "version" ? QString() : QStringLiteral("value")));
    if (!parser.parse(QStringList {"vibePlayerQT"} + input))
        return std::unexpected(Error {ExitCode::Usage, parser.errorText()});
    Invocation command;
    for (const auto& name : parser.optionNames()) {
        if (booleanOptions.contains(name) || name == "help" || name == "version") command.flags.insert(name);
        else {
            if (parser.values(name).size() != 1)
                return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Option --%1 may be used only once").arg(name)});
            command.options.insert(name, parser.value(name));
        }
    }
    auto args = parser.positionalArguments();
    if (command.has("version")) { command.command = "version"; return command; }
    if (command.has("help") || args.value(0) == "help") {
        command.command = "help";
        if (args.value(0) == "help") args.removeFirst();
        command.arguments = args;
        return command;
    }
    if (args.isEmpty()) { command.command = "gui"; return command; }
    if (args.front() == "list") args = QStringList {"service", "list"} + args.mid(1);
    const auto group = args.takeFirst();
    if (group == "service" || group == "webdav" || group == "tssl" || group == "video" ||
        group == "config" || group == "privacy" || group == "media" || group == "player" ||
        group == "task" || group == "daemon" || group == "smb") {
        if (args.isEmpty()) return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Missing subcommand for %1").arg(group)});
        command.command = group + ' ' + args.takeFirst();
    } else command.command = group;
    command.arguments = args;
    if (!specs().contains(command.command))
        return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Unknown command; use help")});
    const auto spec = specs().value(command.command);
    if (args.size() < spec.minimum || (spec.maximum >= 0 && args.size() > spec.maximum))
        return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Usage: %1").arg(spec.usage)});
    for (const auto& name : parser.optionNames())
        if (!globalOptions.contains(name) && !spec.options.contains(name))
            return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Option --%1 is not valid for %2").arg(name, command.command)});
    if (command.command == "tssl export" &&
        (command.has("all") ? (!args.isEmpty() || !command.has("output")) :
         (command.has("output") ? args.isEmpty() : args.size() != 2)))
        return std::unexpected(Error {ExitCode::Usage, spec.usage});
    if (command.command == "video pack" && command.has("output") && command.has("webdav"))
        return std::unexpected(Error {ExitCode::Usage, QStringLiteral("--output and --webdav are mutually exclusive")});
    if (command.command == "tssl backup" && command.has("format") &&
        command.option("format") != "zip" && command.option("format") != "files")
        return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Backup --format must be zip or files")});
    return command;
}

std::expected<QStringList, Cli::Error> Cli::splitLine(const QString& line)
{
    QStringList words;
    QString word;
    QChar quote;
    bool started = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const auto ch = line[i];
        if (!quote.isNull()) {
            if (ch == quote && i + 1 < line.size() && line[i + 1] == quote) {
                word += ch; ++i;
            } else if (ch == quote) quote = QChar();
            else word += ch;
        } else if (ch == '"' || ch == '\'') { quote = ch; started = true; }
        else if (ch.isSpace()) {
            if (started) { words += word; word.clear(); started = false; }
        } else { word += ch; started = true; }
    }
    if (!quote.isNull()) return std::unexpected(Error {ExitCode::Usage, QStringLiteral("Unclosed quote")});
    if (started) words += word;
    return words;
}

QString Cli::help(const QString& command)
{
    QString text = QStringLiteral("vibePlayerQT CLI\n\nUsage: vibePlayerQT <command> [arguments] [options]\n");
    const QList<QPair<QString,QString>> groups {{"service","Services"}, {"webdav","WebDAV files"},
        {"tssl","TSSL packages and backups"}, {"video","Video packaging and restore"},
        {"config","Configuration"}, {"privacy","Privacy PIN"}, {"media","Media browsing"}, {"player","Playback controls"},
        {"task","Background tasks"}, {"daemon","Task daemon"}, {"smb","SMB files"}, {"","Interactive shell"}};
    for (const auto& [group, heading] : groups) {
        QString usages;
        for (auto it = specs().cbegin(); it != specs().cend(); ++it) {
            const bool inGroup = group.isEmpty() ? !it.key().contains(' ')
                                                 : it.key().startsWith(group + ' ');
            if (inGroup && (command.isEmpty() || it.key() == command || it.key().startsWith(command + ' ')))
                usages += "  " + it->usage + '\n';
        }
        if (!usages.isEmpty()) text += '\n' + heading + "\n" + usages;
    }
    text += QStringLiteral("\nGlobal options\n"
        "  --json                 One JSON result object; progress goes to stderr\n"
        "  --private              Request access to private services\n"
        "  --pin-env NAME         Read PIN from the named environment variable\n"
        "  --password-env NAME    Read password from the named environment variable\n"
        "  --s3-secret-env NAME   Read S3 secret from the named environment variable\n"
        "  --yes                  Confirm deletion in scripts\n"
        "  --data-dir DIR         Use an independent profile\n");
    text += QStringLiteral("  --background           Submit a persistent background transfer\n"
        "  --fail-fast            Stop a shell script on its first failure\n"
        "  --s3-session-token-env NAME   Read an AWS temporary session token\n"
        "  --s3-credentials env|profile|instance   AWS credential provider\n"
        "  --s3-profile NAME      Profile in ~/.aws/credentials\n"
        "  --s3-role-arn ARN      Obtain temporary credentials through STS AssumeRole\n"
        "  --s3-role-session-name NAME  STS role session name\n"
        "  --s3-external-id ID    ExternalId for STS role assumption\n");
    if (command.isEmpty() || command == "video" || command == "video pack")
        text += QStringLiteral("\nVideo pack options\n"
            "  --video copy|h264|h265|auto   --audio copy|aac   --quality high|balanced|compact\n"
            "  --jobs N   --ffmpeg FILE   --temp DIR   --keep-local\n"
            "  --auto-codecs LIST   --auto-target h264|h265\n");
    if (command.isEmpty() || command == "tssl" || command == "tssl backup" || command == "tssl restore")
        text += QStringLiteral("\nS3 backup options\n"
            "  --s3-endpoint URL   --s3-bucket NAME   --s3-region NAME\n"
            "  --s3-prefix PATH   --s3-access-key KEY\n");
    if (command.isEmpty() || command == "tssl" || command == "tssl backup" || command == "tssl restore")
        text += QStringLiteral("\nTSSL backup format\n"
            "  --format zip|files    Backup override; default is the saved tsslBackup/format (zip)\n"
            "  Restore discovers ZIP archives and legacy .tssl files together; no --format is needed.\n"
            "  ZIP compression does not encrypt the TSSL keys. Progress is written to stderr.\n");
    text += QStringLiteral("\nExamples\n"
        "  vibePlayerQT list\n"
        "  vibePlayerQT webdav ls \"My WebDAV\" /\n"
        "  vibePlayerQT service list --json\n\n"
        "list is an alias for service list. No arguments opens the desktop application.\n"
        "Shell shortcuts: ls, mkdir, upload, download (after use <service>).\n"
        "Commands wait until completion. No plaintext password/PIN arguments are accepted.\n");
    return text;
}

QStringList Cli::commandNames()
{
    return specs().keys() + QStringList {"list","ls","mkdir","upload","download","stat","remove","move","copy","rename"};
}

void Cli::writeResult(const Invocation& invocation, const Result& result)
{
    const auto rendered = renderResult(invocation, result, CliConsole::outputColumns());
    QTextStream output(stdout), errors(stderr);
    output.setEncoding(QStringConverter::Utf8);
    errors.setEncoding(QStringConverter::Utf8);
    output << rendered.standardOutput;
    errors << rendered.standardError;
    output.flush();
    errors.flush();
}
