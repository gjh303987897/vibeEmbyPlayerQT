#include "cli/CliCommand.h"
#include "cli/CliConsole.h"
#include "cli/CliEngine.h"
#include "cli/CliOutput.h"
#include "cli/CliShell.h"
#include "services/backup/S3CredentialProvider.h"
#include "services/management/ServiceOperations.h"
#include "services/management/WorkflowSettings.h"
#include "services/smb/SmbClient.h"
#include "cli/CliCheckpoint.h"
#include "services/webdav/WebDavUploadPlanner.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>

class CliTest final : public QObject {
    Q_OBJECT
private slots:
    void smbPathsStayWithinConfiguredRoot()
    {
        ServerConfig server {.id = "test-smb",.name = "NAS",.baseUrl = "smb://nas/share/root",
                             .serviceType = ServiceType::SMB};
        QVERIFY(ServiceOperations::validate(server));
        QVERIFY(!SmbClient::path(server,"../../outside"));
        QVERIFY(!SmbClient::path(server,"/file:alternate"));
        QVERIFY(!SmbClient::path(server,"/CON.txt"));
        QVERIFY(!SmbClient::path(server,"/trailing."));
        QVERIFY(!SmbClient::path(server,"/folder\\escape"));
#ifdef Q_OS_WIN
        auto path = SmbClient::path(server,QStringLiteral("/电影/../节目/影片.mp4"));
        QVERIFY(path);
        QCOMPARE(QDir::fromNativeSeparators(*path),QStringLiteral("//nas/share/root/节目/影片.mp4"));
#endif
        QTemporaryDir directory;
        QFile file(directory.filePath("content")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("checkpoint"); file.close();
        QVERIFY(!Cli::fileHash(file.fileName()).isEmpty());
        CliConsole::canceled.store(true);
        QVERIFY(Cli::fileHash(file.fileName()).isEmpty());
        CliConsole::canceled.store(false);
        QVERIFY(!SmbClient::hasLinkedAncestor(file.fileName()));
    }

    void shellExpressionsKeepQuotingAndPropagateFailures()
    {
        QTemporaryDir directory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        Cli::Engine engine(directory.path());
        Cli::Invocation defaults; defaults.command = "shell";
        Cli::Shell shell(engine,defaults);
        qputenv("CLI_EXPRESSION_TEST","dark");
        QCOMPARE(shell.execute("config set appearance/themeMode $CLI_EXPRESSION_TEST").second.code,Cli::ExitCode::Success);
        QCOMPARE(shell.execute("config get appearance/themeMode | get appearance/themeMode").second.data.toString(),QString("dark"));
        QCOMPARE(shell.execute("config set appearance/themeMode $(config get appearance/themeMode | get appearance/themeMode)").second.code,Cli::ExitCode::Success);
        QCOMPARE(shell.execute("list | count").second.data.toInt(),0);
        QCOMPARE(shell.execute("config get missing | count").second.code,Cli::ExitCode::Usage);
        QCOMPARE(shell.execute("config set appearance/themeMode '$CLI_EXPRESSION_TEST'").second.code,Cli::ExitCode::Usage);
        QCOMPARE(shell.execute("config set appearance/themeMode ${CLI_EXPRESSION_TEST}").second.code,Cli::ExitCode::Success);
        QVERIFY(shell.complete("webdav m").contains("webdav mkdir"));
        QVERIFY(!shell.history().isEmpty());
        QCOMPARE(shell.execute("history --clear").second.code,Cli::ExitCode::Success);
        QVERIFY(shell.history().isEmpty());
        qunsetenv("CLI_EXPRESSION_TEST");
    }

    void privacyLifecycleAndConfiguration()
    {
        QTemporaryDir directory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        Cli::Engine engine(directory.path());
        qputenv("CLI_PIN_NEW","1234"); qputenv("CLI_PIN_CONFIRM","1234"); qputenv("CLI_PIN_BAD","9999");
        auto execute = [&](QStringList words) { return engine.execute(*Cli::parse(words)); };
        QCOMPARE(execute({"privacy","set","--new-pin-env","CLI_PIN_NEW","--confirm-pin-env","CLI_PIN_CONFIRM"}).code,Cli::ExitCode::Success);
        QCOMPARE(execute({"privacy","set","--new-pin-env","CLI_PIN_NEW","--confirm-pin-env","CLI_PIN_CONFIRM"}).code,Cli::ExitCode::Conflict);
        QCOMPARE(execute({"privacy","change","--pin-env","CLI_PIN_BAD","--new-pin-env","CLI_PIN_BAD","--confirm-pin-env","CLI_PIN_BAD"}).code,Cli::ExitCode::Authentication);
        QCOMPARE(execute({"privacy","change","--pin-env","CLI_PIN_NEW","--new-pin-env","CLI_PIN_BAD","--confirm-pin-env","CLI_PIN_BAD"}).code,Cli::ExitCode::Success);
        QCOMPARE(execute({"privacy","remove","--pin-env","CLI_PIN_BAD","--yes"}).code,Cli::ExitCode::Success);
        QCOMPARE(execute({"privacy","unlock","--pin-env","CLI_PIN_BAD"}).code,Cli::ExitCode::Authentication);
        QCOMPARE(execute({"config","set","player/speed","2"}).code,Cli::ExitCode::Success);
        QCOMPARE(execute({"config","set","player/speed","nan"}).code,Cli::ExitCode::Usage);
        QCOMPARE(execute({"config","set","appearance/serviceCardSizePercent","200"}).code,Cli::ExitCode::Usage);
        QCOMPARE(execute({"config","set","player/subtitleFile",directory.filePath("missing.srt")}).code,Cli::ExitCode::Usage);
        for (const auto& key : {"CLI_PIN_NEW","CLI_PIN_CONFIRM","CLI_PIN_BAD"}) qunsetenv(key);
    }

    void sharedS3ProfileLoadsTemporaryCredentials()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("credentials")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[test]\naws_access_key_id=profile-access\naws_secret_access_key=profile-secret\naws_session_token=profile-token\n"); file.close();
        const auto previous = qgetenv("AWS_SHARED_CREDENTIALS_FILE");
        const bool present = qEnvironmentVariableIsSet("AWS_SHARED_CREDENTIALS_FILE");
        qputenv("AWS_SHARED_CREDENTIALS_FILE",QFileInfo(file).absoluteFilePath().toUtf8());
        const auto credentials = S3CredentialProvider::load("profile","test");
        if (present) qputenv("AWS_SHARED_CREDENTIALS_FILE",previous); else qunsetenv("AWS_SHARED_CREDENTIALS_FILE");
        QVERIFY(credentials); QCOMPARE(credentials->sessionToken,QString("profile-token"));
        QCOMPARE(credentials->accessKey,QString("profile-access"));
        QVERIFY(!S3CredentialProvider::load("unsupported"));
    }

