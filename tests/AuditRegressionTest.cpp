#include "database/SessionRepository.h"
#include "database/SqlTransaction.h"
#include "database/SessionTokenCipher.h"
#include "services/backup/TsslBackupService.h"
#include "services/emby/EmbyClient.h"
#include "services/emby/EmbyRecommendationCache.h"
#include "services/scheduler/ScheduledPlaybackManager.h"
#include "services/webdav/BoundedReplyWriter.h"
#include "services/webdav/HlsManifestValidator.h"
#include "utils/JsonUtils.h"
#include "network/SelfSignedTlsPolicy.h"
#include "player/PlayerController.h"
#include "services/smb/SmbClient.h"
#include "services/management/ServiceCardLoader.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include "services/iptv/IptvParser.h"
#include "services/webdav/WebDavClient.h"
#include "services/webdav/WebDavPlaybackProxy.h"
#include "services/webdav/TransferManager.h"
#include "cli/CliRuntime.h"
#include "services/ffmpeg/FfmpegProgress.h"
#include "utils/AppLogger.h"
#include "utils/ErrorPresentation.h"

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
#include <QSslSocket>
#include <QSslCertificate>
#include <QSslKey>
#include <QSignalSpy>
#include <QtConcurrentRun>
#include <optional>

namespace {
QStringList capturedLogMessages;
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

class TlsEndpoint final : public QTcpServer {
public:
    QSslCertificate certificate;
    QSslKey key;
    QList<QByteArray> requests;
    QByteArray responseBody {"video-bytes"};
    QByteArray responseStatus {"206 Partial Content"};
    QByteArray responseHeaders {"Content-Range: bytes 0-10/11\r\n"};
protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) { socket->deleteLater(); return; }
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QSslSocket::readyRead, this, [this, socket]() {
            auto bytes = socket->property("request").toByteArray() + socket->readAll();
            socket->setProperty("request", bytes);
            if (!bytes.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
            socket->setProperty("handled", true);
            requests.append(bytes);
            Endpoint::respond(socket, responseBody, responseStatus, responseHeaders);
        });
        socket->startServerEncryption();
    }
};
}

