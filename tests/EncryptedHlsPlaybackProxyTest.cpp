#include "services/webdav/EncryptedHlsPlaybackProxy.h"
#include "services/encryptedhls/EncryptedHlsTarContainer.h"
#include "services/webdav/AesGcmDecryptor.h"
#include "services/webdav/HlsManifestValidator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <QStringDecoder>

#include <optional>

namespace {
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray identifierBytes(char value = 'A')
{
    return QByteArray(TsslPackage::identifierLength, value);
}

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
                    const auto lower = buffer->toLower();
                    const auto rangeHeaderAt = lower.indexOf("range:");
                    const auto rangeMatch = rangeHeaderAt >= 0
                        ? QRegularExpression(QStringLiteral("bytes=(\\d+)-(\\d*)"))
                              .match(QString::fromLatin1(buffer->mid(rangeHeaderAt)))
                        : QRegularExpressionMatch();
                    QByteArray body;
                    int status = 200;
                    QByteArray extraHeaders;
                    // Emulate a throttling server: the first N requests fail
                    // outright (503), later ones succeed. Reproduces the
                    // intermittent preview blanks users saw with concurrency.
                    if (failFirstRequests > 0) {
                        --failFirstRequests;
                        socket->write("HTTP/1.1 503 Slow Down\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        ++requestCount;
                        return;
                    }
                    QByteArray full;
                    if (path.startsWith("/index.m3u8s")) {
                        full = manifest;
                    } else if (path.startsWith("/movie.m3u8sp")) {
                        full = container;
                    } else if (path.startsWith("/segment.ts")) {
                        full = encryptedSegment;
                        if (tamperSegment) {
                            full.back() ^= 0x01;
                        }
                    }
                    if (full.isEmpty()) {
                        status = 404;
                    } else if (supportsRange && rangeMatch.hasMatch()) {
                        // Serve exactly one bounded range like a conforming
                        // WebDAV server; the listing preview path depends on
                        // 206 responses to avoid full-manifest downloads.
                        const auto start = rangeMatch.captured(1).toLongLong();
                        auto end = rangeMatch.captured(2).isEmpty()
                            ? full.size() - 1
                            : std::min(rangeMatch.captured(2).toLongLong(), full.size() - 1);
                        if (start >= full.size() || end < start) {
                            status = 416;
                        } else {
                            status = 206;
                            body = full.mid(static_cast<int>(start), static_cast<int>(end - start + 1));
                            extraHeaders = "Content-Range: bytes " + QByteArray::number(start) + '-' +
                                QByteArray::number(end) + '/' + QByteArray::number(full.size()) + "\r\n";
                        }
                    } else {
                        body = full;
                    }
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                  (status == 200 ? " OK\r\n" : status == 206 ? " Partial Content\r\n" : " Not Found\r\n"));
                    socket->write("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
                    socket->write(extraHeaders);
                    socket->write("Connection: close\r\n\r\n");
                    socket->write(body);
                    socket->disconnectFromHost();
                    ++requestCount;
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

    QUrl containerUrl() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/movie.m3u8sp").arg(server.serverPort()));
    }

    QTcpServer server;
    QByteArray manifest {
        QByteArrayLiteral("#EXTM3U\n#M3U8S-IDENTIFIER:") + identifierBytes() +
        QByteArrayLiteral("\n#EXT-X-VERSION:3\n#EXTINF:4.0,\nsegment.ts\n#EXT-X-ENDLIST\n")
    };
    QByteArray encryptedSegment {
        QByteArray::fromHex("000102030405060708090a0b0c0d0e0f") +
        QByteArray::fromHex("2202c30440943e4df9df8f7a75d44dca38dc0ac547ebbc31646a1e86c2") +
        QByteArray::fromHex("4607c2b2d88008a628da3a3f92378a13")
    };
    QByteArray container;
    bool tamperSegment { false };
    bool supportsRange { true };
    int failFirstRequests { 0 };
    int requestCount { 0 };
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
    void remoteIdentifierPreviewIsResolvedWithoutTssl();
    void remoteMetadataRestoresSourceFileNameWithTssl();
    void previewUsesSingleRangedProbeAndCachesResult();
    void containerPreviewSurvivesOversizedManifestWithUtf8Head();
    void containerPreviewReportsErrorWithoutRangeSupport();
    void previewRetriesTransientFailuresAndCachesNothingOnFailure();
    void previewFallsBackToFullReadWithoutRangeSupport();
    void verifiedPlaintextIsServedAndTamperedTagIsRejected();
    void localPackageRestoresSourceNameAndVerifiesSegments();
    void mismatchedIdentifierIsRejectedBeforePlayback();
};