    void readableTablesPreserveValuesAndAlignChinese()
    {
        Cli::Invocation invocation;
        invocation.command = "service list";
        const QString id(64,'a');
        const QString address = "https://example.invalid/" + QString(100,'b');
        const QJsonArray cards {
            QJsonObject {{"id",id}, {"name",QStringLiteral("家庭影片")}, {"type","webdav"},
                {"address",address}, {"hasSavedSession",false}, {"hasSavedCredentials",true}},
            QJsonObject {{"id","second"}, {"name","Home"}, {"type","emby"},
                {"address","https://example.invalid"}, {"hasSavedSession",true}, {"hasSavedCredentials",false}},
        };
        const auto result = Cli::renderResult(invocation,{Cli::ExitCode::Success,cards,{}},80);
        QVERIFY(result.standardError.isEmpty());
        QVERIFY(result.standardOutput.startsWith("Services (2)\n"));
        QVERIFY(!result.standardOutput.contains('\t'));
        QVERIFY(!result.standardOutput.contains("online",Qt::CaseInsensitive));
        QVERIFY(result.standardOutput.contains(id));
        QVERIFY(result.standardOutput.contains("Saved credentials"));
        const auto lines = result.standardOutput.split('\n');
        const auto chinese = *std::find_if(lines.begin(),lines.end(),[](const QString& line) { return line.contains(QStringLiteral("家庭影片")); });
        const auto english = *std::find_if(lines.begin(),lines.end(),[](const QString& line) { return line.startsWith("Home "); });
        // Four CJK characters occupy eight cells; their UTF-16 length is four.
        QCOMPARE(chinese.indexOf("webdav") + 4,english.indexOf("emby"));
        QString recovered;
        bool capturing = false;
        for (const auto& line : lines) {
            if (line.startsWith("  Address : ")) { capturing = true; recovered += line.mid(12); }
            else if (capturing && line.startsWith(QString(12,' '))) recovered += line.mid(12);
            else if (capturing) break;
        }
        QCOMPARE(recovered,address);
        for (const auto& line : lines) QVERIFY(line.size() <= 80);
    }

