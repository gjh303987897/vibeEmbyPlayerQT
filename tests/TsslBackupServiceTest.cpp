#include "services/backup/TsslBackupService.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>

#include <memory>
#include <optional>

namespace {
bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

// Hold the sole worker so tests can detect synchronous file work without
// relying on disk speed, a large fixture, or timing thresholds.
class WorkerGate final {
public:
    WorkerGate() : m_oldMaximum(QThreadPool::globalInstance()->maxThreadCount())
    {
        QThreadPool::globalInstance()->setMaxThreadCount(1);
        m_worker = QtConcurrent::run([this]() {
            m_entered.release();
            m_release.acquire();
        });
        m_entered.acquire();
    }

    ~WorkerGate()
    {
        release();
        m_worker.waitForFinished();
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(m_oldMaximum);
    }

    void release()
    {
        if (!m_released) {
            m_released = true;
            m_release.release();
        }
    }

private:
    int m_oldMaximum;
    QSemaphore m_entered;
    QSemaphore m_release;
    QFuture<void> m_worker;
    bool m_released { false };
};

struct PutRequest final {
    QByteArray path;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
};

class BackupEndpoint final : public QTcpServer {
public:
    BackupEndpoint()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                    buffer->append(socket->readAll());
                    const auto headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const auto lines = buffer->left(headerEnd).split('\n');
                    PutRequest request;
                    request.path = lines.first().split(' ').value(1);
                    for (const auto& line : lines.sliced(1)) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0) {
                            request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
                        }
                    }
                    const auto length = request.headers.value("content-length").toLongLong();
                    if (buffer->size() - headerEnd - 4 < length) return;
                    request.body = buffer->mid(headerEnd + 4, length);
                    requests.append(std::move(request));
                    buffer->clear();
                    socket->write("HTTP/1.1 201 Created\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                });
            }
        });
    }

    TsslBackupTarget webDavTarget() const
    {
        TsslBackupTarget target;
        target.webDavServer.baseUrl = QStringLiteral("http://localhost:%1/dav").arg(serverPort());
        target.webDavPath = QStringLiteral("backups");
        return target;
    }

    QList<PutRequest> requests;
};
}

class TsslBackupServiceTest final : public QObject {
    Q_OBJECT

private slots:
    void preparesOnlyValidPackagesWithoutBlocking();
    void preparesAnEmptyStore();
    void uploadsFilesSequentiallyWithoutBlocking();
    void signsS3PayloadPreparedInBackground();
    void cancelsDuringFilePreparationAndCanRestart();
    void destructionDuringFilePreparationIsSafe();
    void rejectsUnreadableOrInvalidFiles_data();
    void rejectsUnreadableOrInvalidFiles();
};

void TsslBackupServiceTest::preparesOnlyValidPackagesWithoutBlocking()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TsslStore store(directory.filePath(QStringLiteral("store")));
    TsslPackage package {
        .identifier = QByteArray(TsslPackage::identifierLength, 'A'),
        .rootManifestDigest = QByteArray(32, 'B'),
        .segmentKeys = { { QStringLiteral("segments/0001.ts"), QByteArray(32, 'C') } },
    };
    QVERIFY(store.savePackage(package).has_value());
    const auto validPath = QDir(store.storageDirectory()).filePath(QString::fromLatin1(package.rootManifestDigest.toHex()) + ".tssl");
    const auto invalidPath = QDir(store.storageDirectory()).filePath(QString(64, 'a') + ".tssl");
    QVERIFY(writeBytes(invalidPath, QByteArrayLiteral("invalid JSON")));

    WorkerGate gate;
    auto future = TsslBackupService::preparePackages(store);
    QVERIFY(!future.isFinished());
    bool eventDelivered = false;
    QTimer::singleShot(0, this, [&eventDelivered]() { eventDelivered = true; });
    QTRY_VERIFY(eventDelivered);
    QVERIFY(!future.isFinished());
    gate.release();
    QTRY_VERIFY(future.isFinished());
    const auto prepared = future.takeResult();
    if (!prepared) QFAIL(qPrintable(prepared.error()));
    QCOMPARE(prepared->files, QStringList { validPath });
    QCOMPARE(prepared->digests, std::vector<QByteArray> { package.rootManifestDigest });
}

