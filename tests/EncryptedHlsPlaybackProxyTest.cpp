#include "services/webdav/EncryptedHlsPlaybackProxy.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <optional>

namespace {
class FakeWebDavServer final : public QObject {
public:
    explicit FakeWebDavServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                    buffer->append(socket->readAll());
                    if (!buffer->contains("\r\n\r\n")) {
                        return;
                    }
                    const auto requestLine = buffer->split('\n').front().trimmed().split(' ');
                    const auto path = requestLine.size() >= 2 ? requestLine.at(1) : QByteArray();
                    QByteArray body;
                    if (path.startsWith("/index.m3u8s")) {
                        body = manifest;
                    } else if (path.startsWith("/segment.ts")) {
                        body = encryptedSegment;
                        if (tamperSegment) {
                            body.back() ^= 0x01;
                        }
                    }
                    const auto status = body.isEmpty() ? 404 : 200;
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                  (status == 200 ? " OK\r\n" : " Not Found\r\n"));
                    socket->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
                    socket->write("Connection: close\r\n\r\n");
                    socket->write(body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            }
        });
    }

    bool listen()
    {
        return server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl manifestUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/index.m3u8s").arg(server.serverPort()));
    }

    QTcpServer server;
    QByteArray manifest { "#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:4.0,\nsegment.ts\n#EXT-X-ENDLIST\n" };
    QByteArray encryptedSegment {
        QByteArray::fromHex("000102030405060708090a0b0c0d0e0f") +
        QByteArray::fromHex("2202c30440943e4df9df8f7a75d44dca38dc0ac547ebbc31646a1e86c2") +
        QByteArray::fromHex("4607c2b2d88008a628da3a3f92378a13")
    };
    bool tamperSegment { false };
};

struct HttpResponse {
    int status { 0 };
    QByteArray body;
};

HttpResponse get(const QUrl& url, const QByteArray& range = {})
{
    QNetworkAccessManager manager;
    QEventLoop loop;
    QNetworkRequest request(url);
    if (!range.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("Range"), range);
    }
    auto* reply = manager.get(request);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(5000);
    loop.exec();
    const HttpResponse response {
        .status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        .body = reply->readAll(),
    };
    reply->deleteLater();
    return response;
}
}

class EncryptedHlsPlaybackProxyTest final : public QObject {
    Q_OBJECT

private slots:
    void verifiedPlaintextIsServedAndTamperedTagIsRejected();
};

void EncryptedHlsPlaybackProxyTest::verifiedPlaintextIsServedAndTamperedTagIsRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeWebDavServer origin;
    QVERIFY(origin.listen());

    const auto key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const TsslPackage package {
        .rootManifestDigest = QCryptographicHash::hash(origin.manifest, QCryptographicHash::Sha256),
        .segmentKeys = {
            { QStringLiteral("segment.ts"), key },
        },
    };
    const auto sourcePath = temporary.filePath(QStringLiteral("source.tssl"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(package.toJson()), package.toJson().size());
    source.close();

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    QVERIFY(store.restoreFromFile(sourcePath).has_value());
    EncryptedHlsPlaybackProxy proxy(store);
    ServerConfig server;
    server.id = QStringLiteral("test-webdav");
    server.name = QStringLiteral("Test WebDAV");
    server.baseUrl = origin.manifestUrl().adjusted(QUrl::RemoveFilename).toString();
    server.serviceType = ServiceType::WebDAV;

    std::optional<EncryptedHlsPrepareResult> prepared;
    QEventLoop prepareLoop;
    QTimer prepareTimer;
    prepareTimer.setSingleShot(true);
    connect(&prepareTimer, &QTimer::timeout, &prepareLoop, &QEventLoop::quit);
    proxy.prepareStream(server, {}, origin.manifestUrl(), [&](EncryptedHlsPrepareResult result) {
        prepared.emplace(std::move(result));
        prepareLoop.quit();
    });
    prepareTimer.start(5000);
    prepareLoop.exec();

    QVERIFY(prepared.has_value());
    if (!prepared->has_value()) {
        QFAIL(qPrintable(prepared->error()));
    }
    const auto rootResponse = get((**prepared).url);
    QCOMPARE(rootResponse.status, 200);
    QCOMPARE(rootResponse.body, origin.manifest);

    const auto segmentUrl = (**prepared).url.resolved(QUrl(QStringLiteral("segment.ts")));
    const auto segmentResponse = get(segmentUrl);
    QCOMPARE(segmentResponse.status, 200);
    QCOMPARE(segmentResponse.body, QByteArrayLiteral("Encrypted TS payload for TSSL"));

    const auto rangeResponse = get(segmentUrl, QByteArrayLiteral("bytes=10-15"));
    QCOMPARE(rangeResponse.status, 206);
    QCOMPARE(rangeResponse.body, QByteArrayLiteral("TS pay"));

    origin.tamperSegment = true;
    const auto tamperedResponse = get(segmentUrl, QByteArrayLiteral("bytes=10-15"));
    QCOMPARE(tamperedResponse.status, 502);
    QVERIFY(!tamperedResponse.body.contains(QByteArrayLiteral("Encrypted TS payload for TSSL")));
}

QTEST_GUILESS_MAIN(EncryptedHlsPlaybackProxyTest)

#include "EncryptedHlsPlaybackProxyTest.moc"