    void directoryTypesSizesAndBatchFailuresAreReadable()
    {
        Cli::Invocation invocation;
        invocation.command = "webdav ls";
        const QJsonArray rows {
            QJsonObject {{"name",QStringLiteral("电影")}, {"directory",true}, {"size",4096}},
            QJsonObject {{"name","movie.mkv"}, {"directory",false}, {"size",1572864}},
        };
        const auto directory = Cli::renderResult(invocation,{Cli::ExitCode::Success,rows,{}}).standardOutput;
        QVERIFY(directory.contains("Folder")); QVERIFY(directory.contains("File"));
        QVERIFY(directory.contains("1.50 MiB")); QVERIFY(!directory.contains("4096"));
        invocation.command = "video pack";
        const QJsonArray items {QJsonObject {{"output","D:/good.m3u8sp"}},
            QJsonObject {{"source","D:/bad.mp4"}, {"error","FFmpeg failed"}}};
        const auto batch = Cli::renderResult(invocation,{Cli::ExitCode::Partial,
            QJsonObject {{"succeeded",1}, {"failed",1}, {"items",items}},"FFmpeg failed"},80);
        QVERIFY(batch.standardOutput.contains("Succeeded : 1"));
        QVERIFY(batch.standardOutput.contains("Failed    : 1"));
        QVERIFY(batch.standardOutput.contains("Items (2)"));
        QVERIFY(batch.standardOutput.contains("FFmpeg failed")); // Union of every row's fields.
        QVERIFY(batch.standardOutput.contains("D:/good.m3u8sp"));
        QVERIFY(!batch.standardOutput.contains("[{"));
        QVERIFY(batch.standardError.contains("[6: partial failure]"));
    }

    void narrowOutputAndUnicodeAreSafe()
    {
        Cli::Invocation invocation;
        invocation.command = "config get";
        const QString path = QStringLiteral("D:/中文/e\u0301/📺/") + QString(60,'x');
        const auto narrow = Cli::renderResult(invocation,{Cli::ExitCode::Success,
            QJsonObject {{"m3u8s/temporaryDirectory",path}},{}},24);
        for (const auto& line : narrow.standardOutput.split('\n')) {
            QVERIFY(line.size() <= 24);
            QVERIFY(line.isEmpty() || !line.back().isHighSurrogate());
            QVERIFY(line.isEmpty() || !line.front().isLowSurrogate());
            QVERIFY(!line.startsWith(QChar(0x0301))); // Keep combining accents with their base.
        }
        invocation.command = "service list";
        const auto controls = Cli::renderResult(invocation,{Cli::ExitCode::Success,
            QJsonArray {QJsonObject {{"name","bad\x1b[31m\nname"}, {"type","webdav"}}},{}},24);
        QVERIFY(!controls.standardOutput.contains(QChar(0x1b)));
        QVERIFY(!controls.standardOutput.contains("\nname"));
        invocation.command = "webdav ls";
        const auto heading = Cli::renderResult(invocation,{Cli::ExitCode::Success,QJsonArray {},{}},20);
        for (const auto& line : heading.standardOutput.split('\n')) QVERIFY(line.size() <= 20);
    }