void EncryptedHlsPlaybackProxyTest::remoteIdentifierPreviewIsResolvedWithoutTssl()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeWebDavServer origin;
    QVERIFY(origin.listen());

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    EncryptedHlsPlaybackProxy proxy(store);
    ServerConfig server;
    server.id = QStringLiteral("preview-webdav");
    server.name = QStringLiteral("Preview WebDAV");
    server.baseUrl = origin.manifestUrl().adjusted(QUrl::RemoveFilename).toString();
    server.serviceType = ServiceType::WebDAV;

    std::optional<EncryptedHlsIdentifierPreviewResult> resolved;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    proxy.resolveIdentifierPreview(server, {}, origin.manifestUrl(),
                                   [&](EncryptedHlsIdentifierPreviewResult result) {
        resolved.emplace(std::move(result));
        loop.quit();
    });
    timer.start(5000);
    loop.exec();

    QVERIFY(resolved.has_value());
    if (!resolved->has_value()) {
        QFAIL(qPrintable(resolved->error()));
    }
    QCOMPARE((**resolved).identifier, QStringLiteral("AAAAAAAAAAAAAAAA...AAAAAAAAAAAA"));
    QVERIFY((**resolved).sourceFileName.isEmpty());
}

void EncryptedHlsPlaybackProxyTest::remoteMetadataRestoresSourceFileNameWithTssl()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeWebDavServer origin;
    QVERIFY(origin.listen());

    const auto identifier = identifierBytes('S');
    const auto sourceFileName = QStringLiteral("Remote Original Movie.mkv");
    const auto sourceNameKey = QByteArray(32, '\x35');
    const auto sourceNameIv = QByteArray(16, '\x17');
    const auto encryptedSourceName = AesGcmDecryptor::encryptAuthenticatedData(
        sourceFileName.toUtf8(),
        sourceNameKey,
        sourceNameIv,
        TsslPackage::sourceFileNameAuthenticatedData(identifier));
    QVERIFY(encryptedSourceName.has_value());

    const QByteArray plainManifest =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:4.0,\nsegment.ts\n#EXT-X-ENDLIST\n";
    auto manifest = HlsManifestValidator::insertM3u8sIdentifier(plainManifest, identifier);
    QVERIFY(manifest.has_value());
    manifest = HlsManifestValidator::insertEncryptedSourceFileName(*manifest, *encryptedSourceName);
    QVERIFY(manifest.has_value());
    origin.manifest = *manifest;

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    const TsslPackage package {
        .version = 3,
        .identifier = identifier,
        .rootManifestDigest = QCryptographicHash::hash(*manifest, QCryptographicHash::Sha256),
        .encryptedSourceFileName = *encryptedSourceName,
        .sourceFileNameKey = sourceNameKey,
        .segmentKeys = {
            { QStringLiteral("segment.ts"), QByteArray(32, '\x42') },
        },
    };
    QVERIFY(store.savePackage(package).has_value());
    EncryptedHlsPlaybackProxy proxy(store);
    ServerConfig server;
    server.id = QStringLiteral("metadata-webdav");
    server.name = QStringLiteral("Metadata WebDAV");
    server.baseUrl = origin.manifestUrl().adjusted(QUrl::RemoveFilename).toString();
    server.serviceType = ServiceType::WebDAV;

    std::optional<EncryptedHlsIdentifierPreviewResult> resolved;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    proxy.resolveIdentifierPreview(server, {}, origin.manifestUrl(),
                                   [&](EncryptedHlsIdentifierPreviewResult result) {
        resolved.emplace(std::move(result));
        loop.quit();
    });
    timer.start(5000);
    loop.exec();

    QVERIFY(resolved.has_value());
    if (!resolved->has_value()) {
        QFAIL(qPrintable(resolved->error()));
    }
    QCOMPARE((**resolved).identifier, QStringLiteral("SSSSSSSSSSSSSSSS...SSSSSSSSSSSS"));
    QCOMPARE((**resolved).sourceFileName, sourceFileName);
}

