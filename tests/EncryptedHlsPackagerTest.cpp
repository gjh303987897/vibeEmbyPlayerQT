#include "services/encryptedhls/EncryptedHlsBatchPackager.h"
#include "services/encryptedhls/EncryptedHlsPackager.h"
#include "services/encryptedhls/EncryptedHlsSourcePlanner.h"
#include "services/encryptedhls/EncryptedHlsTarContainer.h"
#include "services/webdav/AesGcmDecryptor.h"
#include "services/webdav/HlsManifestValidator.h"

#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <optional>
#include <utility>

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

bool containsArgumentPair(const QStringList& arguments, const QString& option, const QString& value)
{
    const auto index = arguments.indexOf(option);
    return index >= 0 && index + 1 < arguments.size() && arguments.at(index + 1) == value;
}
}

class EncryptedHlsPackagerTest final : public QObject {
    Q_OBJECT

private slots:
    void encryptsEverySegmentAndCreatesMatchingMetadata();
    void honorsCancellationBeforeEncryptingPlaintext();
    void buildsFfmpegArgumentsForSelectedEncodings();
    void packagesNormalVideoThroughFfmpeg();
    void packagesLegacyDirectoryFormatThroughFfmpeg();
    void packagesBatchAndContinuesAfterFailure();
    void cancelsBatchBeforeStartingRemainingItems();
    void plansFilesAndNestedFoldersWithoutFlattening();
    void deduplicatesSourcesCoveredByASelectedFolder();
    void rejectsUnsupportedFilesAndHonorsDiscoveryCancellation();
};

void EncryptedHlsPackagerTest::plansFilesAndNestedFoldersWithoutFlattening()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("sources/FolderB/season-01/extras")));
    QVERIFY(root.mkpath(QStringLiteral("output")));

    const auto videoA = root.filePath(QStringLiteral("sources/videoA.mp4"));
    const auto folderB = root.filePath(QStringLiteral("sources/FolderB"));
    const auto episode = root.filePath(QStringLiteral("sources/FolderB/season-01/episode.mkv"));
    const auto extra = root.filePath(QStringLiteral("sources/FolderB/season-01/extras/clip.mov"));
    QVERIFY(writeBytes(videoA, QByteArrayLiteral("video-a")));
    QVERIFY(writeBytes(episode, QByteArrayLiteral("episode")));
    QVERIFY(writeBytes(extra, QByteArrayLiteral("extra")));
    QVERIFY(writeBytes(root.filePath(QStringLiteral("sources/FolderB/readme.txt")),
                       QByteArrayLiteral("ignored")));

    const auto outputRoot = root.filePath(QStringLiteral("output"));
    std::atomic_bool canceled { false };
    const auto planned = EncryptedHlsSourcePlanner::plan(
        { videoA, folderB }, outputRoot, canceled);
    if (!planned) {
        QFAIL(qPrintable(planned.error()));
    }
    QCOMPARE(planned->sources.size(), 3);

    const auto outputFor = [&planned](const QString& sourcePath) {
        const auto match = std::ranges::find_if(planned->sources, [&sourcePath](const auto& source) {
            return QFileInfo(source.sourcePath) == QFileInfo(sourcePath);
        });
        return match == planned->sources.end() ? QString() : match->outputDirectory;
    };
    QCOMPARE(QDir::cleanPath(outputFor(videoA)), QDir::cleanPath(outputRoot));
    QCOMPARE(QDir::cleanPath(outputFor(episode)),
             QDir(outputRoot).filePath(QStringLiteral("FolderB/season-01")));
    QCOMPARE(QDir::cleanPath(outputFor(extra)),
             QDir(outputRoot).filePath(QStringLiteral("FolderB/season-01/extras")));
    QVERIFY(QDir(outputFor(episode)).exists());
    QVERIFY(QDir(outputFor(extra)).exists());
}

void EncryptedHlsPackagerTest::deduplicatesSourcesCoveredByASelectedFolder()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("FolderB/nested")));
    QVERIFY(root.mkpath(QStringLiteral("output")));
    const auto folder = root.filePath(QStringLiteral("FolderB"));
    const auto video = root.filePath(QStringLiteral("FolderB/nested/video.webm"));
    QVERIFY(writeBytes(video, QByteArrayLiteral("video")));

    std::atomic_bool canceled { false };
    const auto planned = EncryptedHlsSourcePlanner::plan(
        { folder, video, folder }, root.filePath(QStringLiteral("output")), canceled);
    if (!planned) {
        QFAIL(qPrintable(planned.error()));
    }
    QCOMPARE(planned->sources.size(), 1);
    QCOMPARE(QDir::cleanPath(planned->sources.constFirst().outputDirectory),
             root.filePath(QStringLiteral("output/FolderB/nested")));
}