    void jsonRemainsOneObjectWithOriginalTypes()
    {
        Cli::Invocation invocation;
        invocation.command = "webdav stat"; invocation.flags.insert("json");
        const QJsonObject data {{"name",QStringLiteral("中文影片.mkv")}, {"directory",false}, {"size",1572864}};
        const auto rendered = Cli::renderResult(invocation,{Cli::ExitCode::Success,data,{}},20);
        QCOMPARE(rendered.standardOutput.count('\n'),1);
        const auto envelope = QJsonDocument::fromJson(rendered.standardOutput.toUtf8()).object();
        QCOMPARE(envelope.value("schemaVersion").toInt(),1);
        QCOMPARE(envelope.value("command").toString(),invocation.command);
        QCOMPARE(envelope.value("data").toObject(),data);
        QCOMPARE(envelope.value("ok").toBool(),true);
        QVERIFY(envelope.value("error").isNull());
        QVERIFY(rendered.standardError.isEmpty());
        const auto failure = Cli::renderResult(invocation,{Cli::ExitCode::Conflict,{},"Already exists"});
        const auto error = QJsonDocument::fromJson(failure.standardOutput.toUtf8()).object();
        QCOMPARE(error.value("error").toObject().value("code").toInt(),5);
        QVERIFY(error.value("data").isNull());
        QCOMPARE(failure.standardError,QString("Already exists\n"));
    }

    void parserAndQuoting()
    {
        auto alias = Cli::parse({"list","--json"});
        QVERIFY(alias); QCOMPARE(alias->command, QString("service list"));
        QVERIFY(Cli::parse({"webdav","download","one","a","b","--overwrite"}));
        QVERIFY(!Cli::parse({"video","pack","a","--jobs","1","--jobs","2"}));
        QVERIFY(!Cli::parse({"service","login","one","--password","secret"}));
        QVERIFY(!Cli::parse({"tssl","export","--all"}));
        QVERIFY(!Cli::parse({"video","pack","a","--output","b","--webdav","one"}));
        auto words = Cli::splitLine(QStringLiteral(R"(upload "D:\中文 路径\影片.mp4" '/电影/影片.mp4')"));
        QVERIFY(words);
        QCOMPARE(*words, (QStringList {"upload",QStringLiteral("D:\\中文 路径\\影片.mp4"),QStringLiteral("/电影/影片.mp4")}));
        QVERIFY(!Cli::splitLine("upload 'unfinished"));
        QCOMPARE(*Cli::splitLine(QStringLiteral(R"("\\server\share\" "D:\folder\")")),
                 (QStringList {QStringLiteral("\\\\server\\share\\"),QStringLiteral("D:\\folder\\")}));
        auto literal = Cli::splitLine("ls $HOME | $(echo test)");
        QVERIFY(literal); QVERIFY(literal->contains("$HOME")); QVERIFY(literal->contains("|"));
    }

