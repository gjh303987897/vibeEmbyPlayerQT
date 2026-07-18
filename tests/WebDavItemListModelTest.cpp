#include "viewmodels/WebDavItemListModel.h"

#include <QSignalSpy>
#include <QTest>

#include <utility>
#include <vector>

namespace {
WebDavItem item(QString name, bool directory, bool playable)
{
    return WebDavItem {
        .name = std::move(name),
        .directory = directory,
        .playable = playable,
    };
}

WebDavItem audioItem(QString name)
{
    return WebDavItem {
        .name = std::move(name),
        .audioPlayable = true,
    };
}
}

class WebDavItemListModelTest final : public QObject {
    Q_OBJECT

private slots:
    void defaultModeShowsEveryItem();
    void videoModeShowsOnlyFoldersAndVideos();
    void newDirectoryContentsRespectCurrentMode();
    void audioModeShowsOnlyAudioFiles();
};

void WebDavItemListModelTest::defaultModeShowsEveryItem()
{
    WebDavItemListModel model;
    model.setItems({
        item(QStringLiteral("Movies"), true, false),
        item(QStringLiteral("film.mkv"), false, true),
        item(QStringLiteral("notes.txt"), false, false),
    });

    QCOMPARE(model.count(), 3);
    QVERIFY(!model.videoMode());
    QCOMPARE(model.itemAt(2)->name, QStringLiteral("notes.txt"));
}

void WebDavItemListModelTest::videoModeShowsOnlyFoldersAndVideos()
{
    WebDavItemListModel model;
    model.setItems({
        item(QStringLiteral("Movies"), true, false),
        item(QStringLiteral("film.mkv"), false, true),
        audioItem(QStringLiteral("soundtrack.flac")),
        item(QStringLiteral("notes.txt"), false, false),
        item(QStringLiteral("Artwork"), true, false),
    });
    QSignalSpy countSpy(&model, &WebDavItemListModel::countChanged);

    model.setVideoMode(true);

    QCOMPARE(countSpy.count(), 1);
    QCOMPARE(model.count(), 3);
    QVERIFY(model.videoMode());
    QCOMPARE(model.itemAt(0)->name, QStringLiteral("Movies"));
    QCOMPARE(model.itemAt(1)->name, QStringLiteral("film.mkv"));
    QCOMPARE(model.itemAt(2)->name, QStringLiteral("Artwork"));
    QVERIFY(!model.itemAt(3).has_value());

    model.setVideoMode(false);
    QCOMPARE(model.count(), 5);
    QCOMPARE(model.itemAt(2)->name, QStringLiteral("soundtrack.flac"));
}

void WebDavItemListModelTest::newDirectoryContentsRespectCurrentMode()
{
    WebDavItemListModel model;
    model.setVideoMode(true);
    model.setItems({
        item(QStringLiteral("readme.pdf"), false, false),
        item(QStringLiteral("Series"), true, false),
        item(QStringLiteral("episode.mp4"), false, true),
    });

    QCOMPARE(model.count(), 2);
    QCOMPARE(model.itemAt(0)->name, QStringLiteral("Series"));
    QCOMPARE(model.itemAt(1)->name, QStringLiteral("episode.mp4"));

    model.clear();
    QCOMPARE(model.count(), 0);
    QVERIFY(model.videoMode());
}

void WebDavItemListModelTest::audioModeShowsOnlyAudioFiles()
{
    WebDavItemListModel model;
    model.setItems({
        item(QStringLiteral("film.mkv"), false, true),
        audioItem(QStringLiteral("track.flac")),
        item(QStringLiteral("notes.txt"), false, false),
        audioItem(QStringLiteral("track-two.mp3")),
    });

    model.setDisplayMode(QStringLiteral("audio"));

    QCOMPARE(model.count(), 2);
    QVERIFY(model.audioMode());
    QCOMPARE(model.itemAt(0)->name, QStringLiteral("track.flac"));
    QCOMPARE(model.itemAt(1)->name, QStringLiteral("track-two.mp3"));

    model.setItems({
        audioItem(QStringLiteral("next-folder-track.ogg")),
        item(QStringLiteral("Series"), true, false),
        item(QStringLiteral("episode.mp4"), false, true),
    });
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.itemAt(0)->name, QStringLiteral("next-folder-track.ogg"));
}

QTEST_MAIN(WebDavItemListModelTest)

#include "WebDavItemListModelTest.moc"
