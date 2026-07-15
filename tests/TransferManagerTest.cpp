#include "services/webdav/TransferManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

namespace {
class DownloadServer final : public QObject {
public:
    explicit DownloadServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    auto request = socket->property("requestBuffer").toByteArray();
                    request += socket->readAll();
                    socket->setProperty("requestBuffer", request);
                    if (!request.contains("\r\n\r\n") || socket->property("responseStarted").toBool()) {
                        return;
                    }

                    socket->setProperty("responseStarted", true);
                    const auto requestLine = request.left(request.indexOf("\r\n"));
                    const auto path = requestLine.split(' ').value(1);
                    ++m_requestCounts[path];
                    auto& failuresRemaining = m_failuresRemaining[path];
                    if (failuresRemaining > 0) {
                        --failuresRemaining;
                        socket->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    const auto body = std::make_shared<QByteArray>(m_payloads.value(path));
                    const auto offset = std::make_shared<qsizetype>(0);

                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: ");
                    socket->write(QByteArray::number(body->size()));
                    socket->write("\r\nConnection: close\r\n\r\n");

                    auto* timer = new QTimer(socket);
                    timer->setInterval(15);
                    connect(timer, &QTimer::timeout, socket, [socket, timer, body, offset]() {
                        constexpr qsizetype chunkSize = 2048;
                        const auto remaining = body->size() - *offset;
                        const auto size = std::min(chunkSize, remaining);
                        if (size > 0) {
                            socket->write(body->constData() + *offset, size);
                            *offset += size;
                        }
                        if (*offset >= body->size()) {
                            timer->stop();
                            socket->disconnectFromHost();
                        }
                    });
                    timer->start();
                });
            }
        });
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    void addPayload(const QByteArray& path, QByteArray payload)
    {
        m_payloads.insert(path, std::move(payload));
    }

    void failNextRequests(const QByteArray& path, int count)
    {
        m_failuresRemaining.insert(path, count);
    }

    int requestCount(const QByteArray& path) const
    {
        return m_requestCounts.value(path);
    }

    QUrl url(const QString& path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path));
    }

private:
    QTcpServer m_server;
    QHash<QByteArray, QByteArray> m_payloads;
    QHash<QByteArray, int> m_failuresRemaining;
    QHash<QByteArray, int> m_requestCounts;
};

class UploadServer final : public QObject {
public:
    explicit UploadServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = m_server.nextPendingConnection()) {
                socket->setReadBufferSize(8 * 1024);
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    if (socket->property("uploadStarted").toBool()) {
                        return;
                    }

                    auto request = socket->property("requestBuffer").toByteArray();
                    request += socket->readAll();
                    socket->setProperty("requestBuffer", request);
                    const auto headerEnd = request.indexOf("\r\n\r\n");
                    if (headerEnd < 0) {
                        return;
                    }

                    const auto header = request.left(headerEnd);
                    const auto requestLine = header.left(header.indexOf("\r\n"));
                    const auto path = requestLine.split(' ').value(1);
                    qint64 contentLength = -1;
                    for (const auto& line : header.split('\n')) {
                        const auto normalized = line.trimmed();
                        if (normalized.toLower().startsWith("content-length:")) {
                            contentLength = normalized.mid(sizeof("content-length:") - 1).trimmed().toLongLong();
                            break;
                        }
                    }

                    ++m_requestCounts[path];
                    auto& failuresRemaining = m_failuresRemaining[path];
                    auto shouldFail = false;
                    if (failuresRemaining > 0) {
                        --failuresRemaining;
                        shouldFail = true;
                    }
                    if (contentLength < 0) {
                        socket->setProperty("uploadStarted", true);
                        socket->write("HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }

                    socket->setProperty("uploadStarted", true);
                    socket->setProperty("uploadPath", path);
                    socket->setProperty("uploadLength", contentLength);
                    socket->setProperty("uploadBody", request.mid(headerEnd + 4));
                    socket->setProperty("uploadShouldFail", shouldFail);

                    auto* timer = new QTimer(socket);
                    timer->setInterval(5);
                    connect(timer, &QTimer::timeout, socket, [this, socket, timer]() {
                        auto body = socket->property("uploadBody").toByteArray();
                        body += socket->read(16 * 1024);
                        const auto contentLength = socket->property("uploadLength").toLongLong();
                        if (body.size() < contentLength) {
                            socket->setProperty("uploadBody", body);
                            return;
                        }

                        const auto path = socket->property("uploadPath").toByteArray();
                        timer->stop();
                        if (socket->property("uploadShouldFail").toBool()) {
                            socket->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        } else {
                            m_uploadedPayloads.insert(path, body.left(contentLength));
                            socket->write("HTTP/1.1 201 Created\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        }
                        socket->disconnectFromHost();
                    });
                    timer->start();
                });
            }
        });
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    void failNextRequests(const QByteArray& path, int count)
    {
        m_failuresRemaining.insert(path, count);
    }

    int requestCount(const QByteArray& path) const
    {
        return m_requestCounts.value(path);
    }

    QByteArray uploadedPayload(const QByteArray& path) const
    {
        return m_uploadedPayloads.value(path);
    }

    QUrl url(const QString& path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path));
    }

