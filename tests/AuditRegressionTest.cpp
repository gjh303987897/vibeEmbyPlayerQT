#include "database/SessionRepository.h"
#include "database/SqlTransaction.h"
#include "database/SessionTokenCipher.h"
#include "services/backup/TsslBackupService.h"
#include "services/emby/EmbyClient.h"
#include "services/iptv/IptvParser.h"
#include "services/webdav/WebDavClient.h"
#include "services/webdav/WebDavPlaybackProxy.h"
#include "services/webdav/TransferManager.h"
#include "cli/CliRuntime.h"
#include "services/ffmpeg/FfmpegProgress.h"
#include "utils/AppLogger.h"

#include <QFile>
#include <QHostAddress>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <QLoggingCategory>
#include <thread>
#include <chrono>
#include <QSemaphore>
#include <QtConcurrentRun>
#include <optional>

namespace {
QString uniqueId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray {};
}
class Endpoint final : public QTcpServer {
public:
    Endpoint()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto buffer = socket->property("buffer").toByteArray() + socket->readAll();
                    socket->setProperty("buffer", buffer);
                    const auto end = buffer.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    qint64 length = 0;
                    for (const auto& line : buffer.first(end).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                    }
                    if (buffer.size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    requests.append(buffer);
                    handler(socket, buffer);
                });
            }
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("http://localhost:%1/").arg(serverPort())); }
    static void respond(QTcpSocket* socket, QByteArray body, QByteArray status = "200 OK", QByteArray headers = {})
    {
        socket->write("HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) +
                      "\r\nConnection: close\r\n" + headers + "\r\n" + body);
        socket->disconnectFromHost();
    }
    QList<QByteArray> requests;
    std::function<void(QTcpSocket*, const QByteArray&)> handler;
};
TsslBackupTarget s3Target(const Endpoint& endpoint)
{
    TsslBackupTarget target;
    target.type = TsslBackupTarget::Type::S3;
    target.s3Endpoint = endpoint.url();
    target.s3Bucket = "test-bucket";
    target.s3Region = "us-east-1";
    target.s3AccessKey = "fake-access";
    target.s3SecretKey = "fake-secret";
    return target;
}
ServerConfig serverConfig(QString id = "server")
{
    return {.id = id, .name = id, .baseUrl = "http://localhost/", .username = "test", .serviceType = ServiceType::Emby};
}
class Parser : public MediaServerClientBase {
public:
    using MediaServerClientBase::parseItems;
    using MediaServerClientBase::parseLibraries;
};
}

class AuditRegressionTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
    }
    void privateHistoryStaysPrivate()
    {
        QTemporaryDir directory;
        SessionRepository repository(uniqueId(), directory.filePath("db.sqlite"));
        QVERIFY(repository.initialize());
        auto server = serverConfig();
        QVERIFY(repository.saveServer(server));
        const auto now = QDateTime::currentDateTimeUtc();
        for (const auto source : {PlaybackHistorySource::Emby, PlaybackHistorySource::Jellyfin,
                                  PlaybackHistorySource::Iptv, PlaybackHistorySource::WebDav}) {
            PlaybackHistoryItem item {.id = uniqueId(), .source = source, .serviceId = server.id,
                .serviceName = server.name, .replayTarget = uniqueId(), .title = "Private",
                .playedDate = now.toLocalTime().date(), .playedAt = now, .updatedAt = now, .privacyMode = true};
            QVERIFY(repository.savePlaybackHistory(item));
        }
        const auto ordinary = repository.loadPlaybackHistory(false, 0, 100);
        QVERIFY(ordinary && ordinary->empty());
        const auto all = repository.loadPlaybackHistory(true, 0, 100);
        QVERIFY(all && all->size() == 4);
        for (const auto& item : *all) QVERIFY(item.privacyMode);
        const auto dates = repository.loadPlaybackHistoryDates(false);
        QVERIFY(dates && dates->empty());
        const auto daily = repository.loadPlaybackHistoryForDate(false, now.toLocalTime().date());
        QVERIFY(daily && daily->empty());
        QVERIFY(repository.deletePlaybackHistoryForDate(false, now.toLocalTime().date()));
        QCOMPARE(repository.loadPlaybackHistory(true, 0, 100)->size(), size_t(4));
        // Moving a formerly ordinary service to private must also hide its history.
        PlaybackHistoryItem item {.id = uniqueId(), .source = PlaybackHistorySource::Emby,
            .serviceId = server.id, .serviceName = server.name, .replayTarget = "ordinary", .title = "Ordinary",
            .playedDate = now.toLocalTime().date(), .playedAt = now, .updatedAt = now};
        QVERIFY(repository.savePlaybackHistory(item));
        QVERIFY(repository.setServerPrivateMode(server.id, true));
        QVERIFY(repository.loadPlaybackHistory(false, 0, 100)->empty());
    }
    void commitFailureLeavesConnectionUsable()
    {
        QTemporaryDir directory;
        const auto writerId = uniqueId(), readerId = uniqueId();
        {
            auto writer = QSqlDatabase::addDatabase("QSQLITE", writerId);
            writer.setDatabaseName(directory.filePath("db.sqlite"));
            writer.setConnectOptions("QSQLITE_BUSY_TIMEOUT=0");
            QVERIFY(writer.open());
            QSqlQuery setup(writer);
            QVERIFY(setup.exec("CREATE TABLE value (n INTEGER)"));
            QVERIFY(setup.exec("INSERT INTO value VALUES (1)"));
            auto reader = QSqlDatabase::addDatabase("QSQLITE", readerId);
            reader.setDatabaseName(writer.databaseName());
            QVERIFY(reader.open());
            QSqlQuery held(reader);
            QVERIFY(held.exec("BEGIN"));
            QVERIFY(held.exec("SELECT * FROM value"));
            QVERIFY(held.next());
            SqlTransaction transaction(writer);
            QVERIFY(transaction.begin());
            QVERIFY(setup.exec("UPDATE value SET n=2"));
            QVERIFY(!transaction.commit());
            held.finish();
            QVERIFY(held.exec("ROLLBACK"));
            SqlTransaction next(writer);
            QVERIFY(next.begin());
            QVERIFY(setup.exec("SELECT n FROM value"));
            QVERIFY(setup.next());
            QCOMPARE(setup.value(0).toInt(), 1);
            setup.finish();
            QVERIFY(next.commit());
        }
        QSqlDatabase::removeDatabase(writerId);
        QSqlDatabase::removeDatabase(readerId);
    }
    void failedDeletionPreservesRelatedRows()
    {
        QTemporaryDir directory;
        const auto connection = uniqueId();
        SessionRepository repository(connection, directory.filePath("db.sqlite"));
        QVERIFY(repository.initialize());
        const auto server = serverConfig();
        UserSession session {.server = server, .userId = "user", .username = "test", .accessToken = "fake-token",
                             .createdAt = QDateTime::currentDateTimeUtc()};
        QVERIFY(repository.saveSession(session));
        {
            QSqlQuery query(QSqlDatabase::database(connection));
            QVERIFY(query.exec("CREATE TRIGGER fail_server_delete BEFORE DELETE ON servers BEGIN SELECT RAISE(ABORT, 'injected'); END"));
        }
        QVERIFY(!repository.deleteServer(server.id, true));
        const auto saved = repository.loadSession(server.id);
        QVERIFY(saved && saved->has_value());
        QCOMPARE(repository.loadServiceCards(true)->size(), size_t(1));
    }
    void reorderPreservesPrivateSlots()
    {
        QTemporaryDir directory;
        const auto connection = uniqueId();
        SessionRepository repository(connection, directory.filePath("db.sqlite"));
        QVERIFY(repository.initialize());
        for (const auto& id : {QString("A"), QString("B"), QString("C")}) {
            auto server = serverConfig(id);
            server.privateMode = id == "B";
            QVERIFY(repository.saveServer(server));
        }
        QVERIFY(repository.moveServerTo("C", 0, false));
        auto cards = repository.loadServiceCards(true);
        QVERIFY(cards && cards->size() == 3);
        QCOMPARE(cards->at(0).server.id, QString("C"));
        QCOMPARE(cards->at(1).server.id, QString("B"));
        QCOMPARE(cards->at(2).server.id, QString("A"));
        QSqlQuery query(QSqlDatabase::database(connection));
        QVERIFY(query.exec("SELECT COUNT(DISTINCT sort_order) FROM servers"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 3);
        query.finish();
        QVERIFY(repository.moveServer("A", -1, false));
        QCOMPARE(repository.loadServiceCards(true)->at(0).server.id, QString("A"));
    }
    void sessionMigrationAndEncryption()
    {
        QTemporaryDir directory;
        const auto connection = uniqueId(), path = directory.filePath("db.sqlite");
        SessionRepository repository(connection, path);
        QVERIFY(repository.initialize());
        auto server = serverConfig();
        QVERIFY(repository.saveServer(server));
        {
            QSqlQuery query(QSqlDatabase::database(connection));
            QVERIFY(query.exec("DROP TABLE sessions"));
            QVERIFY(query.exec("CREATE TABLE sessions (id INTEGER PRIMARY KEY, server_id TEXT, user_id TEXT, username TEXT, access_token TEXT, created_at TEXT)"));
            QVERIFY(query.exec("INSERT INTO sessions VALUES (1,'server','old','test','legacy-old-token','2026-01-01'),(2,'server','new','test','legacy-new-token','2026-02-01')"));
        }
        const auto initialized = repository.initialize();
        QVERIFY2(initialized.has_value(), qPrintable(initialized ? QString() : initialized.error()));
        const auto loaded = repository.loadSession(server.id);
        QVERIFY(loaded && loaded->has_value());
        QCOMPARE((**loaded).accessToken, QString("legacy-new-token"));
        QCOMPARE((**loaded).userId, QString("new"));
        QSqlQuery query(QSqlDatabase::database(connection));
        QVERIFY(query.exec("SELECT access_token FROM sessions"));
        QVERIFY(query.next());
#ifdef Q_OS_WIN
        QVERIFY(query.value(0).toString().startsWith(SessionTokenCipher::prefix));
        QVERIFY(!readFile(path).contains("legacy-new-token"));
        QVERIFY(!readFile(path).contains("legacy-old-token"));
#endif
        QVERIFY(!query.next());
        query.finish();
        SessionRepository readonly(uniqueId(), path, SessionRepository::OpenMode::ReadOnly);
        QCOMPARE((**readonly.loadSession(server.id)).accessToken, QString("legacy-new-token"));
        QVERIFY(repository.clearSession());
        QVERIFY(!repository.loadSession(server.id)->has_value());
    }
    void playlistReimportKeepsParentIdentity()
    {
        QTemporaryDir directory;
        SessionRepository repository(uniqueId(), directory.filePath("db.sqlite"));
        QVERIFY(repository.initialize());
        auto server = serverConfig(); server.serviceType = ServiceType::IPTV;
        IptvPlaylist playlist {.id = "original", .serviceId = server.id, .name = "Channels",
            .sourceType = "local", .sourcePath = "a.m3u", .importedPath = "a.m3u", .importedAt = "2026-10-07"};
        IptvChannel channel {.id = "channel", .playlistId = playlist.id, .name = "Old", .groupName = "TV", .streamUrl = "http://localhost/live"};
        QVERIFY(repository.saveIptvPlaylist(server, playlist, {channel}));
        playlist.id = "replacement"; channel.name = "New";
        QVERIFY(repository.saveIptvPlaylist(server, playlist, {channel}));
        QCOMPARE((**repository.loadIptvPlaylist(server.id)).id, QString("original"));
        QCOMPARE(repository.loadIptvChannels(server.id)->at(0).name, QString("New"));
    }
    void parsesQuotedPlaylistComma()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("channels.m3u");
        QVERIFY(writeFile(path, "#EXTM3U\n#EXTINF:-1 tvg-logo=\"https://example.com/a,b.png\" group-title=\"Movies\",Proper Name\nhttps://example.com/live\n"));
        const auto channels = IptvParser::parseFile(path);
        QVERIFY(channels && channels->size() == 1);
        QCOMPARE(channels->front().name, QString("Proper Name"));
        QCOMPARE(channels->front().groupName, QString("Movies"));
    }
    void malformedMediaListsFail_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("html") << QByteArray("<html>error</html>");
        QTest::newRow("broken") << QByteArray("{\"Items\":[");
        QTest::newRow("missing") << QByteArray("{}");
        QTest::newRow("wrong-type") << QByteArray("{\"Items\":null}");
        QTest::newRow("bad-entry") << QByteArray("{\"Items\":[1]}");
    }
    void malformedMediaListsFail()
    {
        QFETCH(QByteArray, body);
        QVERIFY(!Parser::parseItems(body, {}, {}));
        QVERIFY(!Parser::parseLibraries(body, {}, {}));
        const auto empty = Parser::parseItems("{\"Items\":[]}", {}, {});
        QVERIFY(empty && empty->empty());
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [body](QTcpSocket* socket, const QByteArray&) { Endpoint::respond(socket, body); };
        NetworkClient network; EmbyClient client(network);
        UserSession session {.server = serverConfig(), .userId = "user", .accessToken = "fake"};
        session.server.baseUrl = endpoint.url().toString();
        std::optional<ItemResult> result;
        client.searchVideoItems(session, "query", 0, 10, [&](auto value) { result = std::move(value); });
        QTRY_VERIFY(result.has_value());
        QVERIFY(!*result);
        QCOMPARE(result->error().kind, NetworkErrorKind::Parse);
    }
    void userCancellationIsNotTimeout()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket*, const QByteArray&) {};
        NetworkClient client;
        std::optional<NetworkResult> result;
        client.get(endpoint.url(), {}, false, [&](auto value) { result = std::move(value); });
        QTRY_COMPARE(endpoint.requests.size(), 1);
        client.cancelRequests();
        QTRY_VERIFY(result.has_value());
        QVERIFY(!*result);
        QCOMPARE(result->error().kind, NetworkErrorKind::Canceled);
        WebDavClient dav;
        auto server = serverConfig(); server.serviceType = ServiceType::WebDAV;
        server.baseUrl = endpoint.url().toString();
        std::optional<WebDavListResult> listing;
        dav.listDirectory(server, {}, endpoint.url(), [&](auto value) { listing = std::move(value); });
        QTRY_COMPARE(endpoint.requests.size(), 2);
        dav.cancelRequests();
        QTRY_VERIFY(listing.has_value());
        QCOMPARE(listing->error().kind, NetworkErrorKind::Canceled);
    }
    void postRedirectDoesNotResendCredentials()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray&) {
            Endpoint::respond(socket, {}, "302 Found", "Location: /other\r\n");
        };
        NetworkClient client;
        std::optional<NetworkResult> result;
        client.postJson(endpoint.url(), {}, {{"Password", "fake"}}, false, [&](auto value) { result = std::move(value); });
        QTRY_VERIFY(result.has_value());
        QVERIFY(!*result);
        QCOMPARE(result->error().httpStatus, 302);
        QCOMPARE(endpoint.requests.size(), 1);
    }
    void s3RejectsInvalidOrStalledPagination_data()
    {
        QTest::addColumn<QByteArray>("body"); QTest::addColumn<int>("requests");
        QTest::newRow("invalid-xml") << QByteArray("<broken") << 1;
        QTest::newRow("wrong-root") << QByteArray("<Error/>") << 1;
        QTest::newRow("missing-token") << QByteArray("<ListBucketResult><IsTruncated>true</IsTruncated></ListBucketResult>") << 1;
        QTest::newRow("repeated-token") << QByteArray("<ListBucketResult><IsTruncated>true</IsTruncated><NextContinuationToken>same</NextContinuationToken></ListBucketResult>") << 2;
    }
    void s3RejectsInvalidOrStalledPagination()
    {
        QFETCH(QByteArray, body); QFETCH(int, requests);
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [body](QTcpSocket* socket, const QByteArray&) { Endpoint::respond(socket, body); };
        TsslBackupService service;
        std::optional<TsslBackupRestoreResult> result;
        service.restore(s3Target(endpoint), [&](auto value) { result = std::move(value); });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
        QVERIFY(!*result);
        QVERIFY(!service.isRunning());
        QCOMPARE(endpoint.requests.size(), requests);
    }
    void s3RetriesAreBounded()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray&) { Endpoint::respond(socket, {}, "503 Unavailable"); };
        TsslBackupService service;
        std::optional<TsslBackupRestoreResult> result;
        service.restore(s3Target(endpoint), [&](auto value) { result = std::move(value); });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
        QVERIFY(!*result);
        QCOMPARE(endpoint.requests.size(), 3);
        endpoint.handler = [](QTcpSocket* socket, const QByteArray&) { Endpoint::respond(socket, "<ListBucketResult><IsTruncated>false</IsTruncated></ListBucketResult>"); };
        result.reset();
        service.restore(s3Target(endpoint), [&](auto value) { result = std::move(value); });
        QTRY_VERIFY(result.has_value());
        QVERIFY(*result && result->value().empty());
    }
    void webDavMutationsUseDigestCredentials()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray& request) {
            if (request.contains("Authorization: Digest") && request.contains("username=\"test\"")) {
                Endpoint::respond(socket, {}, "204 No Content");
            } else {
                Endpoint::respond(socket, {}, "401 Unauthorized",
                    "WWW-Authenticate: Digest realm=\"test\", nonce=\"0123456789\", algorithm=MD5, qop=\"auth\"\r\n");
            }
        };
        auto server = serverConfig(); server.serviceType = ServiceType::WebDAV;
        server.baseUrl = endpoint.url().toString();
        WebDavClient client;
        for (const auto operation : {WebDavClient::Mutation::Remove, WebDavClient::Mutation::Move, WebDavClient::Mutation::Copy}) {
            std::optional<WebDavVoidResult> result;
            client.mutate(server, "fake-password", operation, endpoint.url().resolved(QUrl("source")),
                endpoint.url().resolved(QUrl("target")), false, [&](auto value) { result = std::move(value); });
            QTRY_VERIFY(result.has_value());
            QVERIFY2(result->has_value(), qPrintable(result->has_value() ? QString() : result->error().message));
        }
    }
    void retainedPartialResumes_data()
    {
        QTest::addColumn<bool>("networkFailure");
        QTest::newRow("pause") << false;
        QTest::newRow("automatic-retry") << true;
    }
    void retainedPartialResumes()
    {
        QFETCH(bool, networkFailure);
        QTemporaryDir directory;
        const auto path = directory.filePath("partial.bin");
        QByteArray payload(64 * 1024, '\0');
        for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = char(i % 251);
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [&](QTcpSocket* socket, const QByteArray& request) {
            qint64 start = 0;
            for (const auto& line : request.split('\n')) {
                if (line.toLower().startsWith("range:")) start = line.trimmed().mid(13).split('-').first().toLongLong();
            }
            const bool ranged = start > 0;
            socket->write(ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n");
            if (ranged) socket->write("Content-Range: bytes " + QByteArray::number(start) + '-' +
                QByteArray::number(payload.size() - 1) + '/' + QByteArray::number(payload.size()) + "\r\n");
            socket->write("Content-Length: " + QByteArray::number(payload.size() - start) + "\r\nETag: \"stable\"\r\nConnection: close\r\n\r\n");
            auto* timer = new QTimer(socket);
            timer->setInterval(10);
            const bool interrupt = networkFailure && endpoint.requests.size() == 1;
            connect(timer, &QTimer::timeout, socket, [&, socket, timer, offset = start, interrupt]() mutable {
                const auto count = std::min<qint64>(2048, payload.size() - offset);
                socket->write(payload.constData() + offset, count);
                offset += count;
                if (interrupt && offset >= 8192) { timer->stop(); socket->disconnectFromHost(); }
                else if (offset == payload.size()) { timer->stop(); socket->disconnectFromHost(); }
            });
            timer->start();
        };
        TransferManager manager;
        auto server = serverConfig(); server.serviceType = ServiceType::WebDAV;
        const auto id = manager.enqueueDownload(server, {}, endpoint.url(), path, payload.size(), true, "\"stable\"");
        if (!networkFailure) {
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(path).size() >= 8192, 3000);
            manager.pauseTask(id);
            QTRY_COMPARE_WITH_TIMEOUT(manager.tasks()->data(manager.tasks()->index(0), TransferTaskListModel::StatusRole).toString(), QString("paused"), 3000);
            QVERIFY(QFileInfo(path).size() >= 8192);
            manager.resumeTask(id);
        }
        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 10000);
        QCOMPARE(endpoint.requests.size(), 2);
        QVERIFY(endpoint.requests.at(1).contains("Range: bytes="));
        QVERIFY(endpoint.requests.at(1).contains("If-Range: \"stable\""));
        QCOMPARE(readFile(path), payload);
    }
    void backupTimeoutTracksProgress_data()
    {
        QTest::addColumn<bool>("totalDeadline");
        QTest::newRow("active-slow-transfer") << false;
        QTest::newRow("total-deadline") << true;
    }
    void changedEtagIsNotAppendedToPartialDownload()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("partial.bin");
        QVERIFY(writeFile(path, "old"));
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray&) {
            Endpoint::respond(socket, "new", "206 Partial Content", "Content-Range: bytes 3-5/6\r\nETag: \"changed\"\r\n");
        };
        TransferManager manager;
        auto server = serverConfig(); server.serviceType = ServiceType::WebDAV;
        manager.enqueueDownload(server, {}, endpoint.url(), path, 6, true, "\"original\"");
        QTRY_COMPARE_WITH_TIMEOUT(manager.failedCount(), 1, 3000);
        QCOMPARE(readFile(path), QByteArray("old"));
        QCOMPARE(endpoint.requests.size(), 1);
        QVERIFY(endpoint.requests.first().contains("If-Match: \"original\""));
    }
    void backupTimeoutTracksProgress()
    {
        QFETCH(bool, totalDeadline);
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray& request) {
            if (request.contains("list-type=2")) {
                Endpoint::respond(socket, "<ListBucketResult><IsTruncated>false</IsTruncated><Contents><Key>test.tssl</Key></Contents></ListBucketResult>");
            } else {
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 120\r\nConnection: close\r\n\r\n");
                auto* timer = new QTimer(socket);
                timer->setInterval(150);
                connect(timer, &QTimer::timeout, socket, [socket, timer, count = 0]() mutable {
                    socket->write("0123456789");
                    if (++count == 12) { timer->stop(); socket->disconnectFromHost(); }
                });
                timer->start();
            }
        };
        TsslBackupService service(TsslBackupNetworkPolicy {600, totalDeadline ? 800 : 5000, 0});
        std::optional<TsslBackupRestoreResult> result;
        service.restore(s3Target(endpoint), [&](auto value) { result = std::move(value); });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 4000);
        if (totalDeadline) {
            QVERIFY(!*result);
            QVERIFY(result->error().contains("time limit"));
        } else {
            QVERIFY2(result->has_value(), qPrintable(result->has_value() ? QString() : result->error()));
            QCOMPARE(result->value().size(), 1);
            QCOMPARE(readFile(result->value().first()).size(), 120);
        }
        service.cleanupRestoreFiles();
    }
    void proxyRejectsOversizedHeader()
    {
        WebDavPlaybackProxy proxy;
        auto server = serverConfig(); server.serviceType = ServiceType::WebDAV;
        const auto url = proxy.streamUrlFor(server, {}, QUrl("http://localhost/file"));
        QTcpSocket socket;
        socket.connectToHost(url.host(), url.port());
        QVERIFY(socket.waitForConnected(1000));
        auto request = "GET " + url.path().toUtf8() + " HTTP/1.1\r\nX: ";
        request += QByteArray(64 * 1024 - request.size(), 'x');
        socket.write(request);
        socket.flush();
        QByteArray response;
        connect(&socket, &QTcpSocket::readyRead, &socket, [&] { response += socket.readAll(); });
        QTRY_VERIFY2_WITH_TIMEOUT(response.contains("431"), qPrintable(QString::fromLatin1(response) + socket.errorString()), 3000);
        QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState);
    }
    void interruptedAwaitAllowsLateCompletion()
    {
        CliConsole::canceled.store(false);
        std::function<void(std::expected<int, QString>)> late;
        int cancellations = 0;
        const auto result = Cli::await<std::expected<int, QString>>([&](auto complete) {
            late = complete;
            QTimer::singleShot(0, [] { QCoreApplication::exit(); });
        }, [&] { ++cancellations; });
        QVERIFY(!result);
        QCOMPARE(cancellations, 1);
        late(42);
        CliConsole::canceled.store(false);
    }
    void malformedFfmpegProgressIsIgnored()
    {
        QVERIFY(!FfmpegProgress::parseLine(""));
        QVERIFY(!FfmpegProgress::parseLine("diagnostic without delimiter"));
        QVERIFY(!FfmpegProgress::parseLine("=no-key"));
        const auto field = FfmpegProgress::parseLine("out_time_us=123456\r");
        QVERIFY(field.has_value());
        QCOMPARE(field->key, QByteArray("out_time_us"));
        QCOMPARE(field->value, QByteArray("123456"));
    }
    void interruptedFutureFinishesBeforeInputsAreReleased()
    {
        CliConsole::canceled.store(false);
        QSemaphore release;
        std::atomic_bool completed = false;
        auto future = QtConcurrent::run([&]() -> std::expected<int, QString> {
            release.acquire();
            completed.store(true);
            return 42;
        });
        // A previous quit may make exec return immediately. Completion must
        // not depend on processing another event in the loop being shut down.
        std::jthread completion([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            release.release();
        });
        QTimer::singleShot(0, [] { QCoreApplication::exit(); });
        const auto result = Cli::awaitFuture(std::move(future));
        Q_UNUSED(result)
        QVERIFY(completed.load());
        CliConsole::canceled.store(false);
    }
    void fileLogsRotateAndSerializeWrites()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("app.log");
        QVERIFY(writeFile(path, QByteArray(10 * 1024 * 1024 - 4, 'x')));
        const auto previous = qgetenv("VIBEPLAYER_LOG_FILE");
        qputenv("VIBEPLAYER_LOG_FILE", path.toUtf8());
        QLoggingCategory::setFilterRules("vibeplayer.info=false");
        std::vector<std::thread> writers;
        for (int writer = 0; writer < 4; ++writer) writers.emplace_back([writer] {
            for (int i = 0; i < 200; ++i) AppLogger::info("concurrent", QStringLiteral("writer=%1 item=%2").arg(writer).arg(i));
        });
        for (auto& writer : writers) writer.join();
        if (previous.isNull()) qunsetenv("VIBEPLAYER_LOG_FILE");
        else qputenv("VIBEPLAYER_LOG_FILE", previous);
        QLoggingCategory::setFilterRules({});
        QVERIFY(QFileInfo::exists(path + ".1"));
        QVERIFY(QFileInfo(path).size() < 10 * 1024 * 1024);
        const auto lines = readFile(path).trimmed().split('\n');
        QCOMPARE(lines.size(), 800);
        QSet<QByteArray> unique;
        for (const auto& line : lines) {
            QVERIFY(line.startsWith("info [concurrent] writer="));
            unique.insert(line);
        }
        QCOMPARE(unique.size(), 800);
    }
    void deletedScheduleIsRemovedFromPendingIds()
    {
        QTemporaryDir directory;
        SessionRepository repository(uniqueId(), directory.filePath("db.sqlite"));
        QVERIFY(repository.initialize());
        QVERIFY(repository.saveServer(serverConfig()));
        ScheduledPlaybackTask task {.id = "deleted", .serverId = "server", .scheduleType = "daily",
            .startTime = "12:00", .durationMinutes = 5, .enabled = true};
        QVERIFY(repository.saveScheduledPlaybackTask(task));
        repository.setPendingMissedScheduledPlaybackTaskIds({"deleted", "hidden-private-task"});
        QVERIFY(repository.deleteScheduledPlaybackTask(task.id));
        QCOMPARE(repository.pendingMissedScheduledPlaybackTaskIds(), QStringList {"hidden-private-task"});
    }
    void disconnectedProxyClientsAreSafe()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray&) {
            QTimer::singleShot(100, socket, [socket] { Endpoint::respond(socket, QByteArray(1024, 'x')); });
        };
        WebDavPlaybackProxy proxy;
        auto server = serverConfig(); server.serviceType = ServiceType::WebDAV;
        const auto url = proxy.streamUrlFor(server, {}, endpoint.url());
        for (int i = 0; i < 10; ++i) {
            QTcpSocket socket;
            socket.connectToHost(url.host(), url.port());
            QVERIFY(socket.waitForConnected(1000));
            socket.write("GET " + url.path().toUtf8() + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
            QTRY_COMPARE_WITH_TIMEOUT(endpoint.requests.size(), i + 1, 3000);
            socket.abort();
            QTest::qWait(10);
        }
        QTest::qWait(200);
    }
private:
    QTemporaryDir m_settings;
};
QTEST_GUILESS_MAIN(AuditRegressionTest)
#include "AuditRegressionTest.moc"