class AuditRegressionTest final : public QObject {
    Q_OBJECT
private slots:
    void transfersAnnounceOnlySuccessfulRemoteWrites()
    {
        QTemporaryDir temporary;
        const auto source = temporary.filePath("source.bin");
        QVERIFY(writeFile(source, "test-upload"));
        Endpoint endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray& request) {
            if (request.startsWith("MKCOL")) Endpoint::respond(socket, {}, "409 Conflict");
            else Endpoint::respond(socket, "test-download");
        };
        auto server = serverConfig();
        server.serviceType = ServiceType::WebDAV;
        server.baseUrl = endpoint.url().toString();
        TransferManager manager;
        QSignalSpy writes(&manager, &TransferManager::remoteWriteCompleted);
        QSignalSpy finished(&manager, &TransferManager::taskFinished);
        const auto upload = endpoint.url().resolved(QUrl("uploaded.bin"));
        manager.enqueueUpload(server, {}, source, upload, QFileInfo(source).size());
        manager.enqueueDownload(server, {}, endpoint.url().resolved(QUrl("download.bin")),
                                temporary.filePath("downloaded.bin"), -1);
        manager.enqueueCreateDirectory(server, {}, endpoint.url().resolved(QUrl("folder/")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 5000);
        QCOMPARE(writes.size(), 1);
        QCOMPARE(writes.front()[0].toString(), server.id);
        QCOMPARE(writes.front()[1].toUrl(), upload);
    }
    void playbackProxyValidatesRealCertificates_data()
    {
        QTest::addColumn<QString>("certificateName");
        QTest::addColumn<bool>("trustSelfSigned");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("self-signed-disabled") << QString("valid") << false << false;
        QTest::newRow("self-signed-enabled") << QString("valid") << true << true;
        QTest::newRow("wrong-host-rejected") << QString("wrong-host") << true << false;
        QTest::newRow("expired-rejected") << QString("expired") << true << false;
        QTest::newRow("missing-issuer-disabled") << QString("private-ca-valid") << false << false;
        QTest::newRow("missing-issuer-enabled") << QString("private-ca-valid") << true << true;
        QTest::newRow("missing-issuer-wrong-host-rejected") << QString("private-ca-wrong-host") << true << false;
        QTest::newRow("missing-issuer-expired-rejected") << QString("private-ca-expired") << true << false;
    }
    void playbackProxyValidatesRealCertificates()
    {
        QFETCH(QString, certificateName);
        QFETCH(bool, trustSelfSigned);
        QFETCH(bool, accepted);
        QVERIFY2(QSslSocket::supportsSsl(), "A Qt TLS backend is required for this regression test");
        const auto fixtureDirectory = QFileInfo(QFINDTESTDATA("data/tls/valid.pem")).absolutePath();
        TlsEndpoint endpoint;
        endpoint.certificate = QSslCertificate(readFile(QDir(fixtureDirectory).filePath(certificateName + ".pem")));
        endpoint.key = QSslKey(readFile(QDir(fixtureDirectory).filePath("test-key.pem")), QSsl::Rsa);
        QVERIFY(!endpoint.certificate.isNull());
        QVERIFY(!endpoint.key.isNull());
        QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
        auto server = serverConfig();
        server.username.clear();
        server.trustSelfSignedCertificate = trustSelfSigned;
        const auto remote = QUrl(QString("https://localhost:%1/stream?api_key=test-token").arg(endpoint.serverPort()));
        WebDavPlaybackProxy proxy;
        const auto local = proxy.streamUrlFor(server, {}, remote);
        QVERIFY(local != remote);
        QVERIFY(!local.toString().contains("test-token"));
        QNetworkAccessManager manager;
        QNetworkRequest request(local);
        request.setRawHeader("Range", "bytes=0-10");
        auto* reply = manager.get(request);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), accepted ? 206 : 502);
        QCOMPARE(reply->readAll(), accepted ? QByteArray("video-bytes") : QByteArray());
        QCOMPARE(endpoint.requests.size(), accepted ? 1 : 0);
        if (accepted) {
            QVERIFY(endpoint.requests.front().contains("Range: bytes=0-10"));
            QVERIFY(endpoint.requests.front().contains("api_key=test-token"));
            QVERIFY(!endpoint.requests.front().contains("Authorization: Basic"));
        }
        reply->deleteLater();
        proxy.revoke(local.path().section('/', 1, 1));
    }
    void webDavLoginValidatesRealCertificates_data()
    {
        playbackProxyValidatesRealCertificates_data();
    }
    void webDavLoginValidatesRealCertificates()
    {
        QFETCH(QString, certificateName);
        QFETCH(bool, trustSelfSigned);
        QFETCH(bool, accepted);
        QVERIFY(QSslSocket::supportsSsl());
        const auto fixtureDirectory = QFileInfo(QFINDTESTDATA("data/tls/valid.pem")).absolutePath();
        TlsEndpoint endpoint;
        endpoint.certificate = QSslCertificate(readFile(QDir(fixtureDirectory).filePath(certificateName + ".pem")));
        endpoint.key = QSslKey(readFile(QDir(fixtureDirectory).filePath("test-key.pem")), QSsl::Rsa);
        QVERIFY(!endpoint.certificate.isNull());
        QVERIFY(!endpoint.key.isNull());
        endpoint.responseStatus = "207 Multi-Status";
        endpoint.responseHeaders = "Content-Type: application/xml\r\n";
        endpoint.responseBody = "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>/dav/video.mp4</D:href>"
            "<D:propstat><D:prop><D:displayname>video.mp4</D:displayname><D:resourcetype/>"
            "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>";
        QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
        auto server = serverConfig();
        server.serviceType = ServiceType::WebDAV;
        server.baseUrl = QString("https://localhost:%1/dav/").arg(endpoint.serverPort());
        server.trustSelfSignedCertificate = trustSelfSigned;
        WebDavClient client;
        std::optional<WebDavListResult> result;
        client.listDirectory(server, "test-password", QUrl(server.baseUrl),
                             [&](WebDavListResult value) { result = std::move(value); });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 5000);
        QCOMPARE(result->has_value(), accepted);
        QCOMPARE(endpoint.requests.size(), accepted ? 1 : 0);
        if (accepted) {
            QCOMPARE(result->value().size(), 1);
            QCOMPARE(result->value().front().name, QString("video.mp4"));
            QVERIFY(endpoint.requests.front().startsWith("PROPFIND /dav/"));
        }
    }
    void playerRejectsDisablingCertificateValidation()
    {
        PlayerController player;
        QVERIFY(player.initializeHeadless());
        QSignalSpy errors(&player, &PlayerController::errorOccurred);
        player.playUrl("https://localhost/video", 0, {}, {}, true);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.front().front().toString().contains("validated playback proxy"));
        player.shutdown();
    }
    void configEditsPreserveUsageAndFailedLoginRollsBack()
    {
        QTemporaryDir temporary;
        const auto connection = uniqueId();
        SessionRepository repository(connection, temporary.filePath("session.db"));
        QVERIFY(repository.initialize());
        auto server = serverConfig();
        QVERIFY(repository.saveServer(server));
        QSqlQuery query(QSqlDatabase::database(connection));
        QVERIFY(query.exec("UPDATE servers SET last_used_at = '2000-01-01T00:00:00Z'"));
        server.name = "renamed";
        server.privateMode = true;
        QVERIFY(repository.saveServer(server));
        auto cards = repository.loadAllServiceCards();
        QVERIFY(cards);
        QCOMPARE(cards->size(), size_t(1));
        QCOMPARE(cards->front().lastUsedAt, QString("2000-01-01T00:00:00Z"));
        QCOMPARE(cards->front().server.name, QString("renamed"));
        UserSession session {.server = server, .userId = "user", .username = server.username,
                             .accessToken = "test-token", .createdAt = QDateTime::currentDateTimeUtc()};
        QVERIFY(repository.saveSession(session));
        cards = repository.loadAllServiceCards();
        QVERIFY(cards);
        QVERIFY(cards->front().lastUsedAt != "2000-01-01T00:00:00Z");
        const auto savedUsed = cards->front().lastUsedAt;
        QVERIFY(query.exec("CREATE TRIGGER reject_session BEFORE INSERT ON sessions BEGIN SELECT RAISE(ABORT, 'test failure'); END"));
        session.server.name = "must roll back";
        QVERIFY(!repository.saveSession(session));
        cards = repository.loadAllServiceCards();
        QVERIFY(cards);
        QCOMPARE(cards->front().lastUsedAt, savedUsed);
        QCOMPARE(cards->front().server.name, QString("renamed"));
        const auto savedSession = repository.loadSession(server.id);
        QVERIFY(savedSession);
        QVERIFY(savedSession->has_value());
        QCOMPARE((**savedSession).accessToken, QString("test-token"));
    }
    void serviceCardRefreshStaysResponsiveAndPublishesLatestSnapshot()
    {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("cards.db");
        const auto connection = uniqueId();
        SessionRepository repository(connection, path);
        QVERIFY(repository.initialize());
        auto server = serverConfig();
        QVERIFY(repository.saveServer(server));
        auto privateServer = serverConfig("private"); privateServer.privateMode = true;
        QVERIFY(repository.saveServer(privateServer));
        QSqlQuery lock(QSqlDatabase::database(connection));
        QVERIFY(lock.exec("BEGIN EXCLUSIVE"));
        const auto rollback = qScopeGuard([&] { lock.exec("ROLLBACK"); });
        ServiceCardLoader loader(nullptr, path);
        int ready = 0, heartbeats = 0;
        ServiceCardLoader::Cards received;
        QString error;
        connect(&loader, &ServiceCardLoader::ready, this, [&](const auto& cards) {
            QCOMPARE(QThread::currentThread(), loader.thread());
            received = cards; ++ready;
        });
        connect(&loader, &ServiceCardLoader::failed, this, [&](const auto& message) { error = message; });
        QTimer heartbeat; heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++heartbeats; });
        heartbeat.start();
        QElapsedTimer elapsed; elapsed.start();
        loader.refresh();
        QVERIFY(elapsed.elapsed() < 100);
        QTimer::singleShot(50, &loader, [&] { loader.refresh(); loader.refresh(); });
        QTimer::singleShot(200, &loader, [&] {
            QVERIFY(lock.exec("COMMIT"));
            server.name = "latest";
            QVERIFY(repository.saveServer(server));
            loader.refresh();
        });
        QTRY_VERIFY_WITH_TIMEOUT(ready == 1 || !error.isEmpty(), 5000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(received.size(), size_t(2));
        QCOMPARE(received.front().server.name, QString("latest"));
        QVERIFY(received.back().server.privateMode);
        QVERIFY(heartbeats >= 10);
        QTest::qWait(100);
        QCOMPARE(ready, 1);
    }
    void serviceCardRefreshMissingAndMalformedDatabase()
    {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("missing.db");
        ServiceCardLoader loader(nullptr, path);
        int ready = 0; QString error;
        connect(&loader, &ServiceCardLoader::ready, this, [&](const auto& cards) { QVERIFY(cards.empty()); ++ready; });
        connect(&loader, &ServiceCardLoader::failed, this, [&](const auto& message) { error = message; });
        loader.refresh();
        QTRY_COMPARE(ready, 1);
        QVERIFY(!QFile::exists(path));
        const QByteArray invalid("not a SQLite database");
        QVERIFY(writeFile(path, invalid));
        loader.refresh();
        QTRY_VERIFY(!error.isEmpty());
        QCOMPARE(ready, 1);
        QCOMPARE(readFile(path), invalid);
    }
    void historyReadsDoNotRequireAWriteLock()
    {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("history.db");
        const auto connection = uniqueId();
        SessionRepository writer(connection, path);
        QVERIFY(writer.initialize());
        QSqlQuery query(QSqlDatabase::database(connection));
        QVERIFY(query.exec("INSERT INTO playback_history (id,source_type,service_id,service_name,replay_target,title,subtitle,played_date,played_at,updated_at) "
                           "VALUES ('expired','Local','','','/tmp/a','old','','2000-01-01','2000-01-01T00:00:00Z','2000-01-01T00:00:00Z')"));
        QVERIFY(query.exec("EXPLAIN QUERY PLAN DELETE FROM playback_history WHERE played_date < '2001-01-01'"));
        QVERIFY(query.next());
        QVERIFY(query.value(3).toString().contains("idx_playback_history_date"));
        QVERIFY(query.exec("BEGIN IMMEDIATE"));
        const auto rollback = qScopeGuard([&] { query.exec("ROLLBACK"); });
        SessionRepository reader(uniqueId(), path, SessionRepository::OpenMode::ReadOnly);
        QElapsedTimer elapsed;
        elapsed.start();
        const auto history = reader.loadPlaybackHistory(true, 0, 50);
        QVERIFY2(history.has_value(), qPrintable(history ? QString() : history.error()));
        QVERIFY(history->empty());
        const auto dates = reader.loadPlaybackHistoryDates(true);
        QVERIFY(dates.has_value());
        QVERIFY(dates->empty());
        const auto day = reader.loadPlaybackHistoryForDate(true, QDate(2000,1,1));
        QVERIFY(day.has_value());
        QVERIFY(day->empty());
        const auto links = reader.loadLinkPlaybackHistory(true);
        QVERIFY(links.has_value());
        QVERIFY(links->empty());
        const auto usage = reader.loadDailyUsageStats(true);
        QVERIFY(usage.has_value());
        QVERIFY(usage->empty());
        QVERIFY(elapsed.elapsed() < 1500);
        QVERIFY(query.exec("SELECT COUNT(*) FROM playback_history"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1); // Reads filter expired entries without erasing them.
    }
    void selfSignedTrustDoesNotWaiveOtherTlsChecks()
    {
        const QList<QSslError> selfSigned {QSslError(QSslError::SelfSignedCertificate)};
        QVERIFY(SelfSignedTlsPolicy::allows(selfSigned, true));
        QVERIFY(!SelfSignedTlsPolicy::allows(selfSigned, false));
        QVERIFY(!SelfSignedTlsPolicy::allows({}, true));
        const QList<QSslError> missingIssuer {
            QSslError(QSslError::UnableToGetLocalIssuerCertificate),
            QSslError(QSslError::UnableToVerifyFirstCertificate)};
        QVERIFY(SelfSignedTlsPolicy::allows(missingIssuer, true));
        QVERIFY(!SelfSignedTlsPolicy::allows(missingIssuer, false));
        QVERIFY(SelfSignedTlsPolicy::allows({QSslError(QSslError::UnableToGetIssuerCertificate),
                                            QSslError(QSslError::CertificateUntrusted)}, true));
        for (auto error : {QSslError::HostNameMismatch, QSslError::CertificateExpired,
                           QSslError::CertificateNotYetValid, QSslError::CertificateRevoked,
                           QSslError::NoPeerCertificate, QSslError::CertificateSignatureFailed,
                           QSslError::InvalidCaCertificate, QSslError::CertificateRejected,
                           QSslError::InvalidPurpose, QSslError::PathLengthExceeded,
                           QSslError::CertificateBlacklisted, QSslError::UnspecifiedError}) {
            auto mixed = selfSigned;
            mixed.append(QSslError(error));
            QVERIFY(!SelfSignedTlsPolicy::allows(mixed, true));
            auto mixedMissingIssuer = missingIssuer;
            mixedMissingIssuer.append(QSslError(error));
            QVERIFY(!SelfSignedTlsPolicy::allows(mixedMissingIssuer, true));
        }
    }
    void unchangedTracksPreserveTheModel()
    {
        TrackListModel model;
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        std::vector<TrackInfo> tracks {{.id = 1, .type = "audio", .title = "Main", .selected = true}};
        model.setTracks(tracks);
        QCOMPARE(resets.size(), 1);
        model.setTracks(tracks);
        QCOMPARE(resets.size(), 1);
        tracks.front().selected = false;
        model.setTracks(tracks);
        QCOMPARE(resets.size(), 2);
        QVERIFY(!model.trackAt(0)->selected);
    }
    void playbackWithoutInitializationReportsAnError()
    {
        PlayerController player;
        QSignalSpy errors(&player, &PlayerController::errorOccurred);
        player.playUrl("file:///missing.mp4");
        QCOMPARE(errors.size(), 1);
        QVERIFY(!player.loading());
    }
    void canceledFutureKeepsProcessingEvents()
    {
        CliConsole::canceled.store(false);
        QSemaphore release;
        auto future = QtConcurrent::run([&] { release.acquire(); return 42; });
        int heartbeats = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, &timer, [&] { ++heartbeats; });
        timer.start(5);
        QTimer::singleShot(10, this, [] { CliConsole::canceled.store(true); });
        QTimer::singleShot(180, this, [&] { release.release(); });
        QCOMPARE(Cli::awaitFuture(std::move(future)), 42);
        QVERIFY(heartbeats >= 5);
        CliConsole::canceled.store(false);
    }
    void smbConnectValidatesBeforeAuthentication()
    {
        auto server = serverConfig();
        for (const auto& url : {"https://localhost/share", "smb://localhost", "smb://localhost/share/../other"}) {
            server.baseUrl = url;
            const auto result = SmbClient::connect(server, "unused-test-password");
            QVERIFY(!result);
#ifdef Q_OS_WIN
            QVERIFY(result.error().contains("Invalid") || result.error().contains("share is required"));
#endif
        }
    }
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
#else
        QVERIFY(SessionTokenCipher::isCredentialReference(query.value(0).toString()));