void EncryptedHlsPlaybackProxyTest::previewUsesSingleRangedProbeAndCachesResult()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeWebDavServer origin;
    QVERIFY(origin.listen());

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    EncryptedHlsPlaybackProxy proxy(store);
    ServerConfig server;
    server.id = QStringLiteral("ranged-webdav");
    server.name = QStringLiteral("Ranged WebDAV");
    server.baseUrl = origin.manifestUrl().adjusted(QUrl::RemoveFilename).toString();
    server.serviceType = ServiceType::WebDAV;

    const auto resolve = [&] {
        std::optional<EncryptedHlsIdentifierPreviewResult> resolved;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        proxy.resolveIdentifierPreview(server, {}, origin.manifestUrl(), QStringLiteral("42|now"),
                                       [&](EncryptedHlsIdentifierPreviewResult result) {
            resolved.emplace(std::move(result));
            loop.quit();
        });
        timer.start(5000);
        loop.exec();
        return resolved;
    };

    const auto first = resolve();
    QVERIFY(first.has_value());
    if (!first->has_value()) {
        QFAIL(qPrintable(first->error()));
    }
    QCOMPARE((**first).identifier, QStringLiteral("AAAAAAAAAAAAAAAA...AAAAAAAAAAAA"));
    // A conforming ranged server must see exactly one request per uncached
    // preview: the 16 KiB head window, never a second full-manifest fetch.
    QCOMPARE(origin.requestCount, 1);

    const auto second = resolve();
    QVERIFY(second.has_value());
    QVERIFY(second->has_value());
    // Same URL + revision: served from the preview cache without traffic.
    QCOMPARE(origin.requestCount, 1);
}

void EncryptedHlsPlaybackProxyTest::previewFallsBackToFullReadWithoutRangeSupport()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeWebDavServer origin;
    origin.supportsRange = false;
    QVERIFY(origin.listen());

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    EncryptedHlsPlaybackProxy proxy(store);
    ServerConfig server;
    server.id = QStringLiteral("plain-webdav");
    server.name = QStringLiteral("Plain WebDAV");
    server.baseUrl = origin.manifestUrl().adjusted(QUrl::RemoveFilename).toString();
    server.serviceType = ServiceType::WebDAV;

    std::optional<EncryptedHlsIdentifierPreviewResult> resolved;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    proxy.resolveIdentifierPreview(server, {}, origin.manifestUrl(), QString {},
                                   [&](EncryptedHlsIdentifierPreviewResult result) {
        resolved.emplace(std::move(result));
        loop.quit();
    });
    timer.start(5000);
    loop.exec();

    // Servers that answer ranged requests with 200 must still resolve the
    // preview through the historical whole-manifest path.
    QVERIFY(resolved.has_value());
    if (!resolved->has_value()) {
        QFAIL(qPrintable(resolved->error()));
    }
    QCOMPARE((**resolved).identifier, QStringLiteral("AAAAAAAAAAAAAAAA...AAAAAAAAAAAA"));
}

