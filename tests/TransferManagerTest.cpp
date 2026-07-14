#include "services/webdav/TransferManager.h"

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

    QUrl url(const QString& path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path));
    }

private:
    QTcpServer m_server;
    QHash<QByteArray, QByteArray> m_payloads;
};

QVariant taskData(TransferTaskListModel* model, int row, TransferTaskListModel::Role role)
{
    return model->data(model->index(row, 0), role);
}
}

class TransferManagerTest final : public QObject {
    Q_OBJECT

private slots:
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
};

QTEST_MAIN(TransferManagerTest)
#include "TransferManagerTest.moc"