private:
    QTcpServer m_server;
    QHash<QByteArray, int> m_failuresRemaining;
    QHash<QByteArray, int> m_requestCounts;
    QHash<QByteArray, QByteArray> m_uploadedPayloads;
};

QVariant taskData(TransferTaskListModel* model, int row, TransferTaskListModel::Role role)
{
    return model->data(model->index(row, 0), role);
}

int taskRowForTarget(TransferTaskListModel* model, const QString& target)
{
    for (auto row = 0; row < model->rowCount(); ++row) {
        if (taskData(model, row, TransferTaskListModel::TargetRole).toString() == target) {
            return row;
        }
    }
    return -1;
}
}

class TransferManagerTest final : public QObject {
    Q_OBJECT

private slots:
    void filtersTransferTasksByStatus()
    {
        TransferTaskListModel model;
        model.setTasks({
            TransferTask { .id = QStringLiteral("done"), .status = QStringLiteral("done") },
            TransferTask { .id = QStringLiteral("queued"), .status = QStringLiteral("queued") },
            TransferTask { .id = QStringLiteral("running"), .status = QStringLiteral("running") },
            TransferTask { .id = QStringLiteral("paused"), .status = QStringLiteral("paused") },
            TransferTask { .id = QStringLiteral("failed"), .status = QStringLiteral("failed") },
            TransferTask { .id = QStringLiteral("canceled"), .status = QStringLiteral("canceled") },
        });

        QCOMPARE(model.rowCount(), 6);

        model.setStatusFilter(QStringLiteral("completed"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(taskData(&model, 0, TransferTaskListModel::IdRole).toString(), QStringLiteral("done"));

        model.setStatusFilter(QStringLiteral("incomplete"));
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(taskData(&model, 0, TransferTaskListModel::IdRole).toString(), QStringLiteral("queued"));
        QCOMPARE(taskData(&model, 1, TransferTaskListModel::IdRole).toString(), QStringLiteral("running"));
        QCOMPARE(taskData(&model, 2, TransferTaskListModel::IdRole).toString(), QStringLiteral("paused"));

        model.setStatusFilter(QStringLiteral("failed"));
        QCOMPARE(model.rowCount(), 1);
        model.updateTask(TransferTask { .id = QStringLiteral("failed"), .status = QStringLiteral("queued") });
        QCOMPARE(model.rowCount(), 0);

        model.setStatusFilter(QStringLiteral("canceled"));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(taskData(&model, 0, TransferTaskListModel::IdRole).toString(), QStringLiteral("canceled"));

        model.setStatusFilter(QStringLiteral("invalid"));
        QCOMPARE(model.statusFilter(), QStringLiteral("all"));
        QCOMPARE(model.rowCount(), 6);
    }

    void pausesAndResumesUpload()
    {
        UploadServer server;
        QVERIFY(server.listen());

        const QByteArray payload(2 * 1024 * 1024, 'u');
        QTemporaryDir sourceDirectory;
        QVERIFY(sourceDirectory.isValid());
        const auto sourcePath = sourceDirectory.filePath(QStringLiteral("paused-upload.bin"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        QCOMPARE(sourceFile.write(payload), qint64 { payload.size() });
        sourceFile.close();

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-upload-pause"),
            .name = QStringLiteral("WebDAV upload pause"),
            .serviceType = ServiceType::WebDAV,
        };
        const auto taskId = manager.enqueueUpload(serverConfig,
                                                  {},
                                                  sourcePath,
                                                  server.url(QStringLiteral("/paused-upload.bin")),
                                                  payload.size());
        auto* tasks = manager.tasks();
        QCOMPARE(tasks->rowCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount("/paused-upload.bin"), 1, 3000);
        QCOMPARE(taskData(tasks, 0, TransferTaskListModel::StatusRole).toString(), QStringLiteral("running"));
        QVERIFY(taskData(tasks, 0, TransferTaskListModel::CanPauseRole).toBool());

        manager.pauseTask(taskId);
        QTRY_COMPARE_WITH_TIMEOUT(taskData(tasks, 0, TransferTaskListModel::StatusRole).toString(),
                                  QStringLiteral("paused"),
                                  3000);
        QVERIFY(taskData(tasks, 0, TransferTaskListModel::CanResumeRole).toBool());
        QCOMPARE(QFileInfo(sourcePath).size(), qint64 { payload.size() });

        manager.resumeTask(taskId);
        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 10000);
        QCOMPARE(server.requestCount("/paused-upload.bin"), 2);
        QCOMPARE(server.uploadedPayload("/paused-upload.bin"), payload);
        QCOMPARE(QFileInfo(sourcePath).size(), qint64 { payload.size() });
    }

    void cancelingUploadKeepsLocalFileAndAllowsRetry()
    {
        UploadServer server;
        QVERIFY(server.listen());

        const QByteArray payload(2 * 1024 * 1024, 'c');
        QTemporaryDir sourceDirectory;
        QVERIFY(sourceDirectory.isValid());
        const auto sourcePath = sourceDirectory.filePath(QStringLiteral("canceled-upload.bin"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        QCOMPARE(sourceFile.write(payload), qint64 { payload.size() });
        sourceFile.close();

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-upload-cancel"),
            .name = QStringLiteral("WebDAV upload cancel"),
            .serviceType = ServiceType::WebDAV,
        };
        const auto taskId = manager.enqueueUpload(serverConfig,
                                                  {},
                                                  sourcePath,
                                                  server.url(QStringLiteral("/canceled-upload.bin")),
                                                  payload.size());
        auto* tasks = manager.tasks();
        QTRY_COMPARE_WITH_TIMEOUT(server.requestCount("/canceled-upload.bin"), 1, 3000);

        manager.cancelTask(taskId);
        QTRY_COMPARE_WITH_TIMEOUT(taskData(tasks, 0, TransferTaskListModel::StatusRole).toString(),
                                  QStringLiteral("canceled"),
                                  3000);
        QVERIFY(taskData(tasks, 0, TransferTaskListModel::RetryableRole).toBool());
        QCOMPARE(taskData(tasks, 0, TransferTaskListModel::BytesDoneRole).toLongLong(), qint64 { 0 });
        QCOMPARE(QFileInfo(sourcePath).size(), qint64 { payload.size() });

        manager.retryTask(taskId);
        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 10000);
        QCOMPARE(server.requestCount("/canceled-upload.bin"), 2);
        QCOMPARE(server.uploadedPayload("/canceled-upload.bin"), payload);
        QCOMPARE(QFileInfo(sourcePath).size(), qint64 { payload.size() });
    }

    void retriesFailedUpload()
    {
        UploadServer server;
        QVERIFY(server.listen());
        server.failNextRequests("/failed-upload.bin", 1);

        const QByteArray payload(512 * 1024, 'f');
        QTemporaryDir sourceDirectory;
        QVERIFY(sourceDirectory.isValid());
        const auto sourcePath = sourceDirectory.filePath(QStringLiteral("failed-upload.bin"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        QCOMPARE(sourceFile.write(payload), qint64 { payload.size() });
        sourceFile.close();

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-upload-retry"),
            .name = QStringLiteral("WebDAV upload retry"),
            .serviceType = ServiceType::WebDAV,
        };
        const auto taskId = manager.enqueueUpload(serverConfig,
                                                  {},
                                                  sourcePath,
                                                  server.url(QStringLiteral("/failed-upload.bin")),
                                                  payload.size());
        auto* tasks = manager.tasks();
        QTRY_COMPARE_WITH_TIMEOUT(manager.failedCount(), 1, 3000);
        QVERIFY(taskData(tasks, 0, TransferTaskListModel::RetryableRole).toBool());
        QCOMPARE(QFileInfo(sourcePath).size(), qint64 { payload.size() });

        manager.retryTask(taskId);
        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 10000);
        QCOMPARE(server.requestCount("/failed-upload.bin"), 2);
        QCOMPARE(server.uploadedPayload("/failed-upload.bin"), payload);
    }

    void groupsDownloadFilesAndPublishesAggregateProgress()
    {
        DownloadServer server;
        QVERIFY(server.listen());

        const QByteArray firstPayload(96 * 1024, 'a');
        const QByteArray secondPayload(60 * 1024, 'b');
        server.addPayload("/first.bin", firstPayload);
        server.addPayload("/second.bin", secondPayload);

        QTemporaryDir targetDirectory;
        QVERIFY(targetDirectory.isValid());
        const auto firstPath = targetDirectory.filePath(QStringLiteral("first.bin"));
        const auto secondPath = targetDirectory.filePath(QStringLiteral("second.bin"));

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-test"),
            .name = QStringLiteral("WebDAV test"),
            .serviceType = ServiceType::WebDAV,
        };
        std::vector<TransferManager::DownloadRequest> requests {
            {
                .remoteUrl = server.url(QStringLiteral("/first.bin")),
                .localPath = firstPath,
                .totalBytes = firstPayload.size(),
            },
            {
                .remoteUrl = server.url(QStringLiteral("/second.bin")),
                .localPath = secondPath,
                .totalBytes = -1,
            },
        };

        const auto groupId = manager.enqueueDownloads(serverConfig,
                                                      {},
                                                      QStringLiteral("Season download"),
                                                      targetDirectory.path(),
                                                      std::move(requests));

        auto* summaries = manager.tasks();
        QCOMPARE(summaries->rowCount(), 1);
        QCOMPARE(taskData(summaries, 0, TransferTaskListModel::IdRole).toString(), groupId);
        QVERIFY(taskData(summaries, 0, TransferTaskListModel::IsGroupRole).toBool());
        QCOMPARE(taskData(summaries, 0, TransferTaskListModel::FileCountRole).toInt(), 2);
        QCOMPARE(taskData(summaries, 0, TransferTaskListModel::BytesTotalRole).toLongLong(), qint64 { -1 });

        QVERIFY(manager.selectGroup(groupId));
        auto* details = manager.detailTasks();
        QCOMPARE(details->rowCount(), 2);

        QTRY_VERIFY_WITH_TIMEOUT(manager.downloadBytesPerSecond() > 0, 3000);
        QVERIFY(manager.averageDownloadBytesPerSecond() > 0);
        QCOMPARE(manager.uploadBytesPerSecond(), qint64 { 0 });
        QCOMPARE(manager.averageUploadBytesPerSecond(), qint64 { 0 });

        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 5000);

        const auto expectedBytes = qint64 { firstPayload.size() + secondPayload.size() };
        QCOMPARE(summaries->rowCount(), 1);
        QCOMPARE(taskData(summaries, 0, TransferTaskListModel::StatusRole).toString(), QStringLiteral("done"));
        QCOMPARE(taskData(summaries, 0, TransferTaskListModel::CompletedFileCountRole).toInt(), 2);
        QCOMPARE(taskData(summaries, 0, TransferTaskListModel::BytesDoneRole).toLongLong(), expectedBytes);
        QCOMPARE(taskData(summaries, 0, TransferTaskListModel::BytesTotalRole).toLongLong(), expectedBytes);
        QCOMPARE(taskData(summaries, 0, TransferTaskListModel::BytesRemainingRole).toLongLong(), qint64 { 0 });
        QCOMPARE(taskData(summaries, 0, TransferTaskListModel::ProgressRole).toDouble(), 1.0);
        QVERIFY(taskData(summaries, 0, TransferTaskListModel::AverageBytesPerSecondRole).toLongLong() > 0);

        for (auto row = 0; row < details->rowCount(); ++row) {
            QCOMPARE(taskData(details, row, TransferTaskListModel::StatusRole).toString(), QStringLiteral("done"));
            QCOMPARE(taskData(details, row, TransferTaskListModel::BytesRemainingRole).toLongLong(), qint64 { 0 });
        }
        QCOMPARE(QFileInfo(firstPath).size(), qint64 { firstPayload.size() });
        QCOMPARE(QFileInfo(secondPath).size(), qint64 { secondPayload.size() });
    }