void EncryptedHlsPlaybackProxyTest::verifiedPlaintextIsServedAndTamperedTagIsRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeWebDavServer origin;
    QVERIFY(origin.listen());

    const auto key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const TsslPackage package {
        .identifier = identifierBytes(),
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

void EncryptedHlsPlaybackProxyTest::localPackageRestoresSourceNameAndVerifiesSegments()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto identifier = identifierBytes('L');
    const auto sourceFileName = QStringLiteral("Local Private Movie.mkv");
    const auto sourceNameKey = QByteArray(32, '\x45');
    const auto sourceNameIv = QByteArray(16, '\x23');
    const auto sourceNameEncrypted = AesGcmDecryptor::encryptAuthenticatedData(
        sourceFileName.toUtf8(),
        sourceNameKey,
        sourceNameIv,
        TsslPackage::sourceFileNameAuthenticatedData(identifier));
    QVERIFY(sourceNameEncrypted.has_value());

    const QByteArray plainManifest =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:4.0,\nsegment.ts\n#EXT-X-ENDLIST\n";
    auto manifest = HlsManifestValidator::insertM3u8sIdentifier(plainManifest, identifier);
    QVERIFY(manifest.has_value());
    manifest = HlsManifestValidator::insertEncryptedSourceFileName(*manifest, *sourceNameEncrypted);
    QVERIFY(manifest.has_value());

    const auto segmentKey = QByteArray::fromHex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const auto segmentIv = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const auto encryptedSegment = AesGcmDecryptor::encryptTsSegment(
        QByteArrayLiteral("local verified segment"), segmentKey, segmentIv);
    QVERIFY(encryptedSegment.has_value());

    const auto manifestPath = temporary.filePath(QStringLiteral("index.m3u8s"));
    QFile manifestFile(manifestPath);
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    QCOMPARE(manifestFile.write(*manifest), manifest->size());
    manifestFile.close();
    const auto segmentPath = temporary.filePath(QStringLiteral("segment.ts"));
    QFile segmentFile(segmentPath);
    QVERIFY(segmentFile.open(QIODevice::WriteOnly));
    QCOMPARE(segmentFile.write(*encryptedSegment), encryptedSegment->size());
    segmentFile.close();

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    const TsslPackage package {
        .version = 3,
        .identifier = identifier,
        .rootManifestDigest = QCryptographicHash::hash(*manifest, QCryptographicHash::Sha256),
        .encryptedSourceFileName = *sourceNameEncrypted,
        .sourceFileNameKey = sourceNameKey,
        .segmentKeys = {
            { QStringLiteral("segment.ts"), segmentKey },
        },
    };
    QVERIFY(store.savePackage(package).has_value());
    EncryptedHlsPlaybackProxy proxy(store);

    std::optional<EncryptedHlsPrepareResult> prepared;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    proxy.prepareLocalStream(manifestPath, [&](EncryptedHlsPrepareResult result) {
        prepared.emplace(std::move(result));
        loop.quit();
    });
    timer.start(5000);
    loop.exec();

    QVERIFY(prepared.has_value());
    if (!prepared->has_value()) {
        QFAIL(qPrintable(prepared->error()));
    }
    QCOMPARE((**prepared).displayName, sourceFileName);
    QCOMPARE(get((**prepared).url).body, *manifest);
    const auto segmentUrl = (**prepared).url.resolved(QUrl(QStringLiteral("segment.ts")));
    const auto response = get(segmentUrl);
    QCOMPARE(response.status, 200);
    QCOMPARE(response.body, QByteArrayLiteral("local verified segment"));

    QVERIFY(segmentFile.open(QIODevice::ReadWrite));
    auto tampered = segmentFile.readAll();
    tampered.back() ^= 0x01;
    segmentFile.resize(0);
    QVERIFY(segmentFile.seek(0));
    QCOMPARE(segmentFile.write(tampered), tampered.size());
    segmentFile.close();
    QCOMPARE(get(segmentUrl).status, 502);
}

void EncryptedHlsPlaybackProxyTest::mismatchedIdentifierIsRejectedBeforePlayback()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeWebDavServer origin;
    QVERIFY(origin.listen());

    const TsslPackage package {
        .identifier = identifierBytes('B'),
        .rootManifestDigest = QCryptographicHash::hash(origin.manifest, QCryptographicHash::Sha256),
        .segmentKeys = {
            { QStringLiteral("segment.ts"), QByteArray(32, '\x11') },
        },
    };
    TsslStore store(temporary.filePath(QStringLiteral("store")));
    QVERIFY(store.savePackage(package).has_value());
    EncryptedHlsPlaybackProxy proxy(store);
    ServerConfig server;
    server.id = QStringLiteral("test-webdav");
    server.name = QStringLiteral("Test WebDAV");
    server.baseUrl = origin.manifestUrl().adjusted(QUrl::RemoveFilename).toString();
    server.serviceType = ServiceType::WebDAV;

    std::optional<EncryptedHlsPrepareResult> prepared;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    proxy.prepareStream(server, {}, origin.manifestUrl(), [&](EncryptedHlsPrepareResult result) {
        prepared.emplace(std::move(result));
        loop.quit();
    });
    timer.start(5000);
    loop.exec();

    QVERIFY(prepared.has_value());
    QVERIFY(!prepared->has_value());
    QVERIFY(prepared->error().contains(QStringLiteral("identifier"), Qt::CaseInsensitive));
}