void EncryptedHlsPackagerTest::rejectsUnsupportedFilesAndHonorsDiscoveryCancellation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("output")));
    const auto textFile = root.filePath(QStringLiteral("notes.txt"));
    QVERIFY(writeBytes(textFile, QByteArrayLiteral("not a video")));

    std::atomic_bool active { false };
    const auto unsupported = EncryptedHlsSourcePlanner::plan(
        { textFile }, root.filePath(QStringLiteral("output")), active);
    QVERIFY(!unsupported.has_value());
    QVERIFY(unsupported.error().contains(QStringLiteral("not a supported video")));

    std::atomic_bool canceled { true };
    const auto canceledPlan = EncryptedHlsSourcePlanner::plan(
        { temporary.path() }, root.filePath(QStringLiteral("output")), canceled);
    QVERIFY(!canceledPlan.has_value());
    QVERIFY(canceledPlan.error().contains(QStringLiteral("canceled")));
}

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

void EncryptedHlsPackagerTest::buildsFfmpegArgumentsForSelectedEncodings()
{
    const auto build = [](EncryptedHlsVideoEncoding video,
                          EncryptedHlsAudioEncoding audio,
                          EncryptedHlsVideoQuality quality = EncryptedHlsVideoQuality::Balanced) {
        return EncryptedHlsPackaging::buildFfmpegArguments(
            EncryptedHlsPackageRequest {
                .sourcePath = QStringLiteral("input.mkv"),
                .outputDirectory = QStringLiteral("output"),
                .segmentDurationSeconds = 6,
                .videoEncoding = video,
                .audioEncoding = audio,
                .videoQuality = quality,
            },
            QStringLiteral("segment_%06d.ts"),
            QStringLiteral("index.m3u8"));
    };

    const auto copied = build(EncryptedHlsVideoEncoding::Copy, EncryptedHlsAudioEncoding::Copy);
    QVERIFY(copied.has_value());
    QVERIFY(containsArgumentPair(*copied, QStringLiteral("-c:v"), QStringLiteral("copy")));
    QVERIFY(containsArgumentPair(*copied, QStringLiteral("-c:a"), QStringLiteral("copy")));
    QVERIFY(containsArgumentPair(*copied, QStringLiteral("-hls_flags"), QStringLiteral("temp_file")));
    QVERIFY(!copied->contains(QStringLiteral("-force_key_frames")));
    QVERIFY(!copied->contains(QStringLiteral("independent_segments+temp_file")));

    const auto h264 = build(EncryptedHlsVideoEncoding::H264,
                            EncryptedHlsAudioEncoding::Aac,
                            EncryptedHlsVideoQuality::High);
    QVERIFY(h264.has_value());
    QVERIFY(containsArgumentPair(*h264, QStringLiteral("-c:v"), QStringLiteral("libx264")));
    QVERIFY(containsArgumentPair(*h264, QStringLiteral("-crf"), QStringLiteral("18")));
    QVERIFY(containsArgumentPair(*h264, QStringLiteral("-c:a"), QStringLiteral("aac")));
    QVERIFY(h264->contains(QStringLiteral("-force_key_frames")));
    QVERIFY(h264->contains(QStringLiteral("independent_segments+temp_file")));
    QVERIFY(containsArgumentPair(*h264, QStringLiteral("-sc_threshold"), QStringLiteral("0")));
    QVERIFY(!h264->contains(QStringLiteral("-x265-params")));

    const auto h265 = build(EncryptedHlsVideoEncoding::H265,
                            EncryptedHlsAudioEncoding::Aac,
                            EncryptedHlsVideoQuality::Compact);
    QVERIFY(h265.has_value());
    QVERIFY(containsArgumentPair(*h265, QStringLiteral("-c:v"), QStringLiteral("libx265")));
    QVERIFY(containsArgumentPair(*h265, QStringLiteral("-crf"), QStringLiteral("28")));
    QVERIFY(containsArgumentPair(*h265, QStringLiteral("-x265-params"), QStringLiteral("scenecut=0")));
    QVERIFY(!h265->contains(QStringLiteral("-sc_threshold")));

    const auto invalidVideo = build(static_cast<EncryptedHlsVideoEncoding>(99),
                                    EncryptedHlsAudioEncoding::Aac);
    QVERIFY(!invalidVideo.has_value());
    const auto invalidQuality = build(EncryptedHlsVideoEncoding::H264,
                                      EncryptedHlsAudioEncoding::Aac,
                                      static_cast<EncryptedHlsVideoQuality>(99));
    QVERIFY(!invalidQuality.has_value());
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
    QVERIFY(completed->manifestPath.endsWith(QStringLiteral(".m3u8sp"), Qt::CaseInsensitive));
    QCOMPARE(completed->outputDirectory, completed->manifestPath);
    QVERIFY(!QFileInfo(completed->manifestPath).fileName().contains(
        QStringLiteral("tiny-video"), Qt::CaseInsensitive));
    QCOMPARE(QDir(QFileInfo(completed->manifestPath).absolutePath()).entryList(
                 QStringList { QStringLiteral("*.tssl") }, QDir::Files).size(),
             0);
    QVERIFY(completed->segmentCount > 0);
    QCOMPARE(completed->identifier.size(), TsslPackage::identifierLength);

    const auto archiveIndex = EncryptedHlsTarContainer::readIndex(completed->manifestPath);
    if (!archiveIndex) QFAIL(qPrintable(archiveIndex.error()));
    const auto manifest = EncryptedHlsTarContainer::readEntry(
        completed->manifestPath, *archiveIndex, archiveIndex->manifestPath, 4 * 1024 * 1024);
    if (!manifest) QFAIL(qPrintable(manifest.error()));
    const auto manifestIdentifier = HlsManifestValidator::extractM3u8sIdentifier(*manifest);
    QVERIFY(manifestIdentifier.has_value());
    QCOMPARE(*manifestIdentifier, completed->identifier);

    const auto stored = store.packageForRootDigest(completed->rootManifestDigest);
    QVERIFY(stored.has_value());
    QVERIFY(stored->has_value());
    QCOMPARE((**stored).identifier, completed->identifier);
    QCOMPARE((**stored).version, 4);
    QCOMPARE((**stored).containerIndexSha256, archiveIndex->sha256);
    QCOMPARE((**stored).containerLength, QFileInfo(completed->manifestPath).size());
    const auto recoveredSourceName = (**stored).decryptedSourceFileName();
    QVERIFY(recoveredSourceName.has_value());
    QVERIFY(recoveredSourceName->has_value());
    QCOMPARE(**recoveredSourceName, QFileInfo(sourcePath).fileName());
    QCOMPARE((**stored).rootManifestDigest, completed->rootManifestDigest);
    QCOMPARE((**stored).segmentKeys.size(), completed->segmentCount);
}

