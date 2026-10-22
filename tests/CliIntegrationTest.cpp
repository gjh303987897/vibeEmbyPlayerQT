#include "cli/CliCommand.h"
#include "cli/CliConsole.h"
#include "cli/CliEngine.h"
#include "services/credentials/CredentialStore.h"
#include "services/backup/S3CredentialProvider.h"
#include "services/encryptedhls/EncryptedHlsPackager.h"
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <QXmlStreamWriter>
#include <memory>
#include <QLockFile>
#include <QElapsedTimer>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
bool write(const QString& path, const QByteArray& body)
{ QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(body) == body.size(); }
QByteArray read(const QString& path)
{ QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }

struct Response { int code; QJsonObject json; QByteArray output; QByteArray errors; };
Response run(QStringList arguments, const QString& profile, const QByteArray& input = {})
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("CLI_TEST_PASSWORD","test-password");
    environment.insert("CLI_TEST_SECRET","test-s3-secret");
    environment.insert("CLI_TEST_S3_TOKEN","test-session-token-s3");
    environment.insert("CLI_TEST_PIN","1234");
    environment.insert("CLI_TEST_NEW_PIN","5678");
    environment.remove("VIBEPLAYER_LOG_FILE");
    process.setProcessEnvironment(environment);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool expired = false;
    QObject::connect(&process,&QProcess::finished,&loop,&QEventLoop::quit);
    QObject::connect(&timeout,&QTimer::timeout,&loop,[&] { expired = true; process.kill(); loop.quit(); });
    process.start(QString::fromUtf8(CLI_EXECUTABLE),arguments + QStringList {"--json","--data-dir",profile});
    if (!process.waitForStarted(5000)) {
        QTest::qFail(qPrintable(process.errorString()),__FILE__,__LINE__);
        return {-1,{},{},{}};
    }
    if (!input.isEmpty()) process.write(input);
    process.closeWriteChannel();
    timeout.start(60000);
    if (process.state() != QProcess::NotRunning) loop.exec();
    if (expired) process.waitForFinished(3000);
    const auto output = process.readAllStandardOutput();
    const auto errors = process.readAllStandardError();
    if (expired) QTest::qFail(("CLI timed out: " + errors).constData(),__FILE__,__LINE__);
    return {process.exitCode(),QJsonDocument::fromJson(output).object(),output,errors};
}

struct Request { QByteArray method; QUrl url; QMap<QByteArray,QByteArray> headers; QByteArray body; };
class Endpoint final : public QTcpServer {
public:
    QSet<QString> directories {"/dav/","/bucket/"};
    QMap<QString,QByteArray> files;
    QList<Request> requests;
    bool failAuthentication = false;
    bool mediaFixture = false;
    bool slowGet = false;
    bool invalidRange = false;
    QByteArray mediaFile;
    bool racePut = false;
    bool stall = false;
    QSet<QByteArray> stallMethods;
    bool malicious = false;
    int failCode = 0;
    QMap<QByteArray,int> methodErrors;

    Endpoint()
    {
        connect(this,&QTcpServer::newConnection,this,[this] {
            while (auto* socket = nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,socket,[this,socket,buffer] {
                    *buffer += socket->readAll();
                    const auto end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const auto lines = buffer->left(end).split('\n');
                    const auto first = lines.front().trimmed().split(' ');
                    Request request;
                    request.method = first.value(0);
                    request.url = QUrl::fromEncoded(QByteArray("http://localhost") + first.value(1));
                    for (const auto& line : lines.mid(1)) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0) request.headers[line.left(colon).trimmed().toLower()] = line.mid(colon + 1).trimmed();
                    }
                    const auto length = request.headers.value("content-length").toLongLong();
                    if (buffer->size() < end + 4 + length) return;
                    request.body = buffer->mid(end + 4,length); buffer->clear();
                    requests += request;
                    handle(socket,request);
                });
            }
        });
    }
    QString root() const { return QString("http://127.0.0.1:%1/dav/").arg(serverPort()); }
    QString s3() const { return QString("http://localhost:%1").arg(serverPort()); }
    static void respond(QTcpSocket* socket,int status,const QByteArray& body = {})
    {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\nContent-Type: application/xml\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    QByteArray listing(const QString& path,bool children)
    {
        QByteArray body;
        QXmlStreamWriter xml(&body);
        xml.writeStartDocument();
        xml.writeStartElement("D:multistatus"); xml.writeNamespace("DAV:","D");
        const auto entry = [&](const QString& resource,bool directory) {
            xml.writeStartElement("D:response");
            QUrl url; url.setPath(resource);
            xml.writeTextElement("D:href",QString::fromLatin1(url.toEncoded()));
            xml.writeStartElement("D:propstat"); xml.writeStartElement("D:prop");
            xml.writeTextElement("D:displayname",resource.split('/',Qt::SkipEmptyParts).last());
            xml.writeStartElement("D:resourcetype");
            if (directory) xml.writeEmptyElement("D:collection");
            xml.writeEndElement();
            if (!directory) {
                xml.writeTextElement("D:getcontentlength",QString::number(files.value(resource).size()));
                xml.writeTextElement("D:getetag",QStringLiteral("\"%1\"").arg(QString::fromLatin1(
                    QCryptographicHash::hash(files.value(resource),QCryptographicHash::Sha256).toHex())));
            }
            xml.writeEndElement(); xml.writeTextElement("D:status","HTTP/1.1 200 OK");
            xml.writeEndElement(); xml.writeEndElement();
        };
        entry(path,directories.contains(path));
        if (children) {
            for (const auto& directory : directories)
                if (directory != path && directory.startsWith(path) && !directory.mid(path.size()).chopped(1).contains('/'))
                    entry(directory,true);
            for (auto it = files.begin(); it != files.end(); ++it)
                if (it.key().startsWith(path) && !it.key().mid(path.size()).contains('/')) entry(it.key(),false);
            if (malicious) {
                xml.writeStartElement("D:response");
                xml.writeTextElement("D:href","http://127.0.0.1:1/steal/");
                xml.writeStartElement("D:propstat"); xml.writeStartElement("D:prop");
                xml.writeStartElement("D:resourcetype"); xml.writeEmptyElement("D:collection");
                xml.writeEndElement(); xml.writeEndElement();
                xml.writeTextElement("D:status","HTTP/1.1 200 OK"); xml.writeEndElement(); xml.writeEndElement();
            }
        }
        xml.writeEndElement(); xml.writeEndDocument();
        return body;
    }
    void handle(QTcpSocket* socket,const Request& request)
    {
        if (stall || stallMethods.contains(request.method)) return;
        if (failCode) { respond(socket,failCode); return; }
        if (methodErrors.contains(request.method)) { respond(socket,methodErrors.value(request.method)); return; }
        if (failAuthentication) { respond(socket,401); return; }
        auto path = request.url.path();
        if (request.method == "POST" && request.body.contains("Action=AssumeRole")) {
            respond(socket,200,R"(<AssumeRoleResponse><AssumeRoleResult><Credentials><AccessKeyId>temporary-access</AccessKeyId><SecretAccessKey>temporary-secret</SecretAccessKey><SessionToken>temporary-token</SessionToken><Expiration>2099-01-01T00:00:00Z</Expiration></Credentials></AssumeRoleResult></AssumeRoleResponse>)");
            return;
        }
        if (path.endsWith("/AuthenticateByName")) {
            respond(socket,200,R"({"User":{"Id":"user-id","Name":"user"},"AccessToken":"test-session-token"})"); return;
        }
        if (path.endsWith("/Views") || path.endsWith("/UserViews")) {
            respond(socket,200,mediaFixture ? R"({"Items":[{"Id":"library","Name":"Movies","CollectionType":"movies"}]})" : R"({"Items":[]})"); return;
        }
        if (mediaFixture && path.endsWith("/Items/movie")) {
            respond(socket,200,R"({"Id":"movie","Name":"Example movie","Type":"Movie","RunTimeTicks":10000000})"); return;
        }
        if (mediaFixture && path.endsWith("/PlaybackInfo")) {
            respond(socket,200,R"({"PlaySessionId":"play-session","MediaSources":[{"Id":"media-source"}]})"); return;
        }
        if (mediaFixture && path.endsWith("/Videos/movie/stream")) { respond(socket,200,mediaFile); return; }
        if (mediaFixture && path.contains("/Sessions/Playing")) { respond(socket,204); return; }
        if (mediaFixture && path.endsWith("/Items")) {
            respond(socket,200,R"({"Items":[{"Id":"movie","Name":"Example movie","Type":"Movie"}]})"); return;
        }
        if (request.method == "GET" && QUrlQuery(request.url).hasQueryItem("list-type")) {
            QByteArray body; QXmlStreamWriter xml(&body); xml.writeStartElement("ListBucketResult");
            xml.writeTextElement("IsTruncated","false");
            for (auto it = files.begin(); it != files.end(); ++it) if (it.key().startsWith("/bucket/")) {
                xml.writeStartElement("Contents"); xml.writeTextElement("Key",it.key().mid(8)); xml.writeEndElement();
            }
            xml.writeEndElement(); respond(socket,200,body); return;
        }
        if (request.method == "PROPFIND") {
            if (!files.contains(path) && !directories.contains(path) && directories.contains(path + '/')) path += '/';
            if (!files.contains(path) && !directories.contains(path)) { respond(socket,404); return; }
            respond(socket,207,listing(path,request.headers.value("depth") == "1")); return;
        }
        if (request.method == "MKCOL") {
            if (!path.endsWith('/')) path += '/';
            if (directories.contains(path) || files.contains(path.chopped(1))) { respond(socket,405); return; }
            const auto parent = path.chopped(1).left(path.chopped(1).lastIndexOf('/') + 1);
            if (!directories.contains(parent)) { respond(socket,409); return; }
            directories.insert(path); respond(socket,201); return;
        }
        if (request.method == "PUT") {
            if (racePut && request.headers.value("if-none-match") == "*") files[path] = "race-original";
            if (files.contains(path) && request.headers.value("if-none-match") == "*") { respond(socket,412); return; }
            if (!path.startsWith("/bucket/") && !directories.contains(path.left(path.lastIndexOf('/') + 1))) { respond(socket,409); return; }
            files[path] = request.body; respond(socket,201); return;
        }
        if (request.method == "GET") {
            if (!files.contains(path)) { respond(socket,404); return; }
            const auto body = files.value(path);
            const auto etag = "\"" + QCryptographicHash::hash(body,QCryptographicHash::Sha256).toHex() + "\"";
            const auto range = request.headers.value("range");
            if (range.startsWith("bytes=") && request.headers.value("if-range") == etag) {
                const auto offset = range.mid(6).chopped(1).toLongLong();
                const auto tail = body.mid(offset);
                socket->write("HTTP/1.1 206 Partial Content\r\nContent-Length: " + QByteArray::number(tail.size()) +
                    "\r\nContent-Range: bytes " + QByteArray::number(offset) + '-' + QByteArray::number(body.size() - 1) + '/' +
                    QByteArray::number(body.size() + (invalidRange ? 1 : 0)) + "\r\nETag: " + etag + "\r\nConnection: close\r\n\r\n" + tail);
                socket->disconnectFromHost(); return;
            }
            if (slowGet) {
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                    "\r\nETag: " + etag + "\r\n\r\n" + body.left(body.size() / 2));
                socket->flush(); return;
            }
            respond(socket,200,files.value(path)); return;
        }
        if (request.method == "DELETE") {
            if (!files.contains(path)) { respond(socket,404); return; }
            files.remove(path); respond(socket,204); return;
        }
        if (request.method == "COPY" || request.method == "MOVE") {
            const auto target = QUrl::fromEncoded(request.headers.value("destination")).path();
            if (!files.contains(path)) { respond(socket,404); return; }
            if (files.contains(target) && request.headers.value("overwrite") == "F") { respond(socket,412); return; }
            files[target] = files[path];
            if (request.method == "MOVE") files.remove(path);
            respond(socket,201); return;
        }
        respond(socket,405);
    }
};
QString addService(const QString& profile,Endpoint& endpoint,QString name = "DAV",QString type = "webdav")
{
    const auto response = run({"service","add","--type",type,"--name",name,"--url",endpoint.root(),"--username","user"},profile);
    if (response.code) QTest::qFail(response.errors.constData(),__FILE__,__LINE__);
    return response.json.value("data").toObject().value("id").toString();
}
}

