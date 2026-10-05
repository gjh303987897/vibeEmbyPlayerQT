#include "services/webdav/WebDavClient.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <memory>
#include <optional>

namespace {
class DirectoryEndpoint final : public QTcpServer {
public:
    QByteArray status { "201 Created" };
    QList<QByteArray> methods;
    QList<QByteArray> paths;

    DirectoryEndpoint()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                    *buffer += socket->readAll();
                    const auto headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const auto lines = buffer->first(headerEnd).split('\n');
                    qint64 length = 0;
                    for (const auto& line : lines) {
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toLongLong();
                    }
                    if (buffer->size() - headerEnd - 4 < length) return;
                    const auto parts = lines.first().split(' ');
                    methods.append(parts.value(0));
                    paths.append(parts.value(1));
                    buffer->clear();
                    const auto listing = methods.last() == "PROPFIND";
                    const auto body = listing ? QByteArrayLiteral(
                        "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>/dav/new/</D:href>"
                        "<D:propstat><D:prop><D:displayname>new</D:displayname>"
                        "<D:resourcetype><D:collection/></D:resourcetype></D:prop>"
                        "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>")
                        : QByteArray();
                    socket->write(QByteArrayLiteral("HTTP/1.1 ") + (listing ? QByteArrayLiteral("207 Multi-Status") : status)
                        + QByteArrayLiteral("\r\nContent-Type: application/xml\r\nContent-Length: ")
                        + QByteArray::number(body.size()) + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    QUrl rootUrl() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/dav/").arg(serverPort())); }
};
}

class WebDavClientTest final : public QObject {
    Q_OBJECT

private slots:
    void preservesNamesAndParentEscapes()
    {
        const auto child = WebDavClient::childDirectoryUrl(QUrl(QStringLiteral("https://example.com/dav/a%252Fb/")),
                                                          QStringLiteral("  新 文件夹#100%?  "));
        QVERIFY(child.has_value());
        QCOMPARE(child->host(), QStringLiteral("example.com"));
        QCOMPARE(child->path(QUrl::FullyDecoded), QStringLiteral("/dav/a%2Fb/新 文件夹#100%?/"));
        QVERIFY(child->toEncoded().contains("/a%252Fb/"));
        QVERIFY(child->toEncoded().contains("%23100%25%3F/"));
        QVERIFY(!child->hasQuery());
        QVERIFY(!child->hasFragment());
    }

    void rejectsInvalidNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("empty") << QString();
        QTest::newRow("whitespace") << QStringLiteral("   ");
        QTest::newRow("dot") << QStringLiteral(".");
        QTest::newRow("parent") << QStringLiteral("..");
        QTest::newRow("nested path") << QStringLiteral("one/two");
        QTest::newRow("backslash") << QStringLiteral("one\\two");
        QTest::newRow("newline") << QStringLiteral("one\ntwo");
        QTest::newRow("null") << QStringLiteral("one") + QChar(0);
    }

    void rejectsInvalidNames()
    {
        QFETCH(QString, name);
        const auto child = WebDavClient::childDirectoryUrl(QUrl(QStringLiteral("https://example.com/dav/")), name);
        QVERIFY(!child.has_value());
        QVERIFY(!child.error().isEmpty());
    }

    void createsDirectoryAndReportsFailures_data()
    {
        QTest::addColumn<QByteArray>("status");
        QTest::addColumn<int>("code");
        QTest::newRow("created") << QByteArrayLiteral("201 Created") << 201;
        QTest::newRow("already exists") << QByteArrayLiteral("405 Method Not Allowed") << 405;
        QTest::newRow("forbidden") << QByteArrayLiteral("403 Forbidden") << 403;
        QTest::newRow("missing parent") << QByteArrayLiteral("409 Conflict") << 409;
        QTest::newRow("full storage") << QByteArrayLiteral("507 Insufficient Storage") << 507;
        QTest::newRow("unexpected success code") << QByteArrayLiteral("200 OK") << 200;
    }

    void createsDirectoryAndReportsFailures()
    {
        QFETCH(QByteArray, status);
        QFETCH(int, code);
        DirectoryEndpoint endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        endpoint.status = status;
        WebDavClient client;
        ServerConfig server;
        std::optional<WebDavVoidResult> result;
        const auto child = WebDavClient::childDirectoryUrl(endpoint.rootUrl(), QStringLiteral("new"));
        QVERIFY(child.has_value());
        client.createDirectory(server, {}, *child, [&](WebDavVoidResult value) { result = std::move(value); });
        QTRY_VERIFY(result.has_value());
        QCOMPARE(result->has_value(), code == 201);
        if (code != 201) QCOMPARE(result->error().httpStatus, code);
        QCOMPARE(endpoint.methods, QList<QByteArray> { QByteArrayLiteral("MKCOL") });
        QCOMPARE(endpoint.paths, QList<QByteArray> { QByteArrayLiteral("/dav/new/") });
    }

    void refreshesListingAfterCreation()
    {
        DirectoryEndpoint endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        WebDavClient client;
        ServerConfig server;
        std::optional<WebDavVoidResult> creation;
        std::optional<WebDavListResult> listing;
        const auto child = WebDavClient::childDirectoryUrl(endpoint.rootUrl(), QStringLiteral("new"));
        QVERIFY(child.has_value());
        client.createDirectory(server, {}, *child, [&](WebDavVoidResult result) {
            creation = std::move(result);
            if (creation->has_value())
                client.listDirectory(server, {}, endpoint.rootUrl(), [&](WebDavListResult items) { listing = std::move(items); });
        });
        QTRY_VERIFY(listing.has_value());
        QVERIFY(creation->has_value());
        QVERIFY(listing->has_value());
        QCOMPARE((**listing).size(), 1);
        QCOMPARE((**listing).front().name, QStringLiteral("new"));
        QVERIFY((**listing).front().directory);
        QCOMPARE(endpoint.methods, (QList<QByteArray> { QByteArrayLiteral("MKCOL"), QByteArrayLiteral("PROPFIND") }));
    }
};

QTEST_GUILESS_MAIN(WebDavClientTest)
#include "WebDavClientTest.moc"
