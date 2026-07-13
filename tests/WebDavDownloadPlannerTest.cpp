#include "services/webdav/WebDavClient.h"
#include "services/webdav/WebDavDownloadPlanner.h"

#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <optional>

namespace {
QByteArray responseElement(const QByteArray& href,
                           const QByteArray& displayName,
                           bool directory,
                           qint64 size = -1)
{
    const auto resourceType = directory
        ? QByteArrayLiteral("<D:resourcetype><D:collection/></D:resourcetype>")
        : QByteArrayLiteral("<D:resourcetype/>");
    const auto contentLength = size >= 0
        ? QByteArrayLiteral("<D:getcontentlength>") + QByteArray::number(size) + QByteArrayLiteral("</D:getcontentlength>")
        : QByteArray {};
    return QByteArrayLiteral("<D:response><D:href>") + href + QByteArrayLiteral("</D:href>")
        + QByteArrayLiteral("<D:propstat><D:prop><D:displayname>") + displayName
        + QByteArrayLiteral("</D:displayname>") + resourceType + contentLength
        + QByteArrayLiteral("</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
}

QByteArray multistatus(std::initializer_list<QByteArray> responses)
{
    QByteArray body = QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">");
    for (const auto& response : responses) {
        body += response;
    }
    body += QByteArrayLiteral("</D:multistatus>");
    return body;
}

class LocalWebDavServer final : public QObject {
    Q_OBJECT

public:
    explicit LocalWebDavServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    auto& buffer = m_buffers[socket];
                    buffer += socket->readAll();
                    const auto headerEnd = buffer.indexOf(QByteArrayLiteral("\r\n\r\n"));
                    if (headerEnd < 0) {
                        return;
                    }

                    qint64 contentLength = 0;
                    const auto headers = buffer.first(headerEnd).split('\n');
                    for (const auto& rawHeader : headers) {
                        const auto header = rawHeader.trimmed();
                        if (header.toLower().startsWith("content-length:")) {
                            contentLength = header.mid(QByteArrayLiteral("Content-Length:").size()).trimmed().toLongLong();
                        }
                    }
                    if (buffer.size() < headerEnd + 4 + contentLength) {
                        return;
                    }

                    const auto requestLine = headers.front().trimmed();
                    const auto parts = requestLine.split(' ');
                    const auto method = parts.value(0);
                    const auto path = parts.value(1);
                    m_buffers.remove(socket);
                    if (method != QByteArrayLiteral("PROPFIND")) {
                        writeResponse(socket, 405, QByteArrayLiteral("Method Not Allowed"), {});
                        return;
                    }

                    ++m_requestCounts[path];
                    const auto body = responseFor(path);
                    if (body.isEmpty()) {
                        writeResponse(socket, 404, QByteArrayLiteral("Not Found"), {});
                        return;
                    }
                    writeResponse(socket, 207, QByteArrayLiteral("Multi-Status"), body);
                });
                connect(socket, &QObject::destroyed, this, [this, socket]() {
                    m_buffers.remove(socket);
                });
            }
        });
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl rootUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/root/").arg(m_server.serverPort()));
    }

    int requestCount(const QByteArray& path) const
    {
        return m_requestCounts.value(path);
    }

private:
    QByteArray responseFor(const QByteArray& path) const
    {
        if (path == QByteArrayLiteral("/root/")) {
            return multistatus({
                responseElement("/root/", "root", true),
                responseElement("/root/alpha.txt", "alpha.txt", false, 5),
                responseElement("/root/nested/", "nested", true),
                responseElement("/root/empty/", "empty", true),
            });
        }
        if (path == QByteArrayLiteral("/root/nested/")) {
            return multistatus({
                responseElement("/root/nested/", "nested", true),
                responseElement("/root/nested/beta.bin", "beta.bin", false, 7),
                responseElement("/root/nested/escape.txt", "../escape.txt", false, 11),
            });
        }
        if (path == QByteArrayLiteral("/root/empty/")) {
            return multistatus({
                responseElement("/root/empty/", "empty", true),
            });
        }
        return {};
    }

    static void writeResponse(QTcpSocket* socket,
                              int statusCode,
                              const QByteArray& reason,
                              const QByteArray& body)
    {
        QByteArray response = QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(statusCode) + ' ' + reason
            + QByteArrayLiteral("\r\nContent-Type: application/xml; charset=utf-8\r\nContent-Length: ")
            + QByteArray::number(body.size())
            + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QHash<QByteArray, int> m_requestCounts;
};
}

class WebDavDownloadPlannerTest final : public QObject {
    Q_OBJECT

private slots:
    void plansNestedFolderWithOneRequestPerDirectory()
    {
        LocalWebDavServer server;
        QVERIFY(server.listen());

        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const auto targetPath = temporaryDirectory.filePath(QStringLiteral("download"));
        QVERIFY(QDir().mkpath(targetPath));
        QFile existingFile(QDir(targetPath).filePath(QStringLiteral("alpha.txt")));
        QVERIFY(existingFile.open(QIODevice::WriteOnly));
        QCOMPARE(existingFile.write("existing"), 8);
        existingFile.close();

        ServerConfig config;
        config.id = QStringLiteral("test-webdav");
        config.name = QStringLiteral("Test WebDAV");
        config.baseUrl = server.rootUrl().toString();
        config.serviceType = ServiceType::WebDAV;

        WebDavItem rootItem;
        rootItem.name = QStringLiteral("root");
        rootItem.url = server.rootUrl();
        rootItem.directory = true;

        WebDavClient client;
        WebDavDownloadPlanner planner(client);
        std::optional<WebDavDownloadPlanResult> result;
        QEventLoop eventLoop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
        timeout.start(5000);

        planner.buildPlan(config,
                          {},
                          rootItem,
                          targetPath,
                          [&result, &eventLoop](WebDavDownloadPlanResult planned) {
            result.emplace(std::move(planned));
            eventLoop.quit();
        });
        eventLoop.exec();

        QVERIFY2(result.has_value(), "Planner timed out");
        QVERIFY2(result->has_value(), qPrintable(result->error().message));
        const auto& plan = result->value();
        QCOMPARE(plan.files.size(), size_t { 3 });
        QCOMPARE(plan.directories.size(), size_t { 3 });
        QCOMPARE(plan.bytesTotal, qint64 { 23 });
        QVERIFY(plan.sizeComplete);
        QCOMPARE(server.requestCount("/root/"), 1);
        QCOMPARE(server.requestCount("/root/nested/"), 1);
        QCOMPARE(server.requestCount("/root/empty/"), 1);

        const auto alpha = std::ranges::find_if(plan.files, [](const WebDavDownloadEntry& entry) {
            return entry.remoteUrl.path().endsWith(QStringLiteral("/alpha.txt"));
        });
        QVERIFY(alpha != plan.files.end());
        QCOMPARE(QFileInfo(alpha->localPath).fileName(), QStringLiteral("alpha (1).txt"));

        const auto escaped = std::ranges::find_if(plan.files, [](const WebDavDownloadEntry& entry) {
            return entry.remoteUrl.path().endsWith(QStringLiteral("/escape.txt"));
        });
        QVERIFY(escaped != plan.files.end());
        QCOMPARE(QFileInfo(escaped->localPath).fileName(), QStringLiteral("escape.txt"));
        QVERIFY(QDir::cleanPath(escaped->localPath).startsWith(QDir::cleanPath(targetPath)));
    }
};

QTEST_MAIN(WebDavDownloadPlannerTest)
#include "WebDavDownloadPlannerTest.moc"
