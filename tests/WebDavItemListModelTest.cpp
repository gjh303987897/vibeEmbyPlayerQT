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
}

class WebDavItemListModelTest final : public QObject {
    Q_OBJECT

private slots:
    void defaultModeShowsEveryItem();
    void videoModeShowsOnlyFoldersAndVideos();
    void newDirectoryContentsRespectCurrentMode();
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
    QCOMPARE(model.count(), 4);
    QCOMPARE(model.itemAt(2)->name, QStringLiteral("notes.txt"));
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

QTEST_MAIN(WebDavItemListModelTest)

#include "WebDavItemListModelTest.moc"
