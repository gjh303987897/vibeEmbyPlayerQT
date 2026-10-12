#include "app/ThemeSnapshotController.h"
#include "app/FileDialogController.h"
#include "player/MpvVideoItem.h"
#include "NativeFileDialogTestHelpers.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QJSValue>
#include <QImage>
#include <QBuffer>
#include <QQuickWindow>
#include <QQuickItem>
#include <QSignalSpy>
#include <QPointer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QtTest>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class QmlUtilityTest final : public QObject {
    Q_OBJECT
private:
    QUrl source(const QString& name) { return QUrl::fromLocalFile(QStringLiteral(VIBEPLAYER_SOURCE_DIR) + "/qml/" + name); }
private slots:
    void nativePickerSurvivesVideoPlayback()
    {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires the native Windows platform");
        QTest::failOnWarning(QRegularExpression(".*Thread failed to finish.*"));
        const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (ffmpeg.isEmpty()) QSKIP("Requires FFmpeg to generate a local video fixture");
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto video = temporary.filePath("native-picker.mp4");
        QProcess generator;
        generator.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25",
            "-t", "3", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-y", video});
        QVERIFY(generator.waitForFinished(15000));
        QCOMPARE(generator.exitCode(), 0);
        QQuickWindow window;
        window.resize(640, 360);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        FileDialogController picker;
        picker.attachWindow(&window);
        const auto checkPicker = [&]() {
            int completions = 0;
            const auto title = QStringLiteral("Automatic native picker check (closes shortly)");
            picker.openFile(title, temporary.path(), "Videos (*.mp4)",
                [&](const QString&) { ++completions; });
            QTRY_VERIFY(nativeFileDialogVisible(title));
            QTest::qWait(800);
            const auto remainedOpen = picker.isOpen();
            picker.cancel();
            QTRY_COMPARE(completions, 1);
            QVERIFY2(remainedOpen, "Native picker rejected itself before cancellation");
        };
        checkPicker();
        MpvVideoItem item;
        item.setSize(QSizeF(640, 360));
        item.setParentItem(window.contentItem());
        item.setSource(QUrl::fromLocalFile(video));
        QTRY_VERIFY(item.position() > 0.1);
        item.stop();
        checkPicker();
        QTest::qWait(1200); // Exercise both native helpers' deferred cleanup.
#else
        QSKIP("Windows native picker integration");
#endif
    }
    void nativeHostMovesToTheNewWindow()
    {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires the native Windows platform");
        const auto host = [](QQuickWindow& window) {
            HWND result = nullptr;
            EnumChildWindows(reinterpret_cast<HWND>(window.winId()), [](HWND child, LPARAM data) -> BOOL {
                wchar_t name[128] {};
                GetClassNameW(child, name, 128);
                if (QString::fromWCharArray(name) == "VibePlayerMpvHostWindow") {
                    *reinterpret_cast<HWND*>(data) = child;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&result));
            return result;
        };
        QQuickWindow first, second;
        first.resize(200,200); second.resize(200,200);
        first.show(); second.show();
        MpvVideoItem item;
        item.setSize(QSizeF(100,100));
        item.setParentItem(first.contentItem());
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        item.setSource(QUrl::fromLocalFile(temporary.filePath("nonexistent.mp4")));
        QTRY_VERIFY(host(first));
        const auto original = host(first);
        // Exercise recovery when an existing native host has lost its parent.
        SetLastError(ERROR_SUCCESS);
        QVERIFY(SetParent(original, nullptr) || GetLastError() == ERROR_SUCCESS);
        item.play();
        QTRY_COMPARE(GetParent(original), reinterpret_cast<HWND>(first.winId()));
        item.setParentItem(second.contentItem());
        QTRY_VERIFY(host(second));
        const auto moved = host(second);
        QCOMPARE(GetParent(moved), reinterpret_cast<HWND>(second.winId()));
        // Qt can detach the item before attaching it, destroying and recreating
        // the native host. Both paths must release ownership by the old window.
        if (moved != original) QVERIFY(!IsWindow(original));
        QVERIFY(!host(first));
        item.stop();
#else
        QSKIP("Windows HWND test; other native platforms require their own runtime acceptance");
#endif
    }
    void debouncePreservesClosuresAndDestroysTimer()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, source("BindingOptimizer.qml"));
        QScopedPointer<QObject> optimizer(component.create());
        QVERIFY2(optimizer, qPrintable(component.errorString()));
        engine.globalObject().setProperty("optimizer", engine.newQObject(optimizer.data()));
        auto result = engine.evaluate("var captured = 41; var fired = 0; optimizer.debounce(function() { fired = captured + 1; }, 10)");
        QVERIFY(!result.isError());
        QPointer<QObject> timer = result.toQObject();
        QVERIFY(timer);
        QTRY_COMPARE(engine.globalObject().property("fired").toInt(), 42);
        QTRY_VERIFY(timer.isNull());
    }
    void workerDropsStaleRepliesAndRejectsExecutableFilters()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, source("DataProcessManager.qml"));
        QScopedPointer<QObject> manager(component.create());
        QVERIFY2(manager, qPrintable(component.errorString()));
        engine.globalObject().setProperty("manager", engine.newQObject(manager.data()));
        const auto result = engine.evaluate("var names = []; manager.filterCompleted.connect(function(items) { names.push(items[0].name); }); "
            "manager.filter([{name:'old',type:'video'}], 'type', 'video'); "
            "manager.filter([{name:'new',type:'video'}], 'type', 'video');");
        QVERIFY(!result.isError());
        QTRY_COMPARE(engine.globalObject().property("names").property("length").toInt(), 1);
        QCOMPARE(engine.globalObject().property("names").property(0).toString(), QString("new"));
        QSignalSpy errors(manager.data(), SIGNAL(requestFailed(QString,QString)));
        QVERIFY(!engine.evaluate("manager.filter([{name:'new'}], 'custom', '(function() { return true; })')").isError());
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(errors.front().at(1).toString().contains("unsupported"));
    }
    void cachedImagesOutlivePreloadersAndAreEvicted()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, source("ImageCacheManager.qml"));
        QScopedPointer<QObject> cache(component.create());
        QVERIFY2(cache, qPrintable(component.errorString()));
        QImage pixel(2, 2, QImage::Format_RGB32); pixel.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(pixel.save(&buffer,"PNG"));
        const auto url = "data:image/png;base64," + QString::fromLatin1(png.toBase64());
        QQmlComponent imageComponent(&engine);
        imageComponent.setData("import QtQuick; Image {}", QUrl());
        QScopedPointer<QObject> image(imageComponent.create());
        QVERIFY(image);
        image->setProperty("source", QUrl(url));
        QTRY_COMPARE(image->property("status").toInt(), 1);
        QVERIFY(QMetaObject::invokeMethod(cache.data(), "addToCache", Q_ARG(QVariant,url), Q_ARG(QVariant,QVariant::fromValue(image.data()))));
        const auto entries = cache->property("imageCache").value<QJSValue>();
        QPointer<QObject> retained = entries.property(url).property("image").toQObject();
        QVERIFY(retained);
        QVERIFY(retained != image.data());
        image.reset();
        QVERIFY(retained);
        QVERIFY(QMetaObject::invokeMethod(cache.data(), "clearCache"));
        QTRY_VERIFY(retained.isNull());
        QVERIFY(cache->property("currentCacheSizeBytes").toInt() == 0);
    }
    void snapshotsPublishEncodedImagesAsynchronously()
    {
        QQuickWindow window;
        window.resize(300,200); window.setColor(Qt::red); window.show();
        QTest::qWait(100);
        ThemeSnapshotController controller;
        QSignalSpy ready(&controller, &ThemeSnapshotController::ready);
        QSignalSpy failed(&controller, &ThemeSnapshotController::failed);
        const auto token = controller.grabContent(window.contentItem());
        QVERIFY(token > 0);
        QVERIFY(ready.isEmpty());
        QTRY_VERIFY(!ready.isEmpty() || !failed.isEmpty());
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.front().at(1).toString()));
        QCOMPARE(ready.front().at(0).toInt(), token);
        const auto path = ready.front().at(1).toUrl().toLocalFile();
        const QImage image(path);
        QVERIFY(!image.isNull());
        QVERIFY(image.pixelColor(0,0).red() > 200);
    }
};

QTEST_MAIN(QmlUtilityTest)
#include "QmlUtilityTest.moc"