void EncryptedHlsPlaybackProxyTest::containerPreviewSurvivesOversizedManifestWithUtf8Head()
{
    // Regression: real .m3u8sp manifests exceed the 16 KiB listing window
    // (segment URI lines) and the window edge cuts multi-byte UTF-8 names in
    // the segment list. Both used to abort the container preview with an
    // error dialog when entering a WebDAV folder.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    QByteArray manifest = QByteArrayLiteral("#EXTM3U\n#M3U8S-IDENTIFIER:") + identifierBytes()
        + QByteArrayLiteral("\n#EXT-X-VERSION:3\n");
    // Segment lines are 12 bytes: 第(3) 0(1) 1(1) 话(3) . t s \n. Fill the
    // manifest up to the probe window, then pad the final line so the 16 KiB
    // boundary keeps only the first byte of a 话 sequence, reproducing the
    // real-world mid-character window cut.
    const QByteArray cjkLine = QString::fromUtf8("\u7b2c01\u8a71.ts\n").toUtf8();
    QCOMPARE(cjkLine.size(), 12);
    while (manifest.size() + cjkLine.size() < 16 * 1024) {
        manifest += cjkLine;
    }
    // Start one final line so that its 话 sequence straddles the 16 KiB edge:
    // the character occupies bytes [start+5, start+8), so start+5 <= 16383
    // < start+8 leaves the window holding only its first two bytes.
    while (manifest.size() < 16 * 1024 - 6) {
        manifest += 'A';
    }
    manifest += cjkLine;
    QVERIFY(manifest.size() >= 16 * 1024);
    manifest += QByteArrayLiteral("#EXT-X-ENDLIST\n");
    // The fixed probe window must land mid-line (inside a multi-byte CJK
    // sequence here): exactly the boundary the container preview used to
    // choke on once the manifest outgrew the window.
    QVERIFY(!manifest.first(16 * 1024).endsWith('\n'));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("index.m3u8s")), manifest));
    QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("segments"))));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("segments/000001.ts")),
                      QByteArrayLiteral("encrypted-segment-bytes")));

    const auto archivePath = temporary.filePath(QStringLiteral("movie.m3u8sp"));
    const auto built = EncryptedHlsTarContainer::build(temporary.path(), archivePath,
                                                       QStringLiteral("index.m3u8s"));
    if (!built) QFAIL(qPrintable(built.error()));

    QFile archive(archivePath);
    QVERIFY(archive.open(QIODevice::ReadOnly));

    FakeWebDavServer origin;
    QVERIFY(origin.listen());
    origin.container = archive.readAll();

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    EncryptedHlsPlaybackProxy proxy(store);
    ServerConfig server;
    server.id = QStringLiteral("container-preview-webdav");
    server.name = QStringLiteral("Container Preview WebDAV");
    server.baseUrl = origin.containerUrl().adjusted(QUrl::RemoveFilename).toString();
    server.serviceType = ServiceType::WebDAV;

    std::optional<EncryptedHlsIdentifierPreviewResult> resolved;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    proxy.resolveIdentifierPreview(server, {}, origin.containerUrl(), QStringLiteral("100|now"),
                                   [&](EncryptedHlsIdentifierPreviewResult result) {
        resolved.emplace(std::move(result));
        loop.quit();
    });
    timer.start(5000);
    loop.exec();

    QVERIFY(resolved.has_value());
    if (!resolved->has_value()) {
        QFAIL(qPrintable(resolved->error()));
    }
    QCOMPARE((**resolved).identifier, QStringLiteral("AAAAAAAAAAAAAAAA...AAAAAAAAAAAA"));
}