void EncryptedHlsPackagerTest::packagesLegacyDirectoryFormatThroughFfmpeg()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto sourcePath = temporary.filePath(QStringLiteral("legacy-video.y4m"));
    QVERIFY(writeBytes(sourcePath, tinyY4mVideo()));

    TsslStore store(temporary.filePath(QStringLiteral("legacy-keys")));
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
            [&](const EncryptedHlsPackageResult& result) { completed = result; loop.quit(); });
    connect(&packager, &EncryptedHlsPackager::failed, &loop,
            [&](const QString& error) { failure = error; loop.quit(); });

    const auto started = packager.start(EncryptedHlsPackageRequest {
        .sourcePath = sourcePath,
        .outputDirectory = temporary.path(),
        .segmentDurationSeconds = 2,
        .containerFormat = EncryptedHlsContainerFormat::DirectoryM3u8s,
    });
    if (!started) QFAIL(qPrintable(started.error()));
    timer.start(60'000);
    loop.exec();

    if (!failure.isEmpty()) QFAIL(qPrintable(failure));
    QVERIFY2(completed.has_value(), "Timed out waiting for legacy M3U8S packaging");
    QVERIFY(QFileInfo(completed->outputDirectory).isDir());
    QCOMPARE(QFileInfo(completed->manifestPath).fileName(), QStringLiteral("index.m3u8s"));
    QVERIFY(QFileInfo(completed->manifestPath).isFile());
    QVERIFY(!QFileInfo::exists(completed->outputDirectory + QStringLiteral(".m3u8sp")));
    const auto stored = store.packageForRootDigest(completed->rootManifestDigest);
    QVERIFY(stored.has_value());
    QVERIFY(stored->has_value());
    QCOMPARE((**stored).version, 3);
    QCOMPARE((**stored).containerLength, qint64(0));
}

