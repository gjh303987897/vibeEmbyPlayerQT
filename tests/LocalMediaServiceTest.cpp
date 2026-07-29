#include "services/local/LocalMediaService.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class LocalMediaServiceTest final : public QObject {
    Q_OBJECT

private slots:
    void listsFoldersAndSupportedVideosOnly();
    void rejectsMissingDirectory();
    void invokesAsyncCallback();
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
    createFile(QStringLiteral("notes.txt"));
    createFile(QStringLiteral("soundtrack.mp3"));

    const auto result = LocalMediaService::browseDirectory(temporaryDirectory.path());
    QVERIFY(result.has_value());
    QCOMPARE(result->size(), size_t { 3 });
    QVERIFY(result->at(0).directory);
    QCOMPARE(result->at(0).name, QStringLiteral("Season 1"));
    QCOMPARE(result->at(1).name, QStringLiteral("Movie.MKV"));
    QCOMPARE(result->at(2).name, QStringLiteral("Trailer.mp4"));
    QVERIFY(LocalMediaService::isSupportedVideoFile(QStringLiteral("VIDEO.WEBM")));
    QVERIFY(!LocalMediaService::isSupportedVideoFile(QStringLiteral("cover.jpg")));
}

void LocalMediaServiceTest::rejectsMissingDirectory()
{
    const auto result = LocalMediaService::browseDirectory(QStringLiteral("this-path-does-not-exist"));
    QVERIFY(!result.has_value());
    QVERIFY(!result.error().isEmpty());
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

QTEST_MAIN(LocalMediaServiceTest)
#include "LocalMediaServiceTest.moc"