void EncryptedHlsPlaybackProxyTest::containerPreviewReportsErrorWithoutRangeSupport()
{
    // Regression: a server that rejects ranged reads used to drive the
    // container probe's error path, which invoked a moved-from callback and
    // crashed the process (std::bad_function_call -> abort dialog).
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QByteArray manifest = QByteArrayLiteral("#EXTM3U\n#M3U8S-IDENTIFIER:") + identifierBytes()
        + QByteArrayLiteral("\n#EXT-X-VERSION:3\n000001.ts\n#EXT-X-ENDLIST\n");
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("index.m3u8s")), manifest));
    QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("segments"))));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("segments/000001.ts")),
                      QByteArrayLiteral("encrypted-segment-bytes")));
    const auto archivePath = temporary.filePath(QStringLiteral("movie.m3u8sp"));
    const auto built = EncryptedHlsTarContainer::build(temporary.path(), archivePath,
                                                       QStringLiteral("index.m3u8s"));
    if (!built) QFAIL(qPrintable(built.error()));
    QFile archive(archivePath);
    QVERIFY(archive.open(QIODevice::ReadOnly));

    FakeWebDavServer origin;
    QVERIFY(origin.listen());
    origin.container = archive.readAll();
    origin.supportsRange = false;

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    EncryptedHlsPlaybackProxy proxy(store);
    ServerConfig server;
    server.id = QStringLiteral("norange-preview-webdav");
    server.name = QStringLiteral("No-Range Preview WebDAV");
    server.baseUrl = origin.containerUrl().adjusted(QUrl::RemoveFilename).toString();
    server.serviceType = ServiceType::WebDAV;

    std::optional<EncryptedHlsIdentifierPreviewResult> resolved;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    proxy.resolveIdentifierPreview(server, {}, origin.containerUrl(), QStringLiteral("7|now"),
                                   [&](EncryptedHlsIdentifierPreviewResult result) {
        resolved.emplace(std::move(result));
        loop.quit();
    });
    timer.start(5000);
    loop.exec();

    QVERIFY(resolved.has_value());
    QVERIFY(!resolved->has_value());
    QVERIFY(resolved->error().contains(QStringLiteral("range"), Qt::CaseInsensitive));
}

void EncryptedHlsPlaybackProxyTest::previewRetriesTransientFailuresAndCachesNothingOnFailure()
{
    // Regression: sporadic server throttling used to blank a row for 30 s
    // (failure negative-cache, no retry), which users saw as metadata that
    // "randomly doesn't show" while the file still played fine.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeWebDavServer origin;
    QVERIFY(origin.listen());
    origin.failFirstRequests = 3; // probe + fallback read + first retry probe

    TsslStore store(temporary.filePath(QStringLiteral("store")));
    EncryptedHlsPlaybackProxy proxy(store);
    ServerConfig server;
    server.id = QStringLiteral("flaky-preview-webdav");
    server.name = QStringLiteral("Flaky Preview WebDAV");
    server.baseUrl = origin.manifestUrl().adjusted(QUrl::RemoveFilename).toString();
    server.serviceType = ServiceType::WebDAV;

    std::optional<EncryptedHlsIdentifierPreviewResult> resolved;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    proxy.resolveIdentifierPreview(server, {}, origin.manifestUrl(), QStringLiteral("9|now"),
                                   [&](EncryptedHlsIdentifierPreviewResult result) {
        resolved.emplace(std::move(result));
        loop.quit();
    });
    timer.start(8000);
    loop.exec();

    QVERIFY(resolved.has_value());
    if (!resolved->has_value()) {
        QFAIL(qPrintable(resolved->error()));
    }
    QCOMPARE((**resolved).identifier, QStringLiteral("AAAAAAAAAAAAAAAA...AAAAAAAAAAAA"));

    // The recovered success is cached: a repeat listing read costs no request.
    const auto requestsAfterFirst = origin.requestCount;
    std::optional<EncryptedHlsIdentifierPreviewResult> cached;
    QEventLoop cachedLoop;
    QTimer cachedTimer;
    cachedTimer.setSingleShot(true);
    connect(&cachedTimer, &QTimer::timeout, &cachedLoop, &QEventLoop::quit);
    proxy.resolveIdentifierPreview(server, {}, origin.manifestUrl(), QStringLiteral("9|now"),
                                   [&](EncryptedHlsIdentifierPreviewResult result) {
        cached.emplace(std::move(result));
        cachedLoop.quit();
    });
    cachedTimer.start(1000);
    cachedLoop.exec();
    QVERIFY(cached.has_value());
    QVERIFY(cached->has_value());
    QCOMPARE(origin.requestCount, requestsAfterFirst);
}

QTEST_GUILESS_MAIN(EncryptedHlsPlaybackProxyTest)

#include "EncryptedHlsPlaybackProxyTest.moc"
