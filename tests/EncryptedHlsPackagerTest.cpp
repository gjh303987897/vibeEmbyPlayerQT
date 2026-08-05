#include "services/encryptedhls/EncryptedHlsPackager.h"
#include "services/webdav/AesGcmDecryptor.h"
#include "services/webdav/HlsManifestValidator.h"

#include <QFile>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <optional>

namespace {
bool writeBytes(const QString& path, QByteArrayView bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes.data(), bytes.size()) == bytes.size();
}

QByteArray tinyY4mVideo()
{
    QByteArray video = "YUV4MPEG2 W16 H16 F1:1 Ip A1:1 C420jpeg\n";
    for (int frame = 0; frame < 3; ++frame) {
        video.append("FRAME\n");
        video.append(QByteArray(16 * 16, static_cast<char>(48 + frame * 32)));
        video.append(QByteArray(8 * 8, static_cast<char>(128)));
        video.append(QByteArray(8 * 8, static_cast<char>(128)));
    }
    return video;
}
}

class EncryptedHlsPackagerTest final : public QObject {
    Q_OBJECT

private slots:
    void encryptsEverySegmentAndCreatesMatchingMetadata();
    void honorsCancellationBeforeEncryptingPlaintext();
    void packagesNormalVideoThroughFfmpeg();
};

void EncryptedHlsPackagerTest::encryptsEverySegmentAndCreatesMatchingMetadata()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QByteArray firstPlaintext = "first MPEG-TS payload";
    const QByteArray secondPlaintext = "second MPEG-TS payload with different bytes";
    const QByteArray manifest =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXTINF:6.0,\n"
        "segment_000000.ts\n"
        "#EXTINF:2.0,\n"
        "segment_000001.ts\n"
        "#EXT-X-ENDLIST\n";
    QVERIFY(writeBytes(temporary.filePath(QStringLiteral("index.m3u8")), manifest));
    QVERIFY(writeBytes(temporary.filePath(QStringLiteral("segment_000000.ts")), firstPlaintext));
    QVERIFY(writeBytes(temporary.filePath(QStringLiteral("segment_000001.ts")), secondPlaintext));

    std::atomic_bool canceled { false };
    double progress = 0.0;
    const auto prepared = EncryptedHlsPackaging::encryptHlsDirectory(
        temporary.path(),
        QStringLiteral("Private Sample.final.mkv"),
        canceled,
        [&progress](double value) { progress = value; });
    if (!prepared) {
        QFAIL(qPrintable(prepared.error()));
    }

    QCOMPARE(prepared->segmentCount, 2);
    QCOMPARE(prepared->manifestFileName, QStringLiteral("index.m3u8s"));
    QCOMPARE(prepared->tsslPackage.version, 3);
    QCOMPARE(prepared->tsslPackage.identifier.size(), TsslPackage::identifierLength);
    QCOMPARE(prepared->tsslPackage.segmentKeys.size(), 2);
    QVERIFY(prepared->tsslPackage.segmentKeys.value(QStringLiteral("segment_000000.ts")) !=
            prepared->tsslPackage.segmentKeys.value(QStringLiteral("segment_000001.ts")));
    QCOMPARE(progress, 1.0);
    QVERIFY(!QFile::exists(temporary.filePath(QStringLiteral("index.m3u8"))));

    QFile encryptedManifest(temporary.filePath(prepared->manifestFileName));
    QVERIFY(encryptedManifest.open(QIODevice::ReadOnly));
    const auto manifestBytes = encryptedManifest.readAll();
    const auto identifier = HlsManifestValidator::extractM3u8sIdentifier(manifestBytes);
    QVERIFY(identifier.has_value());
    QCOMPARE(*identifier, prepared->tsslPackage.identifier);
    QVERIFY(!manifestBytes.contains(QByteArrayLiteral("Private Sample.final.mkv")));
    const auto encryptedSourceName = HlsManifestValidator::extractEncryptedSourceFileName(manifestBytes);
    QVERIFY(encryptedSourceName.has_value());
    QCOMPARE(*encryptedSourceName, prepared->tsslPackage.encryptedSourceFileName);
    const auto recoveredSourceName = prepared->tsslPackage.decryptedSourceFileName();
    QVERIFY(recoveredSourceName.has_value());
    QVERIFY(recoveredSourceName->has_value());
    QCOMPARE(**recoveredSourceName, QStringLiteral("Private Sample.final.mkv"));
    QCOMPARE(prepared->tsslPackage.rootManifestDigest.size(), 32);

    const std::array originals { firstPlaintext, secondPlaintext };
    for (int index = 0; index < 2; ++index) {
        const auto fileName = QStringLiteral("segment_%1.ts").arg(index, 6, 10, QLatin1Char('0'));
        QFile encryptedFile(temporary.filePath(fileName));
        QVERIFY(encryptedFile.open(QIODevice::ReadOnly));
        auto encryptedBytes = encryptedFile.readAll();
        QCOMPARE(encryptedBytes.size(), originals.at(static_cast<size_t>(index)).size() + 32);
        const auto decrypted = AesGcmDecryptor::decryptTsSegment(
            encryptedBytes,
            prepared->tsslPackage.segmentKeys.value(fileName));
        QVERIFY(decrypted.has_value());
        QCOMPARE(*decrypted, originals.at(static_cast<size_t>(index)));

        encryptedBytes.back() ^= 0x01;
        QVERIFY(!AesGcmDecryptor::decryptTsSegment(
                     encryptedBytes,
                     prepared->tsslPackage.segmentKeys.value(fileName))
                     .has_value());
    }

    const auto parsedTssl = TsslPackage::parse(prepared->tsslPackage.toJson());
    QVERIFY(parsedTssl.has_value());
    QCOMPARE(parsedTssl->identifier, *identifier);
}