    void pausesAndResumesIndividualDownload()
    {
        DownloadServer server;
        QVERIFY(server.listen());

        const QByteArray payload(384 * 1024, 'p');
        server.addPayload("/paused-item.bin", payload);

        QTemporaryDir targetDirectory;
        QVERIFY(targetDirectory.isValid());
        const auto targetPath = targetDirectory.filePath(QStringLiteral("paused-item.bin"));

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-pause-item"),
            .name = QStringLiteral("WebDAV pause item"),
            .serviceType = ServiceType::WebDAV,
        };
        const auto groupId = manager.enqueueDownload(serverConfig,
                                                     {},
                                                     server.url(QStringLiteral("/paused-item.bin")),
                                                     targetPath,
                                                     payload.size());
        QVERIFY(manager.selectGroup(groupId));
        auto* details = manager.detailTasks();
        QCOMPARE(details->rowCount(), 1);
        const auto childId = taskData(details, 0, TransferTaskListModel::IdRole).toString();

        QTRY_VERIFY_WITH_TIMEOUT(taskData(details, 0, TransferTaskListModel::BytesDoneRole).toLongLong() > 0, 3000);
        manager.pauseTask(childId);
        QTRY_COMPARE_WITH_TIMEOUT(taskData(details, 0, TransferTaskListModel::StatusRole).toString(),
                                  QStringLiteral("paused"),
                                  3000);
        QVERIFY(taskData(details, 0, TransferTaskListModel::CanResumeRole).toBool());
        QVERIFY(!QFileInfo::exists(targetPath));
        QCOMPARE(taskData(manager.tasks(), 0, TransferTaskListModel::StatusRole).toString(), QStringLiteral("paused"));