void TsslBackupServiceTest::preparesAnEmptyStore()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto future = TsslBackupService::preparePackages(TsslStore(directory.filePath(QStringLiteral("missing"))));
    QTRY_VERIFY(future.isFinished());
    const auto prepared = future.takeResult();
    QVERIFY(prepared.has_value());
    QVERIFY(prepared->files.isEmpty());
    QVERIFY(prepared->digests.empty());
}

void TsslBackupServiceTest::uploadsFilesSequentiallyWithoutBlocking()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto first = directory.filePath(QStringLiteral("first.tssl"));
    const auto second = directory.filePath(QStringLiteral("second.tssl"));
    QVERIFY(writeBytes(first, QByteArrayLiteral("before worker")));
    QVERIFY(writeBytes(second, QByteArrayLiteral("second package")));
    BackupEndpoint endpoint;
    QVERIFY(endpoint.listen(QHostAddress::LocalHost));
    TsslBackupService service;
    QSignalSpy progress(&service, &TsslBackupService::progressChanged);
    std::optional<TsslBackupResult> result;
    WorkerGate gate;
    service.backup(endpoint.webDavTarget(), { first, second }, [&result](TsslBackupResult value) { result = std::move(value); });
    QVERIFY(service.isRunning());
    QVERIFY(!result.has_value());
    // The first read must happen after the queued worker starts.
    QVERIFY(writeBytes(first, QByteArrayLiteral("after worker")));
    bool eventDelivered = false;
    QTimer::singleShot(0, this, [&eventDelivered]() { eventDelivered = true; });
    QTRY_VERIFY(eventDelivered);
    QVERIFY(endpoint.requests.isEmpty());
    std::optional<TsslBackupResult> duplicate;
    service.backup(endpoint.webDavTarget(), { second }, [&duplicate](TsslBackupResult value) { duplicate = std::move(value); });
    QVERIFY(duplicate.has_value() && !duplicate->has_value());

    gate.release();
    QTRY_VERIFY(result.has_value());
    QVERIFY(result->has_value());
    QCOMPARE(**result, 2);
    QVERIFY(!service.isRunning());
    QCOMPARE(endpoint.requests.size(), 2);
    QCOMPARE(endpoint.requests.at(0).path, QByteArrayLiteral("/dav/backups/first.tssl"));
    QCOMPARE(endpoint.requests.at(0).body, QByteArrayLiteral("after worker"));
    QCOMPARE(endpoint.requests.at(1).body, QByteArrayLiteral("second package"));
    QCOMPARE(progress.size(), 3);
    QCOMPARE(progress.last().at(0).toInt(), 2);
    QCOMPARE(progress.last().at(1).toInt(), 2);
}

void TsslBackupServiceTest::signsS3PayloadPreparedInBackground()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("package.tssl"));
    QVERIFY(writeBytes(path, QByteArrayLiteral("old contents")));
    BackupEndpoint endpoint;
    QVERIFY(endpoint.listen(QHostAddress::LocalHost));
    TsslBackupTarget target;
    target.type = TsslBackupTarget::Type::S3;
    target.s3Endpoint = QUrl(QStringLiteral("http://localhost:%1").arg(endpoint.serverPort()));
    target.s3Bucket = QStringLiteral("test-bucket");
    target.s3Region = QStringLiteral("us-east-1");
    target.s3Prefix = QStringLiteral("backup/tssl");
    target.s3AccessKey = QStringLiteral("test-access-key");
    target.s3SecretKey = QStringLiteral("test-secret-key");
    TsslBackupService service;
    std::optional<TsslBackupResult> result;
    WorkerGate gate;
    service.backup(target, { path }, [&result](TsslBackupResult value) { result = std::move(value); });
    const QByteArray payload(8 * 1024 * 1024, 'P');
    QVERIFY(writeBytes(path, payload));
    gate.release();
    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 10'000);
    QVERIFY(result->has_value());
    QCOMPARE(endpoint.requests.size(), 1);
    const auto& request = endpoint.requests.first();
    QCOMPARE(request.path, QByteArrayLiteral("/test-bucket/backup/tssl/package.tssl"));
    QCOMPARE(request.body, payload);
    QCOMPARE(request.headers.value("x-amz-content-sha256"), QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    QVERIFY(request.headers.value("authorization").startsWith("AWS4-HMAC-SHA256 Credential=test-access-key/"));
    QVERIFY(request.headers.value("authorization").contains("/us-east-1/s3/aws4_request"));
}