void EncryptedHlsPackagerTest::honorsCancellationBeforeEncryptingPlaintext()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(writeBytes(temporary.filePath(QStringLiteral("index.m3u8")),
                       QByteArrayLiteral("#EXTM3U\n#EXTINF:1,\nsegment_000000.ts\n#EXT-X-ENDLIST\n")));
    const QByteArray plaintext = "unencrypted payload";
    const auto segmentPath = temporary.filePath(QStringLiteral("segment_000000.ts"));
    QVERIFY(writeBytes(segmentPath, plaintext));

    std::atomic_bool canceled { true };
    const auto result = EncryptedHlsPackaging::encryptHlsDirectory(
        temporary.path(), QStringLiteral("sample"), canceled);
    QVERIFY(!result.has_value());
    QFile segment(segmentPath);
    QVERIFY(segment.open(QIODevice::ReadOnly));
    QCOMPARE(segment.readAll(), plaintext);
}

void EncryptedHlsPackagerTest::packagesNormalVideoThroughFfmpeg()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto sourcePath = temporary.filePath(QStringLiteral("tiny-video.y4m"));
    QVERIFY(writeBytes(sourcePath, tinyY4mVideo()));

    TsslStore store(temporary.filePath(QStringLiteral("keys")));
    EncryptedHlsPackager packager(store);
    if (packager.ffmpegExecutable().isEmpty()) {
        QSKIP("FFmpeg is not available in this test environment");
    }

    std::optional<EncryptedHlsPackageResult> completed;
    QString failure;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(&packager, &EncryptedHlsPackager::completed, &loop,
            [&](const EncryptedHlsPackageResult& result) {
                completed = result;
                loop.quit();
            });
    connect(&packager, &EncryptedHlsPackager::failed, &loop, [&](const QString& error) {
        failure = error;
        loop.quit();
    });

    const auto started = packager.start(EncryptedHlsPackageRequest {
        .sourcePath = sourcePath,
        .outputDirectory = temporary.path(),
        .segmentDurationSeconds = 2,
    });
    if (!started) {
        QFAIL(qPrintable(started.error()));
    }
    timer.start(60'000);
    loop.exec();

    if (!failure.isEmpty()) {
        QFAIL(qPrintable(failure));
    }
    QVERIFY2(completed.has_value(), "Timed out waiting for the FFmpeg packaging pipeline");
    QVERIFY(!packager.isRunning());
    QCOMPARE(packager.progress(), 1.0);
    QVERIFY(QFileInfo::exists(completed->manifestPath));
    QCOMPARE(QFileInfo(completed->manifestPath).fileName(), QStringLiteral("index.m3u8s"));
    QVERIFY(!QFileInfo(completed->outputDirectory).fileName().contains(
        QStringLiteral("tiny-video"), Qt::CaseInsensitive));
    QCOMPARE(QDir(completed->outputDirectory).entryList(
                 QStringList { QStringLiteral("*.tssl") }, QDir::Files).size(),
             0);
    QVERIFY(completed->segmentCount > 0);
    QCOMPARE(completed->identifier.size(), TsslPackage::identifierLength);

    QFile manifestFile(completed->manifestPath);
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    const auto manifestIdentifier = HlsManifestValidator::extractM3u8sIdentifier(manifestFile.readAll());
    QVERIFY(manifestIdentifier.has_value());
    QCOMPARE(*manifestIdentifier, completed->identifier);

    const auto stored = store.packageForRootDigest(completed->rootManifestDigest);
    QVERIFY(stored.has_value());
    QVERIFY(stored->has_value());
    QCOMPARE((**stored).identifier, completed->identifier);
    QCOMPARE((**stored).version, 3);
    const auto recoveredSourceName = (**stored).decryptedSourceFileName();
    QVERIFY(recoveredSourceName.has_value());
    QVERIFY(recoveredSourceName->has_value());
    QCOMPARE(**recoveredSourceName, QFileInfo(sourcePath).fileName());
    QCOMPARE((**stored).rootManifestDigest, completed->rootManifestDigest);
    QCOMPARE((**stored).segmentKeys.size(), completed->segmentCount);
}

QTEST_GUILESS_MAIN(EncryptedHlsPackagerTest)

#include "EncryptedHlsPackagerTest.moc"