void EncryptedHlsPackagerTest::packagesBatchAndContinuesAfterFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto firstSourcePath = temporary.filePath(QStringLiteral("first-video.y4m"));
    const auto secondSourcePath = temporary.filePath(QStringLiteral("second-video.y4m"));
    const auto missingSourcePath = temporary.filePath(QStringLiteral("missing-video.y4m"));
    QVERIFY(writeBytes(firstSourcePath, tinyY4mVideo()));
    QVERIFY(writeBytes(secondSourcePath, tinyY4mVideo()));

    TsslStore store(temporary.filePath(QStringLiteral("batch-keys")));
    EncryptedHlsBatchPackager packager(store);
    if (packager.ffmpegExecutable().isEmpty()) {
        QSKIP("FFmpeg is not available in this test environment");
    }

    std::optional<EncryptedHlsBatchResult> completed;
    int completedItems = 0;
    int failedItems = 0;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(&packager, &EncryptedHlsBatchPackager::itemCompleted, &loop,
            [&completedItems](const EncryptedHlsPackageResult&) { ++completedItems; });
    connect(&packager, &EncryptedHlsBatchPackager::itemFailed, &loop,
            [&failedItems](const EncryptedHlsBatchFailure&) { ++failedItems; });
    connect(&packager, &EncryptedHlsBatchPackager::completed, &loop,
            [&](const EncryptedHlsBatchResult& result) {
                completed = result;
                loop.quit();
            });

    EncryptedHlsBatchRequest batch;
    for (const auto& sourcePath : { firstSourcePath, missingSourcePath, secondSourcePath }) {
        batch.packages.append(EncryptedHlsPackageRequest {
            .sourcePath = sourcePath,
            .outputDirectory = temporary.path(),
            .segmentDurationSeconds = 2,
        });
    }
    const auto started = packager.start(std::move(batch));
    if (!started) {
        QFAIL(qPrintable(started.error()));
    }
    timer.start(120'000);
    loop.exec();

    QVERIFY2(completed.has_value(), "Timed out waiting for the M3U8S batch pipeline");
    QVERIFY(!packager.isRunning());
    QCOMPARE(packager.progress(), 1.0);
    QCOMPARE(packager.totalCount(), 3);
    QCOMPARE(packager.processedCount(), 3);
    QCOMPARE(completedItems, 2);
    QCOMPARE(failedItems, 1);
    QCOMPARE(completed->requestedCount, 3);
    QCOMPARE(completed->packages.size(), 2);
    QCOMPARE(completed->failures.size(), 1);
    QCOMPARE(completed->failures.constFirst().sourcePath, missingSourcePath);
    QVERIFY(completed->segmentCount > 0);
    QVERIFY(completed->packages.at(0).outputDirectory != completed->packages.at(1).outputDirectory);
    for (const auto& package : completed->packages) {
        QVERIFY(QFileInfo::exists(package.manifestPath));
    }

    const auto storedPackages = store.listPackages();
    QVERIFY(storedPackages.has_value());
    QCOMPARE(storedPackages->size(), 2);
}

void EncryptedHlsPackagerTest::cancelsBatchBeforeStartingRemainingItems()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    TsslStore store(temporary.filePath(QStringLiteral("cancel-keys")));
    EncryptedHlsBatchPackager packager(store);

    std::optional<EncryptedHlsBatchResult> canceled;
    bool completed = false;
    connect(&packager, &EncryptedHlsBatchPackager::itemFailed, &packager,
            [&packager](const EncryptedHlsBatchFailure&) { packager.cancel(); });
    connect(&packager, &EncryptedHlsBatchPackager::canceled, &packager,
            [&canceled](const EncryptedHlsBatchResult& result) { canceled = result; });
    connect(&packager, &EncryptedHlsBatchPackager::completed, &packager,
            [&completed](const EncryptedHlsBatchResult&) { completed = true; });

    EncryptedHlsBatchRequest batch;
    for (const auto& name : { QStringLiteral("missing-first.mp4"),
                              QStringLiteral("missing-second.mp4") }) {
        batch.packages.append(EncryptedHlsPackageRequest {
            .sourcePath = temporary.filePath(name),
            .outputDirectory = temporary.path(),
        });
    }
    const auto started = packager.start(std::move(batch));
    QVERIFY(started.has_value());

    QVERIFY(canceled.has_value());
    QVERIFY(!completed);
    QVERIFY(!packager.isRunning());
    QCOMPARE(packager.totalCount(), 2);
    QCOMPARE(packager.processedCount(), 1);
    QCOMPARE(canceled->failures.size(), 1);
    QCOMPARE(canceled->failures.constFirst().sourcePath,
             temporary.filePath(QStringLiteral("missing-first.mp4")));
}

QTEST_GUILESS_MAIN(EncryptedHlsPackagerTest)

#include "EncryptedHlsPackagerTest.moc"