    void readOnlyDoesNotCreateOrMigrate()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("database.sqlite3");
        {
            SessionRepository repository("cli-test-empty", path, SessionRepository::OpenMode::ReadOnly);
            auto cards = repository.loadServiceCards(false);
            QVERIFY(cards); QVERIFY(cards->empty()); QVERIFY(!QFileInfo::exists(path));
            QVERIFY(!repository.initialize());
        }
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE","cli-old-schema");
            database.setDatabaseName(path); QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec("CREATE TABLE old_schema (value TEXT)"));
        }
        QSqlDatabase::removeDatabase("cli-old-schema");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto original = file.readAll(); file.close();
        {
            SessionRepository repository("cli-test-old", path, SessionRepository::OpenMode::ReadOnly);
            QVERIFY(!repository.loadServiceCards(false));
        }
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
    }

    void privacyCannotBeBypassedById()
    {
        QTemporaryDir directory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        QString id;
        {
            SessionRepository repository("cli-private",directory.filePath("vibeplayer.sqlite3"));
            QVERIFY(repository.initialize());
            const auto salt = QString("test-salt");
            repository.setPrivacyPinHash(salt,QString::fromLatin1(QCryptographicHash::hash((salt + ":1234").toUtf8(),QCryptographicHash::Sha256).toHex()));
            ServerConfig server;
            server.name = "Hidden"; server.baseUrl = "https://example.invalid/dav/"; server.username = "user";
            server.serviceType = ServiceType::WebDAV; server.privateMode = true;
            server.id = ServiceOperations::cardId(server.baseUrl,server.serviceType,server.username); id = server.id;
            QVERIFY(ServiceOperations::save(repository,server));
        }
        QVERIFY(QFileInfo::exists(directory.filePath("vibePlayerQT/vibePlayerQT.ini")));
        Cli::Engine engine(directory.path());
        auto show = Cli::parse({"service","show",id}); QVERIFY(show);
        QCOMPARE(engine.execute(*show).code,Cli::ExitCode::NotFound);
        qputenv("CLI_TEST_PIN","wrong");
        auto unlocked = Cli::parse({"service","show",id,"--private","--pin-env","CLI_TEST_PIN"}); QVERIFY(unlocked);
        QCOMPARE(engine.execute(*unlocked).code,Cli::ExitCode::Authentication);
        qputenv("CLI_TEST_PIN","1234");
        QCOMPARE(engine.execute(*unlocked).code,Cli::ExitCode::Success);
        QCOMPARE(engine.execute(*show).code,Cli::ExitCode::NotFound);
        auto unlock = Cli::parse({"privacy","unlock","--pin-env","CLI_TEST_PIN"}); QVERIFY(unlock);
        QCOMPARE(engine.execute(*unlock).code,Cli::ExitCode::Success);
        QCOMPARE(engine.execute(*show).code,Cli::ExitCode::Success);
        QCOMPARE(engine.execute(*Cli::parse({"privacy","lock"})).code,Cli::ExitCode::Success);
        QCOMPARE(engine.execute(*show).code,Cli::ExitCode::NotFound);
        qunsetenv("CLI_TEST_PIN");
    }

    void uploadPlanPreservesHierarchyAndEscapes()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("中文/二层")));
        QFile file(directory.filePath(QStringLiteral("中文/二层/a #%.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("payload"); file.close();
        std::atomic_bool canceled {false};
        auto plan = WebDavUploadPlanner::plan(directory.path(),QUrl("http://example.invalid/dav/a%252Fb/"),canceled);
        QVERIFY(plan); QCOMPARE(plan->size(),4);
        QVERIFY((*plan)[0].directory); QVERIFY((*plan)[1].directory); QVERIFY((*plan)[2].directory);
        QCOMPARE(plan->back().remoteUrl.path(QUrl::FullyDecoded),QStringLiteral("/dav/a%2Fb/中文/二层/a #%.txt"));
        QVERIFY(plan->back().remoteUrl.toEncoded().contains("/a%252Fb/"));
        canceled.store(true);
        QVERIFY(!WebDavUploadPlanner::plan(directory.path(),QUrl("http://example.invalid/"),canceled));
    }

    void validatesConfiguration()
    {
        QVariant jobs("0"); QVERIFY(!WorkflowSettings::validate("m3u8s/parallelJobs",jobs));
        QVariant format("mp4"); QVERIFY(WorkflowSettings::validate("encryptedHlsRestore/format",format));
        QVariant secret("x"); QVERIFY(!WorkflowSettings::validate("tsslBackup/s3Secret",secret));
        QVariant boolValue("false"); QVERIFY(WorkflowSettings::validate("m3u8s/keepSuccessfulLocal",boolValue));
        QCOMPARE(boolValue.toBool(),false);
    }

    void databaseBusyIsBounded()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("vibeplayer.sqlite3");
        SessionRepository repository("cli-lock-writer",path);
        QVERIFY(repository.initialize());
        auto lock = QSqlDatabase::addDatabase("QSQLITE","cli-lock");
        lock.setDatabaseName(path); QVERIFY(lock.open());
        {
            QSqlQuery query(lock); QVERIFY(query.exec("BEGIN EXCLUSIVE"));
            SessionRepository reader("cli-lock-reader",path,SessionRepository::OpenMode::ReadOnly);
            QElapsedTimer elapsed; elapsed.start();
            QVERIFY(!reader.loadServiceCards(false));
            QVERIFY(elapsed.elapsed() < 6000);
            QVERIFY(query.exec("ROLLBACK"));
        }
        lock.close(); lock = {}; QSqlDatabase::removeDatabase("cli-lock");
    }
};
QTEST_GUILESS_MAIN(CliTest)
#include "CliTest.moc"