        manager.resumeTask(childId);
        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 8000);
        QCOMPARE(QFileInfo(targetPath).size(), qint64 { payload.size() });
        QCOMPARE(server.requestCount("/paused-item.bin"), 2);
    }

    void pausesAndResumesWholeDownloadGroup()
    {
        DownloadServer server;
        QVERIFY(server.listen());

        const QByteArray firstPayload(320 * 1024, 'g');
        const QByteArray secondPayload(320 * 1024, 'h');
        server.addPayload("/group-first.bin", firstPayload);
        server.addPayload("/group-second.bin", secondPayload);

        QTemporaryDir targetDirectory;
        QVERIFY(targetDirectory.isValid());
        const auto groupPath = targetDirectory.filePath(QStringLiteral("group"));
        QVERIFY(QDir().mkpath(groupPath));
        const auto firstPath = QDir(groupPath).filePath(QStringLiteral("first.bin"));
        const auto secondPath = QDir(groupPath).filePath(QStringLiteral("second.bin"));

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-pause-group"),
            .name = QStringLiteral("WebDAV pause group"),
            .serviceType = ServiceType::WebDAV,
        };
        std::vector<TransferManager::DownloadRequest> requests {
            { server.url(QStringLiteral("/group-first.bin")), firstPath, firstPayload.size() },
            { server.url(QStringLiteral("/group-second.bin")), secondPath, secondPayload.size() },
        };
        const auto groupId = manager.enqueueDownloads(serverConfig,
                                                      {},
                                                      QStringLiteral("Paused group"),
                                                      groupPath,
                                                      std::move(requests));
        QVERIFY(manager.selectGroup(groupId));
        QTRY_VERIFY_WITH_TIMEOUT(manager.downloadBytesPerSecond() > 0, 3000);

        manager.pauseTask(groupId);
        QTRY_COMPARE_WITH_TIMEOUT(taskData(manager.tasks(), 0, TransferTaskListModel::StatusRole).toString(),
                                  QStringLiteral("paused"),
                                  3000);
        QVERIFY(taskData(manager.tasks(), 0, TransferTaskListModel::CanResumeRole).toBool());
        for (auto row = 0; row < manager.detailTasks()->rowCount(); ++row) {
            QCOMPARE(taskData(manager.detailTasks(), row, TransferTaskListModel::StatusRole).toString(),
                     QStringLiteral("paused"));
        }

        manager.resumeTask(groupId);
        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 10000);
        QCOMPARE(QFileInfo(firstPath).size(), qint64 { firstPayload.size() });
        QCOMPARE(QFileInfo(secondPath).size(), qint64 { secondPayload.size() });
    }

    void retriesFailedDownloadGroup()
    {
        DownloadServer server;
        QVERIFY(server.listen());

        const QByteArray payload(64 * 1024, 'r');
        server.addPayload("/retry.bin", payload);
        server.failNextRequests("/retry.bin", 1);

        QTemporaryDir targetDirectory;
        QVERIFY(targetDirectory.isValid());
        const auto targetPath = targetDirectory.filePath(QStringLiteral("retry.bin"));

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-retry"),
            .name = QStringLiteral("WebDAV retry"),
            .serviceType = ServiceType::WebDAV,
        };
        const auto groupId = manager.enqueueDownload(serverConfig,
                                                     {},
                                                     server.url(QStringLiteral("/retry.bin")),
                                                     targetPath,
                                                     payload.size());
        QTRY_COMPARE_WITH_TIMEOUT(manager.failedCount(), 1, 3000);
        QVERIFY(taskData(manager.tasks(), 0, TransferTaskListModel::RetryableRole).toBool());
        QVERIFY(!QFileInfo::exists(targetPath));

        manager.retryTask(groupId);
        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 5000);
        QCOMPARE(QFileInfo(targetPath).size(), qint64 { payload.size() });
        QCOMPARE(server.requestCount("/retry.bin"), 2);
    }

    void retriesOnlySelectedFailedDownload()
    {
        DownloadServer server;
        QVERIFY(server.listen());

        const QByteArray firstPayload(48 * 1024, 'a');
        const QByteArray secondPayload(56 * 1024, 'b');
        server.addPayload("/retry-first.bin", firstPayload);
        server.addPayload("/retry-second.bin", secondPayload);
        server.failNextRequests("/retry-first.bin", 1);
        server.failNextRequests("/retry-second.bin", 1);

        QTemporaryDir targetDirectory;
        QVERIFY(targetDirectory.isValid());
        const auto firstPath = targetDirectory.filePath(QStringLiteral("retry-first.bin"));
        const auto secondPath = targetDirectory.filePath(QStringLiteral("retry-second.bin"));

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-retry-file"),
            .name = QStringLiteral("WebDAV retry file"),
            .serviceType = ServiceType::WebDAV,
        };
        std::vector<TransferManager::DownloadRequest> requests {
            { server.url(QStringLiteral("/retry-first.bin")), firstPath, firstPayload.size() },
            { server.url(QStringLiteral("/retry-second.bin")), secondPath, secondPayload.size() },
        };
        const auto groupId = manager.enqueueDownloads(serverConfig,
                                                      {},
                                                      QStringLiteral("Retry selected file"),
                                                      targetDirectory.path(),
                                                      std::move(requests));
        QVERIFY(manager.selectGroup(groupId));
        QTRY_COMPARE_WITH_TIMEOUT(manager.failedCount(), 1, 3000);

        const auto firstRow = taskRowForTarget(manager.detailTasks(), firstPath);
        const auto secondRow = taskRowForTarget(manager.detailTasks(), secondPath);
        QVERIFY(firstRow >= 0);
        QVERIFY(secondRow >= 0);
        QVERIFY(taskData(manager.detailTasks(), firstRow, TransferTaskListModel::RetryableRole).toBool());
        QVERIFY(taskData(manager.detailTasks(), secondRow, TransferTaskListModel::RetryableRole).toBool());

        const auto firstTaskId = taskData(manager.detailTasks(), firstRow, TransferTaskListModel::IdRole).toString();
        manager.retryTask(firstTaskId);

        QTRY_COMPARE_WITH_TIMEOUT(taskData(manager.detailTasks(), firstRow, TransferTaskListModel::StatusRole).toString(),
                                  QStringLiteral("done"),
                                  5000);
        QCOMPARE(taskData(manager.detailTasks(), secondRow, TransferTaskListModel::StatusRole).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(server.requestCount("/retry-first.bin"), 2);
        QCOMPARE(server.requestCount("/retry-second.bin"), 1);
        QCOMPARE(QFileInfo(firstPath).size(), qint64 { firstPayload.size() });
        QVERIFY(!QFileInfo::exists(secondPath));
    }

    void cancelingIndividualDownloadDeletesLocalFileAndAllowsRetry()
    {
        DownloadServer server;
        QVERIFY(server.listen());

        const QByteArray payload(320 * 1024, 'i');
        server.addPayload("/cancel-item.bin", payload);

        QTemporaryDir targetDirectory;
        QVERIFY(targetDirectory.isValid());
        const auto targetPath = targetDirectory.filePath(QStringLiteral("cancel-item.bin"));

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-cancel-item"),
            .name = QStringLiteral("WebDAV cancel item"),
            .serviceType = ServiceType::WebDAV,
        };
        const auto groupId = manager.enqueueDownload(serverConfig,
                                                     {},
                                                     server.url(QStringLiteral("/cancel-item.bin")),
                                                     targetPath,
                                                     payload.size());
        QVERIFY(manager.selectGroup(groupId));
        auto* details = manager.detailTasks();
        QCOMPARE(details->rowCount(), 1);
        const auto childId = taskData(details, 0, TransferTaskListModel::IdRole).toString();

        QTRY_VERIFY_WITH_TIMEOUT(taskData(details, 0, TransferTaskListModel::BytesDoneRole).toLongLong() > 0, 3000);
        manager.cancelTask(childId);
        QTRY_COMPARE_WITH_TIMEOUT(taskData(details, 0, TransferTaskListModel::StatusRole).toString(),
                                  QStringLiteral("canceled"),
                                  3000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(targetPath), 3000);
        QVERIFY(taskData(details, 0, TransferTaskListModel::RetryableRole).toBool());
        QVERIFY(taskData(manager.tasks(), 0, TransferTaskListModel::RetryableRole).toBool());

        manager.retryTask(childId);
        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 8000);
        QCOMPARE(QFileInfo(targetPath).size(), qint64 { payload.size() });
        QCOMPARE(server.requestCount("/cancel-item.bin"), 2);
    }

    void cancelingGroupDeletesAllLocalFiles()
    {
        DownloadServer server;
        QVERIFY(server.listen());

        const QByteArray completedPayload(4 * 1024, 'c');
        const QByteArray activePayload(512 * 1024, 'a');
        server.addPayload("/completed.bin", completedPayload);
        server.addPayload("/active.bin", activePayload);

        QTemporaryDir targetDirectory;
        QVERIFY(targetDirectory.isValid());
        const auto groupPath = targetDirectory.filePath(QStringLiteral("cancel-group"));
        QVERIFY(QDir().mkpath(groupPath));
        const auto completedPath = QDir(groupPath).filePath(QStringLiteral("completed.bin"));
        const auto activePath = QDir(groupPath).filePath(QStringLiteral("active.bin"));

        TransferManager manager;
        ServerConfig serverConfig {
            .id = QStringLiteral("webdav-cancel"),
            .name = QStringLiteral("WebDAV cancel"),
            .serviceType = ServiceType::WebDAV,
        };
        std::vector<TransferManager::DownloadRequest> requests {
            { server.url(QStringLiteral("/completed.bin")), completedPath, completedPayload.size() },
            { server.url(QStringLiteral("/active.bin")), activePath, activePayload.size() },
        };
        const auto groupId = manager.enqueueDownloads(serverConfig,
                                                      {},
                                                      QStringLiteral("Canceled group"),
                                                      groupPath,
                                                      std::move(requests));
        QVERIFY(manager.selectGroup(groupId));

        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(completedPath).size(), qint64 { completedPayload.size() }, 3000);
        const auto activeRow = taskRowForTarget(manager.detailTasks(), activePath);
        QVERIFY(activeRow >= 0);
        QTRY_VERIFY_WITH_TIMEOUT(taskData(manager.detailTasks(), activeRow, TransferTaskListModel::BytesDoneRole).toLongLong() > 0,
                                 3000);

        manager.cancelTask(groupId);
        QTRY_COMPARE_WITH_TIMEOUT(taskData(manager.tasks(), 0, TransferTaskListModel::StatusRole).toString(),
                                  QStringLiteral("canceled"),
                                  3000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(groupPath), 3000);
        QVERIFY(!QFileInfo::exists(completedPath));
        QVERIFY(!QFileInfo::exists(activePath));
        QVERIFY(taskData(manager.tasks(), 0, TransferTaskListModel::RetryableRole).toBool());
        for (auto row = 0; row < manager.detailTasks()->rowCount(); ++row) {
            QTRY_VERIFY_WITH_TIMEOUT(taskData(manager.detailTasks(), row, TransferTaskListModel::RetryableRole).toBool(),
                                     3000);
        }

        manager.retryTask(groupId);
        QTRY_COMPARE_WITH_TIMEOUT(manager.completedCount(), 1, 10000);
        QCOMPARE(QFileInfo(completedPath).size(), qint64 { completedPayload.size() });
        QCOMPARE(QFileInfo(activePath).size(), qint64 { activePayload.size() });
        QCOMPARE(server.requestCount("/completed.bin"), 2);
        QCOMPARE(server.requestCount("/active.bin"), 2);
    }
};

QTEST_MAIN(TransferManagerTest)
#include "TransferManagerTest.moc"
