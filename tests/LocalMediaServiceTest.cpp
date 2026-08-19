#include "services/local/LocalMediaService.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

class LocalMediaServiceTest final : public QObject {
    Q_OBJECT

private slots:
    void listsFoldersAndSupportedVideosOnly();
    void resolvesDroppedVideoFile();
    void rejectsMissingDirectory();
    void rejectsDirectoryOutsideRoot();
    void invokesAsyncCallback();
    void invokesScopedAsyncCallback();
};

void LocalMediaServiceTest::listsFoldersAndSupportedVideosOnly()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    QVERIFY(QDir(temporaryDirectory.path()).mkdir(QStringLiteral("Season 1")));

    const auto createFile = [&temporaryDirectory](const QString& name) {
        QFile file(QDir(temporaryDirectory.path()).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("test"), 4);
    };
    createFile(QStringLiteral("Movie.MKV"));
    createFile(QStringLiteral("Trailer.mp4"));
    createFile(QStringLiteral("index.m3u8s"));
    createFile(QStringLiteral("segment_000000.ts"));
    createFile(QStringLiteral("Independent.ts"));
    createFile(QStringLiteral("notes.txt"));
    createFile(QStringLiteral("soundtrack.mp3"));

    const auto result = LocalMediaService::browseDirectory(temporaryDirectory.path());
    QVERIFY(result.has_value());
    QCOMPARE(result->size(), size_t { 5 });
    QVERIFY(result->at(0).directory);
    QCOMPARE(result->at(0).name, QStringLiteral("Season 1"));
    QVERIFY(std::ranges::any_of(*result, [](const LocalMediaItem& item) {
        return item.name == QStringLiteral("Movie.MKV");
    }));
    QVERIFY(std::ranges::any_of(*result, [](const LocalMediaItem& item) {
        return item.name == QStringLiteral("Trailer.mp4");
    }));
    QVERIFY(std::ranges::any_of(*result, [](const LocalMediaItem& item) {
        return item.name == QStringLiteral("index.m3u8s");
    }));
    QVERIFY(std::ranges::any_of(*result, [](const LocalMediaItem& item) {
        return item.name == QStringLiteral("Independent.ts");
    }));
    QVERIFY(std::ranges::none_of(*result, [](const LocalMediaItem& item) {
        return item.name == QStringLiteral("segment_000000.ts");
    }));
    QVERIFY(LocalMediaService::isEncryptedHlsManifest(QStringLiteral("INDEX.M3U8S")));
    QVERIFY(LocalMediaService::isSupportedVideoFile(QStringLiteral("VIDEO.WEBM")));
    QVERIFY(!LocalMediaService::isSupportedVideoFile(QStringLiteral("cover.jpg")));
}

void LocalMediaServiceTest::resolvesDroppedVideoFile()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    const auto videoPath = QDir(temporaryDirectory.path()).filePath(QStringLiteral("Dropped Video.MP4"));
    QFile videoFile(videoPath);
    QVERIFY(videoFile.open(QIODevice::WriteOnly));
    QCOMPARE(videoFile.write("test"), 4);
    videoFile.close();

    const auto videoResult = LocalMediaService::resolveVideoFile(QUrl::fromLocalFile(videoPath));
    QVERIFY(videoResult.has_value());
    QCOMPARE(*videoResult, QFileInfo(videoPath).canonicalFilePath());

    const auto textPath = QDir(temporaryDirectory.path()).filePath(QStringLiteral("notes.txt"));
    QFile textFile(textPath);
    QVERIFY(textFile.open(QIODevice::WriteOnly));
    QCOMPARE(textFile.write("test"), 4);
    textFile.close();

    QVERIFY(!LocalMediaService::resolveVideoFile(QUrl::fromLocalFile(textPath)).has_value());
    QVERIFY(!LocalMediaService::resolveVideoFile(QUrl(QStringLiteral("https://example.com/video.mp4"))).has_value());

    const auto manifestPath = QDir(temporaryDirectory.path()).filePath(QStringLiteral("index.m3u8s"));
    QFile manifestFile(manifestPath);
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    QCOMPARE(manifestFile.write("#EXTM3U\n"), 8);
    manifestFile.close();
    const auto manifestResult = LocalMediaService::resolveVideoFile(QUrl::fromLocalFile(manifestPath));
    QVERIFY(manifestResult.has_value());
    QCOMPARE(*manifestResult, QFileInfo(manifestPath).canonicalFilePath());
}

void LocalMediaServiceTest::rejectsMissingDirectory()
{
    const auto result = LocalMediaService::browseDirectory(QStringLiteral("this-path-does-not-exist"));
    QVERIFY(!result.has_value());
    QVERIFY(!result.error().isEmpty());
}

void LocalMediaServiceTest::rejectsDirectoryOutsideRoot()
{
    QTemporaryDir rootDirectory;
    QTemporaryDir outsideDirectory;
    QVERIFY(rootDirectory.isValid());
    QVERIFY(outsideDirectory.isValid());

    const auto result = LocalMediaService::browseDirectoryWithinRoot(
        rootDirectory.path(), outsideDirectory.path());
    QVERIFY(!result.has_value());
    QVERIFY(!result.error().isEmpty());

    const auto rootResult = LocalMediaService::browseDirectoryWithinRoot(
        rootDirectory.path(), rootDirectory.path());
    QVERIFY(rootResult.has_value());
    QCOMPARE(rootResult->path, QFileInfo(rootDirectory.path()).canonicalFilePath());
}

void LocalMediaServiceTest::invokesAsyncCallback()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    LocalMediaService service;
    bool completed = false;
    service.browseDirectoryAsync(temporaryDirectory.path(), [&completed](LocalMediaService::BrowseResult result) {
        QVERIFY(result.has_value());
        completed = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 3000);
}

void LocalMediaServiceTest::invokesScopedAsyncCallback()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    LocalMediaService service;
    bool completed = false;
    service.browseDirectoryWithinRootAsync(
        temporaryDirectory.path(),
        temporaryDirectory.path(),
        [&completed](LocalMediaService::DirectoryListingResult result) {
            QVERIFY(result.has_value());
            completed = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 3000);
}

QTEST_MAIN(LocalMediaServiceTest)
#include "LocalMediaServiceTest.moc"