#endif
        QVERIFY(!readFile(path).contains("legacy-new-token"));
        QVERIFY(!readFile(path).contains("legacy-old-token"));
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
    void playlistRejectsProseAndReadsAttributeStyles()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("styles.m3u");
        QVERIFY(writeFile(path,
            "#EXTM3U\n#EXTINF:-1 group-title='News, live' tvg-logo=https://example.com/logo.png,News\n"
            "random prose\n123\nhttps://example.com/live\n"));
        const auto channels = IptvParser::parseFile(path);
        QVERIFY(channels && channels->size() == 1);
        QCOMPARE(channels->front().groupName, QString("News, live"));
        QCOMPARE(channels->front().logoUrl, QString("https://example.com/logo.png"));
        QCOMPARE(channels->front().streamUrl, QString("https://example.com/live"));
    }
    void jsonNumbersPreserveIntegersAndRejectOverflow()
    {
        const auto object = QJsonDocument::fromJson(
            "{\"id\":1234567890,\"ticks\":100000000000000001,\"large\":2147483648,\"fraction\":1.5,\"overflow\":1e30}").object();
        QCOMPARE(jsonString(object, "id"), QString("1234567890"));
        QCOMPARE(jsonInt64Any(object, {"ticks"}), qint64(100000000000000001LL));
        QCOMPARE(jsonString(object, "ticks"), QString("100000000000000001"));
        QCOMPARE(jsonIntAny(object, {"large"}, -9), -9);
        QCOMPARE(jsonInt64Any(object, {"overflow"}, -9), qint64(-9));
        QCOMPARE(jsonInt64Any(object, {"fraction"}, -9), qint64(-9));
    }
    void recommendationCachePreservesNavigationAndResume()
    {
        MediaItem item {.id = "show", .itemType = "Series"};
        item.folder = true; item.played = true; item.playbackPositionTicks = 1234567890123;
        item.seasonName = "Season One"; item.indexNumber = "2"; item.people = "Person";
        const std::vector<MediaItem> items {item};
        const auto cached = EmbyRecommendationCache::deserialize(EmbyRecommendationCache::serialize(items), "fake");
        QVERIFY(cached && cached->size() == 1);
        QVERIFY(cached->front().folder); QVERIFY(cached->front().played);
        QCOMPARE(cached->front().playbackPositionTicks, item.playbackPositionTicks);
        QCOMPARE(cached->front().seasonName, item.seasonName);
        QCOMPARE(cached->front().people, item.people);
        const auto legacy = EmbyRecommendationCache::deserialize("[{\"id\":\"old\",\"itemType\":\"Series\"}]", "fake");
        QVERIFY(legacy && legacy->front().folder);
    }
    void hlsTagsFollowTheActualHeader()
    {
        const QByteArray manifest = "\n \r\n#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nsegment.ts\n#EXT-X-ENDLIST\n";
        const auto withIdentifier = HlsManifestValidator::insertM3u8sIdentifier(manifest, QByteArray(4096, 'A'));
        QVERIFY(withIdentifier);
        QVERIFY(HlsManifestValidator::validate(*withIdentifier));
        const auto withSource = HlsManifestValidator::insertEncryptedSourceFileName(*withIdentifier, QByteArray(48, 'x'));
        QVERIFY(withSource);
        QVERIFY(HlsManifestValidator::validate(*withSource));
        QVERIFY(withSource->indexOf("#EXTM3U") < withSource->indexOf("#M3U8S-SOURCE-NAME:"));
    }
    void webdavInvalidXmlIsNotAnEmptyDirectory()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray&) {
            Endpoint::respond(socket, "<broken>", "207 Multi-Status");
        };
        WebDavClient client;
        std::optional<WebDavListResult> result;
        client.listDirectory(serverConfig(), {}, endpoint.url(), [&](auto value) { result = std::move(value); });
        QTRY_VERIFY(result.has_value()); QVERIFY(!*result);
        QCOMPARE(result->error().kind, NetworkErrorKind::Parse);
        std::optional<WebDavItemResult> stat;
        client.statItem(serverConfig(), {}, endpoint.url(), [&](auto value) { stat = std::move(value); });
        QTRY_VERIFY(stat.has_value()); QVERIFY(!*stat);
        QCOMPARE(stat->error().kind, NetworkErrorKind::Parse);
    }
    void emptyScheduledCandidatesStopRequests()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray&) { Endpoint::respond(socket, "{\"Items\":[]}"); };
        QTemporaryDir directory;
        SessionRepository repository(uniqueId(), directory.filePath("db.sqlite")); QVERIFY(repository.initialize());
        auto server = serverConfig(); server.baseUrl = endpoint.url().toString();
        QVERIFY(repository.saveSession(UserSession {.server = server, .userId = "user", .username = "test", .accessToken = "fake", .createdAt = QDateTime::currentDateTimeUtc()}));
        NetworkClient network; EmbyClient client(network);
        ScheduledPlaybackManager manager(client, repository);
        manager.runNow(ScheduledPlaybackTask {.id = "task", .serverId = server.id});
        QTRY_VERIFY(!manager.errorMessage().isEmpty());
        QVERIFY(!manager.active());
        QTest::qWait(250);
        QCOMPARE(endpoint.requests.size(), 1);
    }
    void destroyedSchedulerIgnoresPendingReply()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.handler = [](QTcpSocket* socket, const QByteArray&) {
            QTimer::singleShot(100, socket, [socket] { Endpoint::respond(socket, "{\"Items\":[]}"); });
        };
        QTemporaryDir directory;
        SessionRepository repository(uniqueId(), directory.filePath("db.sqlite")); QVERIFY(repository.initialize());
        auto server = serverConfig(); server.baseUrl = endpoint.url().toString();
        QVERIFY(repository.saveSession(UserSession {.server = server, .userId = "user", .username = "test", .accessToken = "fake", .createdAt = QDateTime::currentDateTimeUtc()}));
        NetworkClient network; EmbyClient client(network);
        auto manager = std::make_unique<ScheduledPlaybackManager>(client, repository);
        manager->runNow(ScheduledPlaybackTask {.id = "task", .serverId = server.id});
        QTRY_COMPARE(endpoint.requests.size(), 1);
        manager.reset();
        QTest::qWait(250);
        QCOMPARE(endpoint.requests.size(), 1);
    }
    void replyWriterBoundsSlowConsumerAndDrainsTail()
    {
        Endpoint endpoint; QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        const QByteArray body(8 * 1024 * 1024, 'x');
        endpoint.handler = [body](QTcpSocket* socket, const QByteArray&) { Endpoint::respond(socket, body); };
        QTcpServer downstream; QVERIFY(downstream.listen(QHostAddress::LocalHost));
        QTcpSocket consumer; consumer.setReadBufferSize(1024);
        consumer.connectToHost(QHostAddress::LocalHost, downstream.serverPort());
        QVERIFY(consumer.waitForConnected(1000));
        QTRY_VERIFY(downstream.hasPendingConnections());
        auto* producer = downstream.nextPendingConnection();
        QNetworkAccessManager manager; auto* reply = manager.get(QNetworkRequest(endpoint.url()));
        QPointer<QNetworkReply> guard(reply);
        qint64 received = 0, peak = 0;
        new BoundedReplyWriter(reply, producer, [] {}, [&](qint64 bytes) {
            received += bytes; peak = std::max(peak, producer->bytesToWrite() + bytes);
        });
        QTest::qWait(200);
        QVERIFY(received < body.size());
        QVERIFY(peak <= BoundedReplyWriter::maximumBufferedBytes);
        QCOMPARE(reply->readBufferSize(), BoundedReplyWriter::maximumBufferedBytes);
        QByteArray result;
        connect(&consumer, &QTcpSocket::readyRead, &consumer, [&] { result += consumer.readAll(); });
        consumer.setReadBufferSize(0); result += consumer.readAll();
        QTRY_COMPARE_WITH_TIMEOUT(result.size(), body.size(), 8000);
        QCOMPARE(result, body);
        QTRY_VERIFY(guard.isNull());
        producer->deleteLater();
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
    void sensitiveLogsRedactEveryCookieAndTokenBeforeWriting()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("redacted.log");
        const auto previous = qgetenv("VIBEPLAYER_LOG_FILE");
        qputenv("VIBEPLAYER_LOG_FILE", path.toUtf8());
        capturedLogMessages.clear();
        const auto previousHandler = qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
            capturedLogMessages.append(message);
        });
        AppLogger::setSanitizer([](const QString& message) { return ErrorPresentation::sanitizeDetails(message); });
        const auto restore = qScopeGuard([&] {
            AppLogger::setSanitizer({});
            qInstallMessageHandler(previousHandler);
            if (previous.isNull()) qunsetenv("VIBEPLAYER_LOG_FILE"); else qputenv("VIBEPLAYER_LOG_FILE", previous);
        });
        AppLogger::warning("security-test", "Authorization: Bearer fake-bearer-secret\n"
            "Cookie: session=fake-cookie-first; refresh=fake-cookie-second\n"
            "https://user:fake-url-password@example.test/item?api_key=fake-api-key&password=fake-query-password\n"
            "Token: fake-token-secret");
        const auto written = readFile(path);
        const auto console = capturedLogMessages.join('\n').toUtf8();
        QVERIFY(!written.isEmpty()); QVERIFY(!console.isEmpty());
        QVERIFY(written.contains("[REDACTED]")); QVERIFY(console.contains("[REDACTED]"));
        for (const auto* secret : {"fake-bearer-secret", "fake-cookie-first", "fake-cookie-second",
                                   "fake-url-password", "fake-api-key", "fake-query-password", "fake-token-secret"}) {
            QVERIFY(!written.contains(secret));
            QVERIFY(!console.contains(secret));
        }
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