class CliIntegrationTest final : public QObject {
    Q_OBJECT
private slots:
    void stsRoleCredentialsUseSourceTokenAndBoundedCancellation()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const auto url = QUrl(QString("http://127.0.0.1:%1/").arg(endpoint.serverPort()));
        auto credentials = S3CredentialProvider::assumeRole({"source-access","source-secret","source-token"},
            "arn:aws:iam::123456789012:role/Backup","us-east-1","test+session","external+id",nullptr,url);
        QVERIFY(credentials); QCOMPARE(credentials->sessionToken,QString("temporary-token"));
        QVERIFY(endpoint.requests.back().headers.value("authorization").contains("/sts/aws4_request"));
        QCOMPARE(endpoint.requests.back().headers.value("x-amz-security-token"),QByteArray("source-token"));
        QVERIFY(endpoint.requests.back().body.contains("test%2Bsession"));
        endpoint.stall = true;
        std::atomic_bool canceled = false;
        QTimer::singleShot(100,&endpoint,[&] { canceled.store(true); });
        QVERIFY(!S3CredentialProvider::assumeRole({"source-access","source-secret",{}},
            "arn:aws:iam::123456789012:role/Backup","us-east-1","test-session",{},&canceled,url));
    }

    void extendedFileCommandsAndScriptExitCodes()
    {
        QTemporaryDir directory; Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const auto id = addService(directory.path(),endpoint);
        const auto command = [&](QStringList words) { return run(words + QStringList {"--password-env","CLI_TEST_PASSWORD"},directory.path()); };
        QCOMPARE(command({"webdav","mkdir",id,"/a/b/c","--parents"}).code,0);
        QVERIFY(endpoint.directories.contains("/dav/a/b/c/"));
        endpoint.files["/dav/original.txt"] = "original";
        endpoint.files["/dav/existing.txt"] = "existing";
        QCOMPARE(command({"webdav","copy",id,"/original.txt","/existing.txt"}).code,5);
        QCOMPARE(endpoint.files["/dav/existing.txt"],QByteArray("existing"));
        QCOMPARE(command({"webdav","copy",id,"/original.txt","/copy.txt"}).code,0);
        QCOMPARE(command({"webdav","rename",id,"/copy.txt","/renamed.txt"}).code,0);
        QVERIFY(!endpoint.files.contains("/dav/copy.txt"));
        QCOMPARE(command({"webdav","remove",id,"/renamed.txt"}).code,2);
        QCOMPARE(command({"webdav","remove",id,"/renamed.txt","--yes"}).code,0);
        QCOMPARE(command({"webdav","remove",id,"/","--yes"}).code,2);
        const auto target = directory.filePath("download.txt"); QVERIFY(write(target,"old"));
        QCOMPARE(command({"webdav","download",id,"/original.txt",target,"--overwrite"}).code,0);
        QCOMPARE(read(target),QByteArray("original"));
        endpoint.methodErrors["GET"] = 401;
        QCOMPARE(command({"webdav","download",id,"/original.txt",target,"--overwrite"}).code,3);
        QCOMPARE(read(target),QByteArray("original"));
        const auto piped = run({"shell","--fail-fast"},directory.path(),"service show missing\nlist\n");
        QCOMPARE(piped.code,4); QCOMPARE(piped.output.count('\n'),1);
        const auto continued = run({"shell"},directory.path(),"service show missing\nlist | count\nexit\n");
        QCOMPARE(continued.code,4); QCOMPARE(continued.output.count('\n'),2);
    }

    void mediaAndRemotePlaylist()
    {
        QTemporaryDir directory; Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost)); endpoint.mediaFixture = true;
        for (const auto& type : {"emby","jellyfin"}) {
            const auto id = addService(directory.path(),endpoint,type,type);
            QCOMPARE(run({"media","libraries",id},directory.path()).code,3);
            QCOMPARE(run({"service","login",id,"--password-env","CLI_TEST_PASSWORD"},directory.path()).code,0);
            auto libraries = run({"media","libraries",id},directory.path());
            QCOMPARE(libraries.code,0); QCOMPARE(libraries.json["data"].toArray().size(),1);
            QCOMPARE(run({"media","ls",id,"library"},directory.path()).code,0);
            QCOMPARE(run({"media","search",id,"Example"},directory.path()).code,0);
            QCOMPARE(run({"media","show",id,"movie"},directory.path()).json["data"].toObject()["name"].toString(),QString("Example movie"));
            QVERIFY(!libraries.output.contains("test-session-token"));
            QCOMPARE(run({"service","logout",id},directory.path()).code,0);
            QCOMPARE(run({"media","libraries",id},directory.path()).code,3);
        }
        endpoint.files["/dav/playlist.m3u"] = "#EXTM3U\n#EXTINF:-1 group-title=\"News\",Test channel\nstream.ts\n";
        auto playlist = run({"service","add","--type","iptv","--name","Remote IPTV","--playlist",endpoint.root() + "playlist.m3u"},directory.path());
        QCOMPARE(playlist.code,0);
        const auto id = playlist.json["data"].toObject()["id"].toString();
        QCOMPARE(run({"media","channels",id},directory.path()).json["data"].toArray().size(),1);
        QCOMPARE(run({"service","update",id,"--name","Renamed IPTV"},directory.path()).code,0);
        SessionRepository stored("cli-remote-iptv",directory.filePath("vibeplayer.sqlite3"),SessionRepository::OpenMode::ReadOnly);
        const auto channels = stored.loadIptvChannels(id); QVERIFY(channels);
        QCOMPARE(channels->front().streamUrl,endpoint.root() + "stream.ts");
        const auto metadata = stored.loadIptvPlaylist(id); QVERIFY(metadata); QVERIFY(*metadata);
        QCOMPARE((**metadata).sourceType,QString("RemoteURL"));
        QCOMPARE((**metadata).name,QString("Renamed IPTV"));
        QCOMPARE(run({"service","check",id},directory.path()).code,0);
    }

    void backgroundTransferSurvivesWorkerRestart_data()
    {
        QTest::addColumn<bool>("invalidReply");
        QTest::newRow("valid-range") << false;
        QTest::newRow("invalid-range-then-retry") << true;
    }

    void backgroundTransferSurvivesWorkerRestart()
    {
        QFETCH(bool,invalidReply);
        if (!CredentialStore::isAvailable()) QSKIP("Persistent credentials require the platform secure store");
        QTemporaryDir directory; Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const auto id = addService(directory.path(),endpoint);
        endpoint.files["/dav/background.txt"] = QByteArray(100000,'B'); endpoint.slowGet = true;
        QString taskId;
        const auto cleanup = qScopeGuard([&] {
            if (!taskId.isEmpty()) {
                run({"task","cancel",taskId},directory.path());
                QTest::qWait(100);
                run({"task","remove",taskId,"--yes"},directory.path());
                static_cast<void>(CredentialStore::deleteSecret("CliTask/" + taskId));
            }
            run({"daemon","stop"},directory.path()); QTest::qWait(400);
        });
        auto submitted = run({"webdav","download",id,"/background.txt",directory.filePath("background.txt"),
            "--background","--password-env","CLI_TEST_PASSWORD"},directory.path());
        QCOMPARE(submitted.code,0); taskId = submitted.json["data"].toObject()["id"].toString(); QVERIFY(!taskId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(std::ranges::any_of(endpoint.requests,[](const Request& request) { return request.method == "GET"; }),5000);
        QTest::qWait(200);
        QCOMPARE(run({"task","pause",taskId},directory.path()).code,0);
        QCOMPARE(run({"task","show",taskId},directory.path()).json["data"].toObject()["state"].toString(),QString("paused"));
        QCOMPARE(run({"daemon","stop"},directory.path()).code,0); QTest::qWait(600);
        auto interrupted = run({"task","show",taskId},directory.path());
        QCOMPARE(interrupted.code,0); QCOMPARE(interrupted.json["data"].toObject()["state"].toString(),QString("interrupted"));
        endpoint.slowGet = false;
        endpoint.invalidRange = invalidReply;
        QCOMPARE(run({"task","resume",taskId},directory.path()).code,0);
        if (invalidReply) {
            QTRY_COMPARE_WITH_TIMEOUT(run({"task","show",taskId},directory.path()).json["data"].toObject()["state"].toString(),
                                      QString("failed"),5000);
            QVERIFY(!QFileInfo::exists(directory.filePath("background.txt")));
            endpoint.invalidRange = false;
            QCOMPARE(run({"task","retry",taskId},directory.path()).code,0);
        }
        QTRY_COMPARE_WITH_TIMEOUT(read(directory.filePath("background.txt")),QByteArray(100000,'B'),5000);
        QVERIFY(std::ranges::any_of(endpoint.requests,[](const Request& request) {
            return request.method == "GET" && request.headers.value("range") == "bytes=50000-";
        }));
        QCOMPARE(run({"task","show",taskId},directory.path()).json["data"].toObject()["state"].toString(),QString("completed"));
        QCOMPARE(run({"task","cancel",taskId},directory.path()).code,5);
        QCOMPARE(run({"task","show",taskId},directory.path()).json["data"].toObject()["state"].toString(),QString("completed"));
        QVERIFY(!read(directory.filePath("cli_tasks.sqlite3")).contains("test-password"));
    }

    void entryAndServices()
    {
        QSettings native(QSettings::NativeFormat,QSettings::UserScope,"vibePlayerQT","vibePlayerQT");
        const auto originalJobs = native.value("m3u8s/parallelJobs");
        const auto originalPin = native.value("privacy/pinHash");
        QTemporaryDir directory;
        auto empty = run({"list"},directory.path());
        QCOMPARE(empty.code,0); QCOMPARE(empty.json.value("schemaVersion").toInt(),1);
        QVERIFY(empty.json.value("data").toArray().isEmpty());
        QVERIFY(!QFileInfo::exists(directory.filePath("vibeplayer.sqlite3")));
        QCOMPARE(run({"playback-history","list","--date","invalid"},directory.path()).code,2);
        QCOMPARE(run({"playback-history","show","missing"},directory.path()).code,4);
        QCOMPARE(run({"local","ls","missing"},directory.path()).code,4);
        QCOMPARE(run({"schedule","show","missing"},directory.path()).code,4);
        auto invalid = run({"unknown"},directory.path());
        QCOMPARE(invalid.code,2); QVERIFY(invalid.json.contains("data")); QVERIFY(invalid.json.value("data").isNull());
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const auto id = addService(directory.path(),endpoint,QStringLiteral("中文 服务"));
        QVERIFY(!id.isEmpty());
        auto updated = run({"service","update",id,"--name",QStringLiteral("中文 新名称")},directory.path());
        QCOMPARE(updated.code,0); QCOMPARE(updated.json.value("data").toObject().value("id").toString(),id);
        auto missing = run({"service","check",id},directory.path()); QCOMPARE(missing.code,3);
        auto check = run({"service","check",id,"--password-env","CLI_TEST_PASSWORD"},directory.path());
        QCOMPARE(check.code,0); QVERIFY(!check.output.contains("test-password")); QVERIFY(!check.output.contains("online"));
        if (CredentialStore::isAvailable()) {
            const auto prior = CredentialStore::loadPassword(id);
            QVERIFY(prior); QVERIFY(!prior->has_value());
            const auto cleanup = qScopeGuard([&] { static_cast<void>(CredentialStore::deletePassword(id)); });
            QCOMPARE(run({"service","login",id,"--password-env","CLI_TEST_PASSWORD"},directory.path()).code,0);
            QCOMPARE(run({"service","check",id},directory.path()).code,0);
            QVERIFY(run({"service","show",id},directory.path()).json.value("data").toObject().value("hasSavedCredentials").toBool());
            QCOMPARE(run({"service","remove",id,"--yes"},directory.path()).code,0);
            QVERIFY(!CredentialStore::loadPassword(id)->has_value());
            // Re-add the same test service for the remaining lifecycle checks.
            QCOMPARE(addService(directory.path(),endpoint,QStringLiteral("中文 新名称")),id);
        }
        QCOMPARE(run({"service","show",QStringLiteral("中文 新名称")},directory.path()).code,0);
        QCOMPARE(run({"service","remove",id},directory.path()).code,2);
        const auto embyId = addService(directory.path(),endpoint,"Duplicate","emby");
        const auto jellyfinId = addService(directory.path(),endpoint,"Duplicate","jellyfin");
        QCOMPARE(run({"service","show","Duplicate"},directory.path()).code,2);
        const auto playlist = directory.filePath(QStringLiteral("中文 播放列表.m3u"));
        QVERIFY(write(playlist,"#EXTM3U\n#EXTINF:-1,Test Channel\nhttps://example.invalid/channel.m3u8\n"));
        const auto iptv = run({"service","add","--type","iptv","--name","TV","--playlist",playlist},directory.path());
        QVERIFY2(iptv.code == 0,iptv.errors.constData());
        const auto iptvId = iptv.json.value("data").toObject().value("id").toString();
        QVERIFY(!iptvId.isEmpty());
        QCOMPARE(run({"service","check",iptvId},directory.path()).code,0);
        QCOMPARE(run({"service","update",iptvId,"--name","TV renamed"},directory.path()).code,0);
        for (const auto& mediaId : {embyId,jellyfinId}) {
            endpoint.failAuthentication = true;
            QCOMPARE(run({"service","login",mediaId,"--password-env","CLI_TEST_PASSWORD"},directory.path()).code,3);
            endpoint.failAuthentication = false;
            auto login = run({"service","login",mediaId,"--password-env","CLI_TEST_PASSWORD"},directory.path());
            QCOMPARE(login.code,0); QVERIFY(!login.output.contains("test-session-token"));
            QCOMPARE(run({"service","check",mediaId},directory.path()).code,0);
        }
        // Hold another open database connection, as the desktop does.
        SessionRepository desktop("cli-simultaneous",directory.filePath("vibeplayer.sqlite3"));
        QVERIFY(desktop.loadServiceCards(false));
        const auto before = read(directory.filePath("vibeplayer.sqlite3"));
        QCOMPARE(run({"list"},directory.path()).code,0);
        QCOMPARE(read(directory.filePath("vibeplayer.sqlite3")),before);
        QCOMPARE(run({"service","remove",embyId,"--yes"},directory.path()).code,0);
        QVERIFY(!*desktop.loadSession(embyId));
        QCOMPARE(run({"service","remove",id,"--yes"},directory.path()).code,0);
        QCOMPARE(run({"service","show",id},directory.path()).code,4);
        auto config = run({"config","set","m3u8s/parallelJobs","2"},directory.path());
        QCOMPARE(config.code,0);
        QCOMPARE(run({"config","get","m3u8s/parallelJobs"},directory.path()).json.value("data").toObject().value("m3u8s/parallelJobs").toInt(),2);
        QCOMPARE(run({"config","set","m3u8s/parallelJobs","0"},directory.path()).code,2);
        QVERIFY(QFileInfo::exists(directory.filePath("vibePlayerQT/vibePlayerQT.ini")));
        native.sync();
        QCOMPARE(native.value("m3u8s/parallelJobs"),originalJobs);
        QCOMPARE(native.value("privacy/pinHash"),originalPin);
    }

    void webDavTransfersAndShell()
    {
        QTemporaryDir directory; Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const auto id = addService(directory.path(),endpoint);
        const auto command = [&](QStringList args) { return run(args + QStringList {"--password-env","CLI_TEST_PASSWORD"},directory.path()); };
        QCOMPARE(command({"webdav","mkdir",id,QStringLiteral("/中文 目录")}).code,0);
        QCOMPARE(command({"webdav","mkdir",id,QStringLiteral("/中文 目录")}).code,5);
        QCOMPARE(command({"webdav","mkdir",id,"/missing/child"}).code,5);
        auto stat = command({"webdav","stat",id,QStringLiteral("/中文 目录")});
        QCOMPARE(stat.code,0); QVERIFY(stat.json.value("data").toObject().value("directory").toBool());
        const auto folder = directory.filePath(QStringLiteral("上传 中文"));
        QVERIFY(QDir().mkpath(folder + "/nested"));
        QVERIFY(write(folder + "/nested/file #%.txt","payload"));
        auto uploaded = command({"webdav","upload",id,folder,"/tree"});
        QCOMPARE(uploaded.code,0); QCOMPARE(endpoint.files.value("/dav/tree/nested/file #%.txt"),QByteArray("payload"));
        QVERIFY(std::ranges::any_of(endpoint.requests,[](const Request& request) {
            return request.method == "PUT" && request.headers.value("if-none-match") == "*";
        }));
        QCOMPARE(command({"webdav","upload",id,folder,"/tree"}).code,5);
        QCOMPARE(command({"webdav","upload",id,folder,"/tree","--overwrite"}).code,0);
        const auto downloaded = directory.filePath(QStringLiteral("下载 中文"));
        QCOMPARE(command({"webdav","download",id,"/tree",downloaded}).code,0);
        QCOMPARE(read(downloaded + "/nested/file #%.txt"),QByteArray("payload"));
        QCOMPARE(command({"webdav","download",id,"/tree",downloaded}).code,5);
        endpoint.methodErrors["GET"] = 401;
        QCOMPARE(command({"webdav","download",id,"/tree/nested/file #%.txt",directory.filePath("unauthorized.txt")}).code,3);
        endpoint.methodErrors["GET"] = 404;
        QCOMPARE(command({"webdav","download",id,"/tree/nested/file #%.txt",directory.filePath("disappeared.txt")}).code,4);
        endpoint.methodErrors.clear();
        endpoint.racePut = true;
        QCOMPARE(command({"webdav","upload",id,folder + "/nested/file #%.txt","/race.txt"}).code,5);
        QCOMPARE(endpoint.files.value("/dav/race.txt"),QByteArray("race-original"));
        endpoint.racePut = false;
        endpoint.failCode = 503; QCOMPARE(command({"webdav","ls",id}).code,1); endpoint.failCode = 0;
        auto shell = run({"shell","--password-env","CLI_TEST_PASSWORD"},directory.path(),
            "use " + id.toUtf8() + "\ncd tree\npwd\nls nested\nexit\n");
        QCOMPARE(shell.code,0); QCOMPARE(shell.output.count('\n'),4); QVERIFY(shell.output.contains("/tree"));
        endpoint.malicious = true;
        QCOMPARE(command({"webdav","download",id,"/tree",directory.filePath("outside")}).code,1);
        QVERIFY(!QFileInfo::exists(directory.filePath("outside")));
    }

    void cancelsNetworkAndCleansDownload()
    {
        QTemporaryDir directory; Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const auto id = addService(directory.path(),endpoint);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
        qputenv("CLI_TEST_PASSWORD","test-password");
        Cli::Engine engine(directory.path());
        endpoint.files["/dav/slow.txt"] = "payload";
        endpoint.stallMethods.insert("GET");
        // Stall only the GET, after the stat/plan completed.
        QTimer cancellation; cancellation.setInterval(1);
        connect(&cancellation,&QTimer::timeout,&endpoint,[&] {
            if (!endpoint.requests.empty() && endpoint.requests.back().method == "GET")
                CliConsole::canceled.store(true);
        });
        cancellation.start();
        auto command = Cli::parse({"webdav","download",id,"/slow.txt",directory.filePath("download.txt"),"--password-env","CLI_TEST_PASSWORD"});
        QVERIFY(command);
        const auto result = engine.execute(*command);
        QCOMPARE(result.code,Cli::ExitCode::Canceled);
        QVERIFY(!QFileInfo::exists(directory.filePath("download.txt")));
        QVERIFY(QDir(directory.path()).entryList({".vibe-download-*"},QDir::Dirs | QDir::Hidden).isEmpty());
        endpoint.stall = true;
        QTimer::singleShot(100,&endpoint,[] { CliConsole::canceled.store(true); });
        QCOMPARE(engine.execute(*Cli::parse({"tssl","restore","--target","webdav","--service",id,"--path","backups","--password-env","CLI_TEST_PASSWORD"})).code,Cli::ExitCode::Canceled);
        qunsetenv("CLI_TEST_PASSWORD"); CliConsole::canceled.store(false);
    }

#ifdef Q_OS_WIN
    void powerShellCaptureFinishesWhileTaskHostIsAlive()
    {
        QTemporaryDir directory;
        const auto cleanup = qScopeGuard([&] { run({"daemon","stop"},directory.path()); QTest::qWait(400); });
        auto program = QStandardPaths::findExecutable("pwsh");
        if (program.isEmpty()) program = QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/WindowsPowerShell/v1.0/powershell.exe");
        const auto executable = QString::fromUtf8(CLI_EXECUTABLE).replace("'","''");
        const auto profile = directory.path().replace("'","''");
        const auto script = QStringLiteral("$r = & '%1' task list --json --data-dir '%2'\nif ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n$r\n")
            .arg(executable,profile);
        const auto scriptPath = directory.filePath("capture.ps1");
        QVERIFY(write(scriptPath,script.toUtf8()));
        QProcess shell;
        shell.start(program,{"-NoProfile","-NonInteractive","-ExecutionPolicy","Bypass","-File",scriptPath});
        const bool finished = shell.waitForFinished(10000);
        if (!finished) { shell.kill(); shell.waitForFinished(3000); }
        QVERIFY2(finished,"PowerShell waited for a detached host to close inherited pipes");
        QCOMPARE(shell.exitCode(),0);
        const auto result = QJsonDocument::fromJson(shell.readAllStandardOutput()).object();
        QVERIFY2(result["ok"].toBool(),shell.readAllStandardError().constData());
        // The same daemon is still available after the shell has exited.
        QCOMPARE(run({"task","list"},directory.path()).code,0);
    }

    void windowsConsoleInterrupt_data()
    {
        QTest::addColumn<bool>("shell");
        QTest::addColumn<bool>("playback");
        QTest::newRow("standalone") << false << false;
        QTest::newRow("shell-returns-to-prompt") << true << false;
        QTest::newRow("encrypted-playback-preparation") << false << true;
    }

    void windowsConsoleInterrupt()
    {
        QFETCH(bool,shell);
        QFETCH(bool,playback);
        QTemporaryDir directory; Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const auto id = addService(directory.path(),endpoint);
        endpoint.files["/dav/slow.txt"] = "payload";
        endpoint.files["/dav/slow.m3u8s"] = "payload";
        endpoint.stallMethods.insert("GET");
        const auto cleanupPlayback = qScopeGuard([&] {
            if (playback) { run({"player","stop"},directory.path()); QTest::qWait(400); }
        });
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("CLI_TEST_PASSWORD","test-password");
        process.setProcessEnvironment(environment);
        // Give only the owned test child a separate hidden console. The signal
        // helper attaches to it, so Ctrl+C cannot reach the test runner or user.
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
            arguments->flags |= CREATE_NEW_CONSOLE;
            arguments->flags &= ~CREATE_NO_WINDOW;
            arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            arguments->startupInfo->wShowWindow = SW_HIDE;
        });
        QStringList arguments = shell ? QStringList {"shell"} : playback ? QStringList {
            "play",id,"/slow.m3u8s","--headless","--wait"} : QStringList {
            "webdav","download",id,"/slow.txt",directory.filePath("download.txt")};
        arguments += QStringList {"--json","--data-dir",directory.path(),"--password-env","CLI_TEST_PASSWORD"};
        QEventLoop loop;
        QTimer poll,timeout; poll.setInterval(5); timeout.setSingleShot(true);
        bool sent = false, resumed = false, expired = false;
        QByteArray output;
        connect(&process,&QProcess::readyReadStandardOutput,&loop,[&] {
            output += process.readAllStandardOutput();
            if (shell && sent && !resumed && output.count('\n') >= 2) {
                resumed = true; process.write("pwd\nexit\n"); process.closeWriteChannel();
            }
        });
        connect(&process,&QProcess::finished,&loop,&QEventLoop::quit);
        connect(&timeout,&QTimer::timeout,&loop,[&] { expired = true; process.kill(); loop.quit(); });
        connect(&poll,&QTimer::timeout,&loop,[&] {
            if (sent || endpoint.requests.empty() || endpoint.requests.back().method != "GET") return;
            sent = true;
            QProcess helper;
            helper.start(QCoreApplication::applicationFilePath(),{"--send-test-interrupt",QString::number(process.processId())});
            QVERIFY(helper.waitForFinished(5000)); QCOMPARE(helper.exitCode(),0);
        });
        process.start(QString::fromUtf8(CLI_EXECUTABLE),arguments);
        QVERIFY(process.waitForStarted(5000));
        if (shell) process.write("use " + id.toUtf8() + "\ndownload /slow.txt \"" +
            directory.filePath("download.txt").toUtf8() + "\"\n");
        else process.closeWriteChannel();
        poll.start(); timeout.start(15000); loop.exec();
        if (expired) process.waitForFinished(3000);
        output += process.readAllStandardOutput();
        QVERIFY2(!expired,process.readAllStandardError().constData());
        QVERIFY(sent);
        if (shell) {
            QCOMPARE(process.exitCode(),130); QVERIFY(resumed);
            const auto lines = output.trimmed().split('\n'); QCOMPARE(lines.size(),3);
            QCOMPARE(QJsonDocument::fromJson(lines[1]).object().value("error").toObject().value("code").toInt(),130);
            QVERIFY(QJsonDocument::fromJson(lines[2]).object().value("ok").toBool());
        } else {
            QCOMPARE(process.exitCode(),130);
            QCOMPARE(QJsonDocument::fromJson(output).object().value("error").toObject().value("code").toInt(),130);
        }
        QVERIFY(!QFileInfo::exists(directory.filePath("download.txt")));
        QVERIFY(QDir(directory.path()).entryList({".vibe-download-*"},QDir::Dirs | QDir::Hidden).isEmpty());
    }