void TsslBackupServiceTest::cancelsDuringFilePreparationAndCanRestart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("package.tssl"));
    QVERIFY(writeBytes(path, QByteArrayLiteral("package")));
    BackupEndpoint endpoint;
    QVERIFY(endpoint.listen(QHostAddress::LocalHost));
    TsslBackupService service;
    std::optional<TsslBackupResult> canceled;
    std::optional<TsslBackupResult> restarted;
    WorkerGate gate;
    service.backup(endpoint.webDavTarget(), { path }, [&](TsslBackupResult value) {
        canceled = std::move(value);
        service.backup(endpoint.webDavTarget(), { path }, [&](TsslBackupResult next) { restarted = std::move(next); });
    });
    service.cancel();
    gate.release();
    QTRY_VERIFY(restarted.has_value());
    QVERIFY(canceled.has_value() && !canceled->has_value());
    QCOMPARE(canceled->error(), QStringLiteral("TSSL backup canceled"));
    QVERIFY(restarted->has_value());
    QCOMPARE(**restarted, 1);
    QCOMPARE(endpoint.requests.size(), 1);
    QVERIFY(!service.isRunning());
}

void TsslBackupServiceTest::destructionDuringFilePreparationIsSafe()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("package.tssl"));
    QVERIFY(writeBytes(path, QByteArrayLiteral("package")));
    BackupEndpoint endpoint;
    QVERIFY(endpoint.listen(QHostAddress::LocalHost));
    bool callbackInvoked = false;
    {
        WorkerGate gate;
        auto service = std::make_unique<TsslBackupService>();
        service->backup(endpoint.webDavTarget(), { path }, [&](TsslBackupResult) { callbackInvoked = true; });
        service.reset();
        gate.release();
    }
    QCoreApplication::processEvents();
    QVERIFY(!callbackInvoked);
    QVERIFY(endpoint.requests.isEmpty());
}

void TsslBackupServiceTest::rejectsUnreadableOrInvalidFiles_data()
{
    QTest::addColumn<qint64>("size");
    QTest::addColumn<QString>("errorText");
    QTest::newRow("missing") << qint64(-1) << QStringLiteral("Unable to read TSSL package");
    QTest::newRow("empty") << qint64(0) << QStringLiteral("size is invalid");
    QTest::newRow("too large") << qint64(256LL * 1024 * 1024 + 1) << QStringLiteral("exceeds 256 MiB");
}

void TsslBackupServiceTest::rejectsUnreadableOrInvalidFiles()
{
    QFETCH(qint64, size);
    QFETCH(QString, errorText);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("package.tssl"));
    if (size >= 0) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(size));
    }
    BackupEndpoint endpoint;
    QVERIFY(endpoint.listen(QHostAddress::LocalHost));
    TsslBackupService service;
    std::optional<TsslBackupResult> result;
    service.backup(endpoint.webDavTarget(), { path }, [&](TsslBackupResult value) { result = std::move(value); });
    QTRY_VERIFY(result.has_value());
    QVERIFY(!result->has_value());
    QVERIFY(result->error().contains(errorText));
    QVERIFY(!service.isRunning());
    QVERIFY(endpoint.requests.isEmpty());
}

QTEST_GUILESS_MAIN(TsslBackupServiceTest)

#include "TsslBackupServiceTest.moc"