#endif

    void tsslAndThreeBackupTargets()
    {
        QTemporaryDir directory; Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const auto id = addService(directory.path(),endpoint);
        TsslPackage package;
        package.identifier = QByteArray(TsslPackage::identifierLength,'A');
        package.rootManifestDigest = QByteArray(32,'D');
        package.segmentKeys.insert("segments/one.ts",QByteArray(32,'K'));
        const auto source = directory.filePath("source.tssl");
        QVERIFY(write(source,package.toJson()));
        QVERIFY(write(directory.filePath("bad.tssl"),"broken"));
        auto imported = run({"tssl","import",source,source,directory.filePath("bad.tssl")},directory.path());
        QCOMPARE(imported.code,6);
        const auto data = imported.json.value("data").toObject();
        QCOMPARE(data.value("imported").toInt(),1); QCOMPARE(data.value("duplicates").toInt(),1); QCOMPARE(data.value("failed").toInt(),1);
        QCOMPARE(run({"tssl","import",directory.filePath("bad.tssl")},directory.path()).code,1);
        const auto digest = QString::fromLatin1(package.rootManifestDigest.toHex());
        auto shown = run({"tssl","show",digest},directory.path());
        QCOMPARE(shown.code,0); QVERIFY(!shown.output.contains(QByteArray(32,'K').toBase64()));
        QCOMPARE(run({"tssl","export",digest,directory.filePath("export.tssl")},directory.path()).code,0);
        QCOMPARE(run({"tssl","export","--all","--output",directory.filePath("all")},directory.path()).code,0);
        QCOMPARE(run({"tssl","export",digest,digest,"--output",directory.filePath("selected")},directory.path()).json["data"].toObject()["exported"].toInt(),1);
        {
            Cli::Engine engine(directory.path());
            const auto resetCancellation = qScopeGuard([] { CliConsole::canceled.store(false); });
            QTimer::singleShot(0,this,[] { CliConsole::canceled.store(true); });
            auto canceledExport = engine.execute(*Cli::parse({"tssl","export","--all","--output",directory.filePath("canceled-export")}));
            QCOMPARE(canceledExport.code,Cli::ExitCode::Canceled);
            QVERIFY(QDir(directory.filePath("canceled-export")).entryList({"*.tssl"},QDir::Files).isEmpty());
            QTimer::singleShot(0,this,[] { CliConsole::canceled.store(true); });
            QCOMPARE(engine.execute(*Cli::parse({"tssl","backup","--target","local","--local-path",directory.filePath("canceled-backup")})).code,
                     Cli::ExitCode::Canceled);
            QVERIFY(QDir(directory.filePath("canceled-backup")).entryList({"*.tssl","*.zip"},QDir::Files).isEmpty());
        }
        QCOMPARE(run({"tssl","remove",digest},directory.path()).code,2);
        for (const auto& target : QStringList {"local","webdav","s3"}) {
            QStringList options {"--target",target};
            if (target == "local") options += QStringList {"--local-path",directory.filePath("backups")};
            if (target == "webdav") {
                endpoint.directories.insert("/dav/backups/");
                options += QStringList {"--service",id,"--path","backups","--password-env","CLI_TEST_PASSWORD"};
            }
            if (target == "s3") options += QStringList {"--s3-endpoint",endpoint.s3(),"--s3-bucket","bucket","--s3-region","us-east-1",
                "--s3-prefix","backups","--s3-access-key","test-access","--s3-secret-env","CLI_TEST_SECRET","--s3-session-token-env","CLI_TEST_S3_TOKEN"};
            const auto backup = run(QStringList {"tssl","backup"} + options,directory.path());
            QVERIFY2(backup.code == 0,(target.toUtf8() + ": " + backup.errors).constData());
            QCOMPARE(backup.json["data"].toObject()["backedUp"].toInt(),1);
            if (target == "local") {
                const auto archives = QDir(directory.filePath("backups")).entryList({"*.zip"},QDir::Files);
                QCOMPARE(archives.size(),1);
                QVERIFY(read(QDir(directory.filePath("backups")).filePath(archives.first())).startsWith("PK"));
                QVERIFY(QDir(directory.filePath("backups")).entryList({"*.tssl"},QDir::Files).isEmpty());
            } else {
                const auto prefix = target == "webdav" ? QStringLiteral("/dav/backups/") : QStringLiteral("/bucket/backups/");
                QCOMPARE(std::ranges::count_if(endpoint.files.keys(),[&](const QString& file) {
                    return file.startsWith(prefix) && file.endsWith(".zip");
                }),1);
            }
            QCOMPARE(run({"tssl","remove",digest,"--yes"},directory.path()).code,0);
            auto restored = run(QStringList {"tssl","restore"} + options,directory.path());
            QCOMPARE(restored.code,0); QCOMPARE(restored.json.value("data").toObject().value("imported").toInt(),1);
            QVERIFY(!restored.output.contains("test-s3-secret"));
            const auto legacy = run(QStringList {"tssl","backup","--format","files"} + options,directory.path());
            QVERIFY2(legacy.code == 0,(target.toUtf8() + ": " + legacy.errors).constData());
            QCOMPARE(run({"tssl","remove",digest,"--yes"},directory.path()).code,0);
            const auto mixed = run(QStringList {"tssl","restore"} + options,directory.path());
            QVERIFY2(mixed.code == 0,(target.toUtf8() + ": " + mixed.errors).constData());
            QCOMPARE(mixed.json["data"].toObject()["imported"].toInt(),1);
            QCOMPARE(mixed.json["data"].toObject()["duplicates"].toInt(),1);
        }
        QVERIFY(std::ranges::any_of(endpoint.requests,[](const Request& request) {
            return request.headers.value("authorization").contains("x-amz-security-token") &&
                   request.headers.value("x-amz-security-token") == "test-session-token-s3";
        }));
        auto removed = run({"tssl","remove",digest,QString(64,'a'),"--yes"},directory.path());
        QCOMPARE(removed.code,6); QCOMPARE(removed.json["data"].toObject()["removed"].toInt(),1);
    }

    void tsslZipAndLegacyBackupCompatibility()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QStringList digests;
        for (int index = 0; index < 2; ++index) {
            TsslPackage package;
            package.identifier = QByteArray(TsslPackage::identifierLength,static_cast<char>('A' + index));
            package.rootManifestDigest = QByteArray(32,static_cast<char>('D' + index));
            package.segmentKeys.insert("segments/one.ts",QByteArray(32,'K'));
            const auto source = directory.filePath(QStringLiteral("密钥 %1.tssl").arg(index));
            QVERIFY(write(source,package.toJson()));
            QCOMPARE(run({"tssl","import",source},directory.path()).code,0);
            digests += QString::fromLatin1(package.rootManifestDigest.toHex());
        }
        QCOMPARE(run({"config","get","tsslBackup/format"},directory.path()).json["data"].toObject()["tsslBackup/format"].toString(),QString("zip"));
        QCOMPARE(run({"config","set","tsslBackup/format","tar"},directory.path()).code,2);
        const auto backupPath = directory.filePath(QStringLiteral("备份 目录"));
        const QStringList local {"--target","local","--local-path",backupPath};
        const auto zipped = run(QStringList {"tssl","backup"} + local,directory.path());
        QVERIFY2(zipped.code == 0,zipped.errors.constData());
        QCOMPARE(zipped.json["data"].toObject()["backedUp"].toInt(),2);
        QCOMPARE(zipped.json["data"].toObject()["format"].toString(),QString("zip"));
        const auto archives = QDir(backupPath).entryList({"*.zip"},QDir::Files);
        QCOMPARE(archives.size(),1);
        QVERIFY(read(QDir(backupPath).filePath(archives.first())).startsWith("PK"));
        QVERIFY(QDir(backupPath).entryList({"*.tssl"},QDir::Files).isEmpty());
        // A temporary legacy override keeps saved defaults and coexists with ZIP snapshots.
        const auto loose = run(QStringList {"tssl","backup","--format","files"} + local,directory.path());
        QVERIFY2(loose.code == 0,loose.errors.constData());
        QCOMPARE(loose.json["data"].toObject()["backedUp"].toInt(),2);
        QCOMPARE(QDir(backupPath).entryList({"*.tssl"},QDir::Files).size(),2);
        QCOMPARE(run({"config","get","tsslBackup/format"},directory.path()).json["data"].toObject()["tsslBackup/format"].toString(),QString("zip"));
        QCOMPARE(run(QStringList {"tssl","remove"} + digests + QStringList {"--yes"},directory.path()).code,0);
        const auto restored = run(QStringList {"tssl","restore"} + local,directory.path());
        QVERIFY2(restored.code == 0,restored.errors.constData());
        QCOMPARE(restored.json["data"].toObject()["imported"].toInt(),2);
        QCOMPARE(restored.json["data"].toObject()["duplicates"].toInt(),2);
        QCOMPARE(restored.json["data"].toObject()["failed"].toInt(),0);
        const auto duplicate = run(QStringList {"tssl","restore"} + local,directory.path());
        QCOMPARE(duplicate.code,0);
        QCOMPARE(duplicate.json["data"].toObject()["duplicates"].toInt(),4);
        // Saved files mode remains usable for old clients; snapshots are left in place.
        QCOMPARE(run({"config","set","tsslBackup/format","files"},directory.path()).code,0);
        const auto savedLoose = run(QStringList {"tssl","backup"} + local,directory.path());
        QCOMPARE(savedLoose.code,0);
        QCOMPARE(savedLoose.json["data"].toObject()["format"].toString(),QString("files"));
        QCOMPARE(QDir(backupPath).entryList({"*.zip"},QDir::Files).size(),1);
        QVERIFY(write(QDir(backupPath).filePath("tssl-backup-corrupt.zip"),"broken ZIP"));
        const auto damaged = run(QStringList {"tssl","restore"} + local,directory.path());
        QCOMPARE(damaged.code,6);
        QCOMPARE(damaged.json["data"].toObject()["duplicates"].toInt(),4);
        QVERIFY(damaged.json["data"].toObject()["failed"].toInt() > 0);
        QVERIFY(!damaged.output.contains(QByteArray(32,'K').toBase64()));
    }

    void iptvProbeUsesBoundedSharedCache()
    {
        QTemporaryDir directory; Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.files["/dav/playlist.m3u"] = "#EXTM3U\n#EXTINF:-1,Live\nmaster.m3u8\n";
        endpoint.files["/dav/master.m3u8"] = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS=\"avc1.640028\"\nhd.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=640x360\nsd.m3u8\n";
        const auto service = run({"service","add","--type","iptv","--name","Live","--playlist",endpoint.root() + "playlist.m3u"},directory.path()).json["data"].toObject()["id"].toString();
        QVERIFY(!service.isEmpty());
        const auto probed = run({"iptv","streams",service,"Live","--probe"},directory.path());
        QCOMPARE(probed.code,0);
        QVERIFY(std::ranges::any_of(probed.json["data"].toArray(),[](const auto& row) { return row.toObject()["width"].toInt() == 1920; }));
        QVERIFY(!probed.output.contains("master.m3u8"));
        const auto requests = endpoint.requests.size();
        QCOMPARE(run({"iptv","streams",service,"Live","--probe"},directory.path()).code,0);
        QCOMPARE(endpoint.requests.size(),requests);
        QCOMPARE(run({"iptv","streams",service,"Live","--refresh"},directory.path()).code,0);
        QVERIFY(endpoint.requests.size() > requests);
    }

    void desktopFolderAndScheduleCommands()
    {
        QTemporaryDir directory;
        const auto root = directory.filePath(QStringLiteral("本地 视频"));
        QVERIFY(QDir().mkpath(root));
        QVERIFY(write(QDir(root).filePath("sample.mp4"),"fixture"));
        QVERIFY(write(QDir(root).filePath("ignore.txt"),"fixture"));
        for (const auto& group : {"local","playback-history","schedule"}) {
            const auto result = run({group,"list"},directory.path());
            QCOMPARE(result.code,0); QVERIFY(result.json["data"].toArray().isEmpty());
        }
        QVERIFY(!QFileInfo::exists(directory.filePath("vibeplayer.sqlite3")));
        const auto added = run({"local","add",root,"--name",QStringLiteral("电影")},directory.path());
        QCOMPARE(added.code,0);
        const auto rootId = added.json["data"].toObject()["id"].toString();
        QCOMPARE(run({"local","add",root},directory.path()).code,5);
        const auto listing = run({"local","ls",rootId},directory.path());
        QCOMPARE(listing.code,0); QCOMPARE(listing.json["data"].toArray().size(),1);
        QCOMPARE(run({"local","ls",rootId,".."},directory.path()).code,2);
        QCOMPARE(run({"local","play",rootId,"../outside.mp4","--headless"},directory.path()).code,2);
        QCOMPARE(run({"local","remove",rootId},directory.path()).code,2);
        QCOMPARE(run({"local","remove",rootId,"--yes"},directory.path()).code,0);
        QVERIFY(QFileInfo::exists(QDir(root).filePath("sample.mp4")));
        QCOMPARE(run({"config","set","iptv/streamPreference","dataSaver"},directory.path()).code,0);
        QCOMPARE(run({"config","get","iptv/streamPreference"},directory.path()).json["data"].toObject()["iptv/streamPreference"].toString(),QString("dataSaver"));
        QCOMPARE(run({"config","set","iptv/streamPreference","invalid"},directory.path()).code,2);
        QCOMPARE(run({"config","set","m3u8s/segmentDuration","2"},directory.path()).code,0);
        QCOMPARE(run({"config","set","m3u8s/segmentDuration","1"},directory.path()).code,2);
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost)); endpoint.mediaFixture = true;
        const auto service = addService(directory.path(),endpoint,"Scheduled Emby","emby");
        QCOMPARE(run({"service","login",service,"--password-env","CLI_TEST_PASSWORD"},directory.path()).code,0);
        const auto task = run({"schedule","add","--service",service,"--schedule-type","weekly","--time","23:59","--days","1","--duration","5","--enabled","false"},directory.path());
        QCOMPARE(task.code,0);
        const auto id = task.json["data"].toObject()["id"].toString();
        QCOMPARE(run({"schedule","update",id,"--days","8"},directory.path()).code,2);
        QCOMPARE(run({"schedule","update",id,"--duration","4"},directory.path()).code,2);
        QCOMPARE(run({"schedule","update",id,"--schedule-type","manual"},directory.path()).code,0);
        QCOMPARE(run({"schedule","show",id},directory.path()).json["data"].toObject()["scheduleType"].toString(),QString("manual"));
        QCOMPARE(run({"schedule","add","--service",service},directory.path()).code,5);
        const auto stopHost = qScopeGuard([&] { run({"schedule","unwatch"},directory.path()); QTest::qWait(250); });
        QCOMPARE(run({"schedule","watch"},directory.path()).code,0);
        QCOMPARE(run({"schedule","status"},directory.path()).json["data"].toObject()["watching"].toBool(),true);
        QCOMPARE(run({"schedule","resolve","--run","invalid"},directory.path()).code,2);
        QCOMPARE(run({"schedule","resolve","--run","false"},directory.path()).code,0);
        QLockFile execution(directory.filePath("vibeplayer.sqlite3.scheduled-playback.lock"));
        execution.setStaleLockTime(0); QVERIFY(execution.tryLock());
        QCOMPARE(run({"schedule","run",id},directory.path()).code,1);
        execution.unlock();
        QCOMPARE(run({"schedule","stop"},directory.path()).code,0);
        QCOMPARE(run({"schedule","remove",id},directory.path()).code,2);
        QCOMPARE(run({"schedule","remove",id,"--yes"},directory.path()).code,0);
        QVERIFY(run({"schedule","list"},directory.path()).json["data"].toArray().isEmpty());
        QCOMPARE(run({"schedule","unwatch"},directory.path()).code,0);
        QTRY_VERIFY(!QFileInfo::exists(directory.filePath("cli-schedule.lock")));
    }

    void iptvFallbackSwitchingAndPlaybackHistory()
    {
        const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (ffmpeg.isEmpty()) QSKIP("External FFmpeg is unavailable");
        QTemporaryDir directory;
        const auto video = directory.filePath(QStringLiteral("测试视频.mp4"));
        QProcess generator;
        generator.start(ffmpeg,{"-v","error","-f","lavfi","-i","testsrc2=size=320x180:rate=25",
            "-f","lavfi","-i","sine=frequency=440","-f","lavfi","-i","sine=frequency=880",
            "-map","0:v","-map","1:a","-map","2:a","-t","4","-c:v","libx264","-c:a","aac","-pix_fmt","yuv420p","-y",video});
        QVERIFY(generator.waitForFinished(15000)); QCOMPARE(generator.exitCode(),0);
        const auto backup = directory.filePath("backup.mp4"); QVERIFY(QFile::copy(video,backup));
        const auto playlist = directory.filePath("channels.m3u");
        QByteArray payload = "#EXTM3U\n";
        for (const auto& path : {directory.filePath("missing.mp4"),video,backup})
            payload += "#EXTINF:-1 tvg-id=\"same\",Merged channel\n" + QUrl::fromLocalFile(path).toString().toUtf8() + '\n';
        QVERIFY(write(playlist,payload));
        const auto added = run({"service","add","--type","iptv","--name","IPTV","--playlist",playlist},directory.path());
        QCOMPARE(added.code,0);
        const auto service = added.json["data"].toObject()["id"].toString();
        const auto channels = run({"media","channels",service},directory.path()).json["data"].toArray();
        QCOMPARE(channels.size(),1); QCOMPARE(channels[0].toObject()["streamCount"].toInt(),3);
        const auto channel = channels[0].toObject()["id"].toString();
        QCOMPARE(run({"privacy","set","--new-pin-env","CLI_TEST_PIN","--confirm-pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        const auto stopPlayer = qScopeGuard([&] {
            run({"player","stop","--private","--pin-env","CLI_TEST_PIN"},directory.path());
            run({"player","stop"},directory.path()); QTest::qWait(450);
        });
        const auto before = run({"iptv","streams",service,channel},directory.path());
        QCOMPARE(before.code,0); QCOMPARE(before.json["data"].toArray().size(),3);
        QVERIFY(!before.output.contains("missing.mp4"));
        QCOMPARE(run({"play",service,channel,"--headless"},directory.path()).code,0);
        QJsonArray streams;
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 12000) {
            streams = run({"player","streams"},directory.path()).json["data"].toArray();
            if (std::ranges::any_of(streams,[](const auto& value) { const auto row = value.toObject(); return row["current"].toBool() && row["width"].toInt() == 320; })) break;
            QTest::qWait(100);
        }
        QVERIFY(std::ranges::any_of(streams,[](const auto& value) { const auto row = value.toObject(); return row["current"].toBool() && row["width"].toInt() == 320; }));
        QString other;
        for (const auto& value : streams) if (value.toObject()["line"].toInt() == 3) other = value.toObject()["id"].toString();
        QVERIFY(!other.isEmpty());
        QCOMPARE(run({"player","stream",other},directory.path()).code,0);
        const auto switched = run({"player","streams","--probe"},directory.path());
        QCOMPARE(switched.code,0);
        QVERIFY(std::ranges::any_of(switched.json["data"].toArray(),[&](const auto& value) { const auto row = value.toObject(); return row["id"].toString() == other && row["current"].toBool(); }));
        QCOMPARE(run({"player","stream","invalid"},directory.path()).code,4);
        QTest::qWait(150);
        QCOMPARE(run({"player","audio","1"},directory.path()).code,0);
        QTRY_VERIFY(std::ranges::any_of(run({"player","status"},directory.path()).json["data"].toObject()["audio"].toArray(),
            [](const auto& value) { return value.toObject()["row"].toInt() == 1 && value.toObject()["selected"].toBool(); }));
        const auto subtitle = directory.filePath(QStringLiteral("字幕.srt"));
        QVERIFY(write(subtitle,"1\n00:00:00,000 --> 00:00:03,000\nCLI subtitle\n"));
        QCOMPARE(run({"player","subtitle",subtitle},directory.path()).code,0);
        QTRY_VERIFY(!run({"player","status"},directory.path()).json["data"].toObject()["subtitles"].toArray().isEmpty());
        QTest::qWait(1200); // Accumulate at least one IPTV watch second in the shared statistics.
        QCOMPARE(run({"service","update",service,"--private-mode","true","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        QCOMPARE(run({"player","status"},directory.path()).code,3);
        QCOMPARE(run({"player","status","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        QCOMPARE(run({"service","update",service,"--private-mode","false","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        QCOMPARE(run({"player","stop"},directory.path()).code,0);
        QTest::qWait(450);
        const auto cached = run({"iptv","streams",service,channel},directory.path());
        QCOMPARE(cached.code,0);
        QVERIFY(std::ranges::any_of(cached.json["data"].toArray(),[](const auto& value) { return value.toObject()["width"].toInt() == 320; }));
        const auto history = run({"playback-history","list","--source","IPTV"},directory.path());
        QCOMPARE(history.code,0); QCOMPARE(history.json["data"].toArray().size(),1);
        const auto record = history.json["data"].toArray()[0].toObject()["id"].toString();
        QCOMPARE(run({"playback-history","show",record},directory.path()).code,0);
        QCOMPARE(run({"playback-history","remove",record},directory.path()).code,2);
        QCOMPARE(run({"playback-history","remove",record,"--yes"},directory.path()).code,0);
        const auto root = run({"local","add",directory.path()},directory.path()).json["data"].toObject()["id"].toString();
        QCOMPARE(run({"local","play",root,QFileInfo(video).fileName(),"--headless","--wait"},directory.path()).code,0);
        const auto local = run({"playback-history","list","--source","Local","--limit","1"},directory.path());
        QCOMPARE(local.json["data"].toArray().size(),1);
        const auto localRecord = local.json["data"].toArray()[0].toObject()["id"].toString();
        QVERIFY(local.json["data"].toArray()[0].toObject()["completed"].toBool());
        QCOMPARE(run({"playback-history","play",localRecord,"--headless","--wait"},directory.path()).code,0);
        QVERIFY(!run({"playback-history","stats"},directory.path()).json["data"].toArray().isEmpty());
        QCOMPARE(run({"playback-history","clear","--date","invalid","--yes"},directory.path()).code,2);
        QCOMPARE(run({"play",video,"--headless","--start","1"},directory.path()).code,0);
        QTest::qWait(350);
        QCOMPARE(run({"player","stop"},directory.path()).code,0); QTest::qWait(450);
        const auto interrupted = run({"playback-history","list","--source","Local","--limit","1"},directory.path()).json["data"].toArray();
        QCOMPARE(interrupted.size(),1);
        QVERIFY(interrupted[0].toObject()["position"].toInteger() >= 1);
        QVERIFY(interrupted[0].toObject()["duration"].toInteger() >= 3);
        QCOMPARE(run({"play",video,"--headless","--wait","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        const auto hidden = run({"playback-history","list"},directory.path());
        const auto unlocked = run({"playback-history","list","--private","--pin-env","CLI_TEST_PIN"},directory.path());
        QVERIFY(unlocked.json["data"].toArray().size() > hidden.json["data"].toArray().size());
        QCOMPARE(run({"playback-history","clear","--date",QDate::currentDate().toString(Qt::ISODate),"--yes","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        QVERIFY(run({"playback-history","list","--private","--pin-env","CLI_TEST_PIN"},directory.path()).json["data"].toArray().isEmpty());
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost)); endpoint.mediaFixture = true; endpoint.mediaFile = read(video);
        const auto emby = addService(directory.path(),endpoint,"Schedule playback","emby");
        QCOMPARE(run({"service","login",emby,"--password-env","CLI_TEST_PASSWORD"},directory.path()).code,0);
        const auto scheduled = run({"schedule","add","--service",emby,"--duration","5"},directory.path());
        QCOMPARE(scheduled.code,0);
        const auto taskId = scheduled.json["data"].toObject()["id"].toString();
        const auto stopScheduler = qScopeGuard([&] { run({"schedule","unwatch"},directory.path()); QTest::qWait(300); });
        QCOMPARE(run({"player","stop","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        QTest::qWait(450);
        QCOMPARE(run({"schedule","run",taskId},directory.path()).code,0);
        bool playing = false;
        timer.restart();
        while (timer.elapsed() < 10000) {
            const auto status = run({"schedule","status"},directory.path());
            if (status.json["data"].toObject()["state"].toString() == "playing") { playing = true; break; }
            QTest::qWait(100);
        }
        QVERIFY(playing);
        QCOMPARE(run({"service","update",emby,"--private-mode","true","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        const auto hiddenSchedule = run({"schedule","status"},directory.path());
        QVERIFY(hiddenSchedule.code == 3 || (hiddenSchedule.code == 0 && hiddenSchedule.json["data"].toObject()["serviceName"].toString().isEmpty()));
        QCOMPARE(run({"service","update",emby,"--private-mode","false","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        QCOMPARE(run({"schedule","run",taskId},directory.path()).code,0);
        QCOMPARE(run({"schedule","remove",taskId,"--yes"},directory.path()).code,0);
        QTRY_VERIFY(run({"schedule","status"},directory.path()).json["data"].toObject()["state"].toString() == "idle");
        QCOMPARE(run({"schedule","stop"},directory.path()).code,0);
        QCOMPARE(run({"schedule","unwatch"},directory.path()).code,0);
        QTRY_VERIFY(!QFileInfo::exists(directory.filePath("cli-schedule.lock")));
    }

    void videoRoundTripAndFailures()
    {
        const auto ffmpeg = EncryptedHlsPackager::locateFfmpegExecutable();
        if (ffmpeg.isEmpty()) QSKIP("External FFmpeg is unavailable");
        QTemporaryDir directory;
        const auto source = directory.filePath(QStringLiteral("原视频.mp4"));
        const auto cleanupPlayer = qScopeGuard([&] {
            if (run({"player","stop"},directory.path()).code == 3)
                if (run({"player","stop","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code == 3)
                    run({"player","stop","--private","--pin-env","CLI_TEST_NEW_PIN"},directory.path());
            QTest::qWait(400);
        });
        QProcess fixture;
        fixture.start(ffmpeg,{"-v","error","-f","lavfi","-i","testsrc2=size=96x64:rate=12","-t","1",
            "-c:v","libx264","-preset","ultrafast",source});
        QVERIFY(fixture.waitForFinished(15000)); QCOMPARE(fixture.exitCode(),0);
        QCOMPARE(run({"config","set","m3u8s/temporaryDirectory",directory.filePath("missing-old-webdav-temp")},directory.path()).code,0);
        auto played = run({"play",source,"--headless","--wait"},directory.path());
        QCOMPARE(played.code,0); QVERIFY(played.json["data"].toObject()["ended"].toBool());
        QCOMPARE(run({"player","volume","40"},directory.path()).code,0);
        QCOMPARE(run({"player","speed","1.5"},directory.path()).code,0);
        QCOMPARE(run({"player","pause"},directory.path()).code,0);
        QCOMPARE(run({"player","resume"},directory.path()).code,0);
        QCOMPARE(run({"player","seek","0"},directory.path()).code,0);
        QCOMPARE(run({"video","pack",source,"--ffmpeg",directory.filePath("missing-ffmpeg"),"--output",directory.filePath("pack")},directory.path()).code,4);
        auto packed = run({"video","pack",source,"--output",directory.filePath("pack"),"--video","copy","--audio","copy","--ffmpeg",ffmpeg},directory.path());
        QCOMPARE(packed.code,0);
        const auto items = packed.json.value("data").toObject().value("items").toArray();
        QCOMPARE(items.size(),1);
        const auto package = items[0].toObject().value("output").toString();
        QVERIFY(QFileInfo::exists(package));
        for (const auto& format : QStringList {"original","mkv","mp4"}) {
            auto restored = run({"video","restore",package,"--output",directory.filePath(format),"--format",format,"--ffmpeg",ffmpeg},directory.path());
            QCOMPARE(restored.code,0); QCOMPARE(restored.json.value("data").toObject().value("succeeded").toInt(),1);
            QVERIFY(QFileInfo::exists(restored.json.value("data").toObject().value("items").toArray()[0].toObject().value("output").toString()));
        }
        auto directoryPack = run({"video","pack",source,"--container","m3u8s","--video","copy","--audio","copy",
            "--ffmpeg",ffmpeg,"--output",directory.filePath("directory-pack")},directory.path());
        QCOMPARE(directoryPack.code,0);
        const auto manifest = directoryPack.json.value("data").toObject().value("items").toArray()[0].toObject().value("output").toString();
        QVERIFY(manifest.endsWith(".m3u8s"));
        QCOMPARE(run({"video","restore",manifest,"--output",directory.filePath("directory-restored"),"--ffmpeg",ffmpeg},directory.path()).code,0);
        QCOMPARE(run({"video","restore",directory.filePath("directory-pack"),"--output",directory.filePath("recursive-restored"),"--ffmpeg",ffmpeg},directory.path()).code,0);
        QCOMPARE(run({"video","pack",source,"--output",directory.filePath("explicit-invalid-temp"),"--temp",directory.filePath("missing-explicit-temp"),"--ffmpeg",ffmpeg},directory.path()).code,1);
        QCOMPARE(run({"video","pack",source,"--output",directory.filePath("invalid-segment"),"--segment-duration","31"},directory.path()).code,2);
        QCOMPARE(run({"config","set","m3u8s/temporaryDirectory",""},directory.path()).code,0);
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const auto id = addService(directory.path(),endpoint);
        endpoint.files["/dav/plain.mp4"] = read(source);
        QCOMPARE(run({"play",id,"/plain.mp4","--headless","--wait","--password-env","CLI_TEST_PASSWORD"},directory.path()).code,0);
        for (const auto& container : QStringList {"m3u8s","m3u8sp"}) {
            const auto remote = run({"video","pack",source,"--container",container,"--video","copy","--audio","copy",
                "--ffmpeg",ffmpeg,"--webdav",id,"--path","/video/" + container,"--password-env","CLI_TEST_PASSWORD"},directory.path());
            QCOMPARE(remote.code,0); QCOMPARE(remote.json.value("data").toObject().value("uploaded").toInt(),1);
            QVERIFY(!remote.json.value("data").toObject().value("localRetained").toBool());
            QVERIFY(std::ranges::any_of(endpoint.files.keys(),[&](const QString& path) {
                return path.startsWith("/dav/video/" + container + '/');
            }));
            auto restored = run({"video","restore","/video/" + container,"--service",id,"--password-env","CLI_TEST_PASSWORD",
                "--ffmpeg",ffmpeg,"--output",directory.filePath("remote-restored-" + container)},directory.path());
            QCOMPARE(restored.code,0);
        }
        const auto otherProfile = directory.filePath("no-keys");
        auto missingKey = run({"video","restore",package,"--output",directory.filePath("no-key-output"),"--ffmpeg",ffmpeg},otherProfile);
        QCOMPARE(missingKey.code,1);
        QVERIFY(write(directory.filePath("broken.mp4"),"not a video"));
        QCOMPARE(run({"play",directory.filePath("broken.mp4"),"--headless","--wait"},directory.path()).code,1);
        auto partial = run({"video","pack",source,directory.filePath("broken.mp4"),"--output",directory.filePath("partial"),"--video","copy","--ffmpeg",ffmpeg},directory.path());
        QCOMPARE(partial.code,6);
        QCOMPARE(partial.json.value("data").toObject().value("succeeded").toInt(),1);
        QCOMPARE(run({"config","get","m3u8s/videoEncoding"},directory.path()).json.value("data").toObject().value("m3u8s/videoEncoding").toString(),QString("h264"));
        endpoint.mediaFixture = true; endpoint.mediaFile = read(source);
        for (const auto& type : {"emby","jellyfin"}) {
            const auto mediaId = addService(directory.path(),endpoint,type,type);
            QCOMPARE(run({"service","login",mediaId,"--password-env","CLI_TEST_PASSWORD"},directory.path()).code,0);
            auto playedRemote = run({"play",mediaId,"movie","--headless","--wait"},directory.path());
            QCOMPARE(playedRemote.code,0); QVERIFY(!playedRemote.output.contains("test-session-token"));
        }
        QVERIFY(std::ranges::any_of(endpoint.requests,[](const Request& request) {
            return request.url.path().endsWith("/Sessions/Playing/Stopped");
        }));
        QCOMPARE(run({"privacy","set","--new-pin-env","CLI_TEST_PIN","--confirm-pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        const auto playlistPath = directory.filePath("private.m3u");
        QVERIFY(write(playlistPath,("#EXTM3U\n#EXTINF:-1,Private\n" + QUrl::fromLocalFile(source).toString() + '\n').toUtf8()));
        const auto privateCard = run({"service","add","--type","iptv","--name","Private IPTV","--playlist",playlistPath,
            "--private","--pin-env","CLI_TEST_PIN"},directory.path());
        QCOMPARE(privateCard.code,0);
        const auto privateId = privateCard.json["data"].toObject()["id"].toString();
        QCOMPARE(run({"play",privateId,"Private","--headless"},directory.path()).code,4);
        QCOMPARE(run({"play",privateId,"Private","--headless","--wait","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        QCOMPARE(run({"player","status"},directory.path()).code,3);
        QCOMPARE(run({"player","status","--private","--pin-env","CLI_TEST_PASSWORD"},directory.path()).code,3);
        QCOMPARE(run({"player","status","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,0);
        // Check an already-running host invalidates an old PIN immediately.
        QCOMPARE(run({"privacy","change","--pin-env","CLI_TEST_PIN","--new-pin-env","CLI_TEST_NEW_PIN",
            "--confirm-pin-env","CLI_TEST_NEW_PIN"},directory.path()).code,0);
        QCOMPARE(run({"player","status","--private","--pin-env","CLI_TEST_PIN"},directory.path()).code,3);
        QCOMPARE(run({"player","status","--private","--pin-env","CLI_TEST_NEW_PIN"},directory.path()).code,0);
        QCOMPARE(run({"privacy","change","--pin-env","CLI_TEST_NEW_PIN","--new-pin-env","CLI_TEST_PIN",
            "--confirm-pin-env","CLI_TEST_PIN"},directory.path()).code,0);
    }
};
int main(int argc,char* argv[])
{
    QCoreApplication application(argc,argv);
#ifdef Q_OS_WIN
    const auto arguments = application.arguments();
    if (arguments.size() == 3 && arguments[1] == "--send-test-interrupt") {
        FreeConsole();
        if (!AttachConsole(arguments[2].toULong())) return 1;
        SetConsoleCtrlHandler([](DWORD) -> BOOL { return TRUE; },TRUE);
        const bool sent = GenerateConsoleCtrlEvent(CTRL_C_EVENT,0);
        Sleep(200); // Let the asynchronous signal dispatch before detaching.
        FreeConsole();
        return sent ? 0 : 1;
    }
#endif
    CliIntegrationTest test;
    return QTest::qExec(&test,argc,argv);
}
#include "CliIntegrationTest.moc"
