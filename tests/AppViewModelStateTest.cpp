#include "viewmodels/AppViewModel.h"
#include "app/TrayController.h"
#include "app/ThemeSnapshotController.h"
#include "app/WindowAppearanceController.h"
#include "player/MpvVideoItem.h"
#include "NativeFileDialogTestHelpers.h"
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QRegularExpression>
#include <QQmlExpression>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QProcess>
#include <QStandardPaths>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>

namespace {
ServiceCard card(const QString& id, bool privateMode = false)
{
    return {.server = {.id = id, .name = id, .baseUrl = "https://localhost/", .username = "tester",
                      .serviceType = ServiceType::WebDAV, .trustSelfSignedCertificate = true,
                      .autoLogin = false, .privateMode = privateMode}};
}
class DirectoryEndpoint final : public QTcpServer {
public:
    int lists {0};
    int uploads {0};
    bool allowUploads {false};
    DirectoryEndpoint()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    qint64 length = 0;
                    for (const auto& line : bytes.first(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    if (bytes.startsWith("PROPFIND")) {
                        ++lists;
                        const QByteArray body = "<d:multistatus xmlns:d=\"DAV:\"/>";
                        socket->write("HTTP/1.1 207 Multi-Status\r\nContent-Length: " + QByteArray::number(body.size()) +
                                      "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    } else if (bytes.startsWith("PUT")) {
                        auto* timer = new QTimer(socket);
                        connect(timer, &QTimer::timeout, socket, [this, socket, timer]() {
                            if (!allowUploads) return;
                            timer->stop();
                            ++uploads;
                            socket->write("HTTP/1.1 201 Created\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            socket->disconnectFromHost();
                        });
                        timer->start(5);
                    }
                });
            }
        });
    }
};
}

class AppViewModelStateTest final : public QObject {
    Q_OBJECT
    QTemporaryDir m_settingsDirectory;
    QSettings::Format m_originalFormat {QSettings::defaultFormat()};
private slots:
    void initTestCase()
    {
        QVERIFY(m_settingsDirectory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDirectory.path());
    }
    void cleanupTestCase() { QSettings::setDefaultFormat(m_originalFormat); }
    void mergedIptvPlaybackSelectsRoutesAndPersistsActualInformation()
    {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("multisource.db");
        SessionRepository repository("iptv_multisource_fixture", path);
        QVERIFY(repository.initialize());
        ServerConfig server {.id = "multi", .name = "Multiple routes", .baseUrl = "test.m3u",
            .username = "", .serviceType = ServiceType::IPTV};
        IptvPlaylist playlist {"p", "multi", "TV", "LocalFile", "test.m3u", "test.m3u", "2026-10-12"};
        const auto now = QDateTime::currentDateTimeUtc();
        IptvChannel hd {.id = "old-hd", .name = "Channel", .groupName = "News", .streamUrl = "https://test/hd"};
        hd.streamInfo = {{.url = hd.streamUrl, .available = true, .width = 1920, .height = 1080,
            .bitrate = 5'000'000, .checkedAt = now}};
        auto sd = hd;
        sd.id = "old-sd";
        sd.streamUrl = "https://test/sd";
        sd.streamInfo = {{.url = sd.streamUrl, .available = true, .width = 640, .height = 360,
            .bitrate = 1'000'000, .checkedAt = now}};
        QVERIFY(repository.saveIptvPlaylist(server, playlist, {hd, sd}));
        AppViewModel vm(nullptr, path);
        vm.backToServices();
        QTRY_COMPARE(vm.services()->count(), 1);
        vm.selectServiceCard(0);
        QTRY_COMPARE(vm.currentView(), QString("iptv"));
        QCOMPARE(vm.iptvChannels()->count(), 1);
        QCOMPARE(vm.iptvChannels()->channelAt(0)->streamUrls.size(), 2);
        vm.setIptvStreamPreference("dataSaver");
        vm.playIptvChannel(0);
        QCOMPARE(vm.currentPlaybackUrl(), QUrl("https://test/sd"));
        vm.reportIptvPlaybackInfo(vm.currentIptvChannelId(), "854x480", "h264", "1.2 Mbps");
        // Reset/stale notifications must not erase valid playback information.
        vm.reportIptvPlaybackInfo(vm.currentIptvChannelId(), "", "", "");
        vm.reportIptvPlaybackInfo("old-sd", "1x1", "wrong", "0");
        QCOMPARE(vm.iptvChannels()->channelAt(0)->width, 854);
        auto corrected = [&] {
            const auto loaded = repository.loadIptvChannels("multi");
            if (!loaded || loaded->empty()) return false;
            const auto& infos = loaded->front().streamInfo;
            const auto info = std::ranges::find(infos, QString("https://test/sd"), &IptvStreamInfo::url);
            return info != infos.end() && info->width == 854 && info->bitrate == 1'200'000
                && info->state == "playback";
        };
        QTRY_VERIFY_WITH_TIMEOUT(corrected(), 5000);
        vm.setIptvStreamPreference("quality");
        QCOMPARE(vm.currentPlaybackUrl(), QUrl("https://test/sd")); // No disruption mid-playback.
        vm.playIptvChannel(0);
        QCOMPARE(vm.currentPlaybackUrl(), QUrl("https://test/hd"));
        const auto streams = vm.iptvPlaybackStreams();
        QCOMPARE(streams.size(), 2);
        const auto hdId = streams[0].toMap().value("id").toString();
        const auto sdId = streams[1].toMap().value("id").toString();
        QVERIFY(!streams[0].toMap().contains("url"));
        QVERIFY(!streams[0].toMap().value("detail").toString().contains("https://"));
        vm.switchIptvPlaybackStream("invalid-menu-id");
        QCOMPARE(vm.currentPlaybackUrl(), QUrl("https://test/hd"));
        vm.reportPlaybackError("Unable to load external subtitle: missing file");
        QVERIFY(vm.hasError()); // Unrelated errors must not initiate failover.
        vm.clearError();
        vm.reportPlaybackError("Playback failed: HTTP 404");
        QVERIFY(!vm.hasError());
        vm.reportPlaybackEnded(0.0, false, true);
        QTRY_COMPARE(vm.currentPlaybackUrl(), QUrl("https://test/sd"));
        QVERIFY(!vm.hasError());
        vm.reportPlaybackEnded(1.0, false, false); // Stop/replacement is not failure.
        QTest::qWait(20);
        QCOMPARE(vm.currentPlaybackUrl(), QUrl("https://test/sd"));
        vm.reportPlaybackEnded(1.0, true, false); // Normal EOF is not failure.
        QCOMPARE(vm.currentPlaybackUrl(), QUrl("https://test/sd"));
        vm.reportPlaybackError("Playback failed: disconnected");
        vm.reportPlaybackEnded(0.0, false, true);
        QVERIFY(vm.hasError());
        const auto exhaustedUrl = vm.currentPlaybackUrl();
        QTest::qWait(50);
        QCOMPARE(vm.currentPlaybackUrl(), exhaustedUrl); // Never cycle endlessly.
        vm.switchIptvPlaybackStream(hdId); // Explicit retry starts a new attempt round.
        QCOMPARE(vm.currentPlaybackUrl(), QUrl("https://test/hd"));
        QVERIFY(!vm.hasError());
        vm.reportPlaybackEnded(0.0, false, true);
        vm.switchIptvPlaybackStream(sdId); // Supersede a queued automatic retry.
        QTest::qWait(50);
        QCOMPARE(vm.currentPlaybackUrl(), QUrl("https://test/sd"));
        vm.switchIptvPlaybackStream(hdId);
        auto* startupTimeout = vm.findChild<QTimer*>("iptvStartupTimeout");
        QVERIFY(startupTimeout);
        QVERIFY(startupTimeout->isActive());
        QVERIFY(QMetaObject::invokeMethod(startupTimeout, "timeout"));
        QTRY_COMPARE(vm.currentPlaybackUrl(), QUrl("https://test/sd"));
        vm.reportPlaybackStarted();
        QVERIFY(!startupTimeout->isActive());
        vm.switchIptvPlaybackStream(hdId);
        vm.reportPlaybackEnded(0.0, false, true);
        vm.closePlayerToDetails();
        QTest::qWait(20);
        QVERIFY(vm.currentPlaybackUrl().isEmpty());
        QVERIFY(vm.iptvPlaybackStreams().isEmpty());
    }
    void iptvImportKeepsEventLoopRunningAndPreservesANewEditor_data()
    {
        QTest::addColumn<bool>("replaceEditor");
        QTest::newRow("complete-import") << false;
        QTest::newRow("new-editor-during-import") << true;
    }
    void iptvImportKeepsEventLoopRunningAndPreservesANewEditor()
    {
        QFETCH(bool, replaceEditor);
        QTemporaryDir temporary;
        const auto databasePath = temporary.filePath("import.db");
        SessionRepository repository("iptv_import_fixture", databasePath);
        QVERIFY(repository.initialize());
        AppViewModel vm(nullptr, databasePath);
        QFile source(temporary.filePath("channels.m3u"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("#EXTM3U\n");
        for (int i = 0; i < 100; ++i)
            source.write(QString("#EXTINF:-1,Channel %1\nhttp://127.0.0.1:1/%1.ts\n").arg(i).toUtf8());
        source.close();
        vm.beginAddServiceCard();
        vm.setServiceType("IPTV");
        vm.setServerName("Imported channels");
        vm.setIptvFilePath(source.fileName());
        {
            auto locked = QSqlDatabase::addDatabase("QSQLITE", "iptv_import_lock");
            locked.setDatabaseName(databasePath);
            QVERIFY(locked.open());
            QSqlQuery query(locked);
            QVERIFY(query.exec("BEGIN IMMEDIATE"));
            vm.saveServiceCard();
            vm.saveServiceCard(); // A second click cannot start a competing import.
            QVERIFY(vm.loading());
            int heartbeats = 0;
            QTimer timer;
            connect(&timer, &QTimer::timeout, [&] { ++heartbeats; });
            timer.start(10);
            QTest::qWait(120);
            QVERIFY(heartbeats >= 3);
            QVERIFY(vm.loading());
            if (replaceEditor) {
                vm.beginAddServiceCard();
                vm.setServiceType("IPTV");
                vm.setServerName("Next playlist");
                vm.setIptvFilePath(source.fileName());
            }
            QVERIFY(query.exec("COMMIT"));
            locked.close();
        }
        QSqlDatabase::removeDatabase("iptv_import_lock");
        QTRY_VERIFY(!vm.loading());
        QTRY_COMPARE(vm.services()->count(), 1);
        auto channels = repository.loadIptvChannels(vm.services()->cardAt(0)->server.id);
        QVERIFY(channels);
        QCOMPARE(channels->size(), size_t{100});
        QVERIFY(!vm.hasError());
        if (replaceEditor) {
            QCOMPARE(vm.serverName(), QString("Next playlist"));
            QCOMPARE(vm.iptvFilePath(), source.fileName());
        } else {
            QVERIFY(vm.iptvFilePath().isEmpty());
        }
    }
    void nativeIptvPlaybackThenImport()
    {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires native Windows desktop");
        QTest::failOnWarning(QRegularExpression(".*Thread failed to finish.*"));
        QTest::failOnWarning(QRegularExpression(".*ReferenceError.*"));
        const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        if (ffmpeg.isEmpty()) QSKIP("Requires FFmpeg for the temporary local stream");
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto video = temporary.filePath("iptv.mp4");
        QProcess generator;
        generator.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=25",
            "-t", "10", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-y", video});
        QVERIFY(generator.waitForFinished(15000));
        QCOMPARE(generator.exitCode(), 0);
        const auto backupVideo = temporary.filePath("backup.mp4");
        QVERIFY(QFile::copy(video, backupVideo));
        QFile playlist(temporary.filePath("channels.m3u"));
        QVERIFY(playlist.open(QIODevice::WriteOnly));
        playlist.write("#EXTM3U\n");
        for (int i = 0; i < 100; ++i) {
            if (i == 0) {
                playlist.write(QString("#EXTINF:-1 group-title=\"Local\",Channel 0\n%1\n")
                    .arg(QUrl::fromLocalFile(temporary.filePath("missing-hd.mp4")).toString()).toUtf8());
            }
            playlist.write(QString("#EXTINF:-1 group-title=\"Local\",Channel %1\n%2\n")
                .arg(i).arg(QUrl::fromLocalFile(video).toString()).toUtf8());
            if (i == 0) {
                playlist.write(QString("#EXTINF:-1 group-title=\"Local\",Channel 0\n%1\n")
                    .arg(QUrl::fromLocalFile(backupVideo).toString()).toUtf8());
                for (int route = 0; route < 4; ++route) {
                    playlist.write(QString("#EXTINF:-1 group-title=\"Local\",Channel 0\n%1\n")
                        .arg(QUrl::fromLocalFile(temporary.filePath(QString("missing-%1.mp4").arg(route))).toString()).toUtf8());
                }
            }
        }
        playlist.close();
        qmlRegisterType<MpvVideoItem>("VibePlayer", 1, 0, "MpvVideoItem");
        QQuickStyle::setStyle("Fusion");
        AppViewModel vm(nullptr, temporary.filePath("session.db"));
        vm.setAutomaticUpdateCheck(false);
        TrayController tray;
        WindowAppearanceController appearance;
        ThemeSnapshotController snapshot;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("appViewModel", &vm);
        engine.rootContext()->setContextProperty("trayController", &tray);
        engine.rootContext()->setContextProperty("windowAppearanceController", &appearance);
        engine.rootContext()->setContextProperty("themeSnapshotController", &snapshot);
        QElapsedTimer elapsed;
        elapsed.start();
        engine.load(QUrl::fromLocalFile(QStringLiteral(VIBEPLAYER_SOURCE_DIR) + "/qml/Main.qml"));
        qInfo() << "Native IPTV loaded:" << elapsed.elapsed();
        QVERIFY(!engine.rootObjects().isEmpty());
        auto* root = engine.rootObjects().first();
        auto* window = qobject_cast<QQuickWindow*>(root);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        vm.setServiceType("IPTV");
        vm.setServerName("Native IPTV test");
        vm.setIptvFilePath(playlist.fileName());
        vm.saveServiceCard();
        QTRY_COMPARE(vm.services()->count(), 1);
        {
            SessionRepository fixture("native-iptv-quality", temporary.filePath("session.db"));
            const auto serviceId = vm.services()->cardAt(0)->server.id;
            const auto channels = fixture.loadIptvChannels(serviceId);
            QVERIFY(channels && !channels->empty());
            const auto& channel = channels->front();
            QCOMPARE(channel.streamUrls.size(), 7);
            const auto now = QDateTime::currentDateTimeUtc();
            std::vector<IptvStreamInfo> infos {
                {.url = channel.streamUrls[0], .available = true, .width = 1920, .height = 1080, .checkedAt = now},
                {.url = channel.streamUrls[1], .available = true, .width = 1280, .height = 720, .checkedAt = now},
                {.url = channel.streamUrls[2], .available = true, .width = 640, .height = 360, .checkedAt = now}};
            QVERIFY(fixture.updateIptvChannelProbe(channel.playlistId, channel.id, 1920, 1080, 0, "h264", "available", infos));
        }
        qInfo() << "Native IPTV imported:" << elapsed.elapsed();
        vm.selectServiceCard(0);
        QTRY_COMPARE(vm.currentView(), QString("iptv"));
        QCOMPARE(vm.iptvChannels()->count(), 100);
        vm.playIptvChannel(0);
        QTRY_COMPARE(vm.currentView(), QString("player"));
        auto* player = root->findChild<MpvVideoItem*>();
        QVERIFY(player);
        QTRY_VERIFY(player->liveStream());
        QTRY_VERIFY(player->position() > 0.1);
        QCOMPARE(vm.currentPlaybackUrl(), QUrl::fromLocalFile(video)); // Failed HD fell back to SD.
        QVERIFY(!vm.hasError());
        QTRY_COMPARE(vm.iptvChannels()->channelAt(0)->width, 1280);
        QTRY_COMPARE(vm.iptvChannels()->channelAt(0)->height, 720);
        auto* streamButton = root->findChild<QQuickItem*>("iptvStreamButton");
        QVERIFY(streamButton);
        QVERIFY(streamButton->isVisible());
        QVERIFY(QMetaObject::invokeMethod(streamButton, "clicked"));
        auto* streamMenu = root->findChild<QQuickItem*>("iptvStreamMenu");
        QVERIFY(streamMenu);
        QTRY_VERIFY(streamMenu->window() && streamMenu->window()->isVisible());
        QCOMPARE(streamMenu->property("count").toInt(), 7);
        // Tool windows must not overlap: repeatedly raising the controls under
        // the translucent menu exposes the progress bar through its lower rows.
        const auto menuAboveControls = [](QQuickItem* menu, QQuickItem* button) {
            return menu->window()->y() + menu->window()->height() <= button->window()->y() - 10;
        };
        QTRY_VERIFY(menuAboveControls(streamMenu, streamButton));
        const auto originalWindowSize = window->size();
        window->resize(980, 640);
        // QWindow applies native resize events asynchronously. Wait for the
        // capped menu size before measuring hover stability.
        QTRY_VERIFY(streamMenu->window()->height() < 440);
        QTRY_VERIFY(menuAboveControls(streamMenu, streamButton));
        QQmlExpression menuWithinPlayer(qmlContext(streamMenu), streamMenu,
            "playerTrackMenuWindow.y >= playerPage.mapToGlobal(0, 0).y + 12");
        QTRY_VERIFY(menuWithinPlayer.evaluate().toBool());
        QTRY_VERIFY(streamMenu->property("contentHeight").toReal() > streamMenu->height());
        QQmlExpression menuFullyOpen(qmlContext(streamMenu), streamMenu,
            "playerTrackMenuRoot.revealProgress === 1");
        QTRY_VERIFY(menuFullyOpen.evaluate().toBool());
        // Hovering the lower rows must leave both native surfaces in place.
        const auto menuGeometry = streamMenu->window()->geometry();
        for (int move = 0; move < 5; ++move) {
            QTest::mouseMove(streamMenu->window(), QPoint(25 + move * 10, streamMenu->window()->height() - 25));
        }
        QCOMPARE(streamMenu->window()->geometry(), menuGeometry);
        // Changing layout while the menu is open must also update its bounds.
        vm.setPlayerLayout("traditional");
        auto* traditionalStreamButton = root->findChild<QQuickItem*>("traditionalIptvStreamButton");
        QVERIFY(traditionalStreamButton);
        QTRY_VERIFY(traditionalStreamButton->isVisible());
        QTRY_VERIFY(menuAboveControls(streamMenu, traditionalStreamButton));
        vm.setPlayerLayout("trendy");
        QTRY_VERIFY(menuAboveControls(streamMenu, streamButton));
        window->resize(originalWindowSize);
        QTRY_COMPARE(streamMenu->window()->height(), 440);
        QTRY_VERIFY(menuAboveControls(streamMenu, streamButton));
        const auto streams = vm.iptvPlaybackStreams();
        const auto backupId = streams[1].toMap().value("id").toString();
        QQmlExpression menuReady(qmlContext(streamMenu), streamMenu, "itemAtIndex(1) !== null");
        QTRY_VERIFY(menuReady.evaluate().toBool());
        QQmlExpression selectBackup(qmlContext(streamMenu), streamMenu,
            "itemAtIndex(1).clicked()");
        selectBackup.evaluate();
        QVERIFY2(!selectBackup.hasError(), qPrintable(selectBackup.error().toString()));
        QTRY_COMPARE(vm.currentPlaybackUrl(), QUrl::fromLocalFile(backupVideo));
        QTRY_VERIFY(player->position() > 0.1);
        QVERIFY(!vm.hasError());
        bool backupSelected = false;
        for (const auto& row : vm.iptvPlaybackStreams()) {
            const auto item = row.toMap();
            if (item.value("id").toString() == backupId) backupSelected = item.value("current").toBool();
        }
        QVERIFY(backupSelected);
        QString primaryId;
        for (const auto& row : streams) {
            const auto item = row.toMap();
            if (item.value("current").toBool()) primaryId = item.value("id").toString();
        }
        QVERIFY(!primaryId.isEmpty());
        vm.switchIptvPlaybackStream(primaryId);
        vm.switchIptvPlaybackStream(backupId); // Old queued file events cannot fail the latest route.
        QTRY_COMPARE(vm.currentPlaybackUrl(), QUrl::fromLocalFile(backupVideo));
        QTRY_VERIFY(player->position() > 0.1);
        QTest::qWait(100);
        QVERIFY(!vm.hasError());
        vm.setPlayerLayout("traditional");
        QTRY_VERIFY(traditionalStreamButton->isVisible());
        QVERIFY(QMetaObject::invokeMethod(traditionalStreamButton, "clicked"));
        QTRY_VERIFY(streamMenu->window() && streamMenu->window()->isVisible());
        QTRY_VERIFY(menuAboveControls(streamMenu, traditionalStreamButton));
        QQmlExpression closeStreams(qmlContext(streamMenu), streamMenu, "playerPage.closeTrackMenu()");
        closeStreams.evaluate();
        QVERIFY2(!closeStreams.hasError(), qPrintable(closeStreams.error().toString()));
        vm.setPlayerLayout("trendy");
        qInfo() << "Native IPTV playing:" << elapsed.elapsed();
        QTest::qWait(2000);
        vm.closePlayerToDetails();
        vm.backToServices();
        qInfo() << "Native IPTV left playback:" << elapsed.elapsed();
        QTest::qWait(500);
        vm.beginAddServiceCard();
        vm.setServiceType("IPTV");
        QQmlExpression showForm(qmlContext(root), root, "serviceDialog.open()");
        showForm.evaluate();
        QVERIFY2(!showForm.hasError(), qPrintable(showForm.error().toString()));
        QTest::qWait(250);
        QQmlExpression header(qmlContext(root), root, "windowHeader");
        QQmlExpression page(qmlContext(root), root, "mainPageRoot");
        const auto headerItem = header.evaluate().value<QObject*>();
        const auto pageItem = page.evaluate().value<QObject*>();
        QVERIFY(headerItem && pageItem);
        const auto backdropSources = [&]() {
            QList<QQuickItem*> sources;
            QList<QQuickItem*> pending{window->contentItem()};
            while (!pending.isEmpty()) {
                auto* item = pending.takeLast();
                pending.append(item->childItems());
                if (!item->inherits("QQuickShaderEffectSource")) continue;
                const auto source = item->property("sourceItem").value<QObject*>();
                if (source == headerItem || source == pageItem) sources.append(item);
            }
            return sources;
        };
        QTRY_COMPARE(backdropSources().size(), 2);
        for (auto* item : backdropSources()) QCOMPARE(item->window(), window);
        vm.chooseIptvPlaylistFile();
        QTest::qWait(800);
        QFileDialog* picker = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == "fileSelectionDialog" && widget->isVisible())
                picker = qobject_cast<QFileDialog*>(widget);
        }
        QVERIFY2(picker, "IPTV picker rejected itself after playback and service form opening");
        QTRY_VERIFY(nativeFileDialogVisible(picker->windowTitle()));
        picker->reject();
        QQmlExpression hideForm(qmlContext(root), root, "serviceDialog.close()");
        hideForm.evaluate();
        window->hide();
        QTest::qWait(1200); // Let the native Shell and its deferred helper cleanup finish.
        qInfo() << "Native IPTV workflow milliseconds:" << elapsed.elapsed();
#else
        QSKIP("Windows native IPTV and file picker integration");
#endif
    }
    void uploadsCoalesceDirectoryRefreshAndDiscardNavigation_data()
    {
        QTest::addColumn<bool>("leavePage");
        QTest::newRow("burst-refresh-once") << false;
        QTest::newRow("navigation-discards-refresh") << true;
    }
    void uploadsCoalesceDirectoryRefreshAndDiscardNavigation()
    {
        QFETCH(bool, leavePage);
        QTemporaryDir temporary;
        DirectoryEndpoint endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
        auto service = card("refresh-test");
        service.server.baseUrl = QString("http://localhost:%1/").arg(endpoint.serverPort());
        AppViewModel vm(nullptr, temporary.filePath("cards.db"));
        vm.services()->setCards({service});
        vm.editServiceCard(0);
        vm.loginSelectedService("fake-password");
        QTRY_VERIFY(vm.currentView() == "webdav" && !vm.loading());
        QCOMPARE(endpoint.lists, 1);
        QList<QUrl> files;
        for (int i = 0; i < 3; ++i) {
            QFile file(temporary.filePath(QString("file-%1.txt").arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("test"), qint64(4));
            files.append(QUrl::fromLocalFile(file.fileName()));
        }
        vm.uploadWebDavFiles(files);
        QCOMPARE(vm.currentView(), QString("transfers"));
        // Reopen the directory while PUT is still pending.
        vm.refreshWebDavDirectory();
        QTRY_VERIFY(vm.currentView() == "webdav" && !vm.loading());
        QCOMPARE(endpoint.lists, 2);
        endpoint.allowUploads = true;
        QTRY_COMPARE(endpoint.uploads, 3);
        QTRY_COMPARE(vm.activeTransferCount(), 0);
        if (leavePage) vm.openSettings();
        QTest::qWait(400);
        QCOMPARE(endpoint.lists, leavePage ? 2 : 3);
        QCOMPARE(vm.currentView(), QString(leavePage ? "settings" : "webdav"));
        QVERIFY(!vm.hasError());
    }
    void loadingCardBindingFollowsPendingCardAndNavigation()
    {
        QTemporaryDir temporary;
        AppViewModel vm(nullptr, temporary.filePath("unused.db"));
        vm.services()->setCards({card("first"), card("second")});
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("vm", &vm);
        QQmlComponent component(&engine);
        component.setData("import QtQml; QtObject { property string selected: vm.loadingServiceCardId }", QUrl());
        QScopedPointer<QObject> binding(component.create());
        QVERIFY2(binding, qPrintable(component.errorString()));
        QCOMPARE(binding->property("selected").toString(), QString());
        vm.selectServiceCard(0);
        QCOMPARE(binding->property("selected").toString(), QString("first"));
        QVERIFY(vm.loading());
        // Replacing the pending card without changing loading used to leave
        // QML's cached binding on the old ID.
        vm.editServiceCard(1);
        QVERIFY(vm.loading());
        QCOMPARE(binding->property("selected").toString(), QString("second"));
        vm.openSettings();
        QCOMPARE(binding->property("selected").toString(), QString());
        QTest::qWait(40);
        QCOMPARE(vm.currentView(), QString("settings"));
        QVERIFY(!vm.loading());
        QVERIFY(!QFile::exists(temporary.filePath("unused.db")));
    }
    void backgroundSnapshotPreservesPrivacyOnLock()
    {
        QTemporaryDir temporary;
        const auto path = temporary.filePath("cards.db");
        SessionRepository repository("viewmodel_test_seed", path);
        QVERIFY(repository.initialize());
        QVERIFY(repository.saveServer(card("public").server));
        QVERIFY(repository.saveServer(card("private", true).server));
        AppViewModel vm(nullptr, path);
        vm.backToServices();
        QTRY_COMPARE(vm.services()->count(), 1);
        QCOMPARE(vm.services()->cardAt(0)->server.id, QString("public"));
        QVERIFY(vm.changePrivacyPin({}, "1234", "1234"));
        QVERIFY(vm.unlockPrivacyMode("1234"));
        QTRY_COMPARE(vm.services()->count(), 2);
        QTRY_VERIFY(!vm.globalHistoryLoading());
        QVERIFY(!vm.hasError());
        vm.backToServices(); // A private-mode refresh is now queued/in flight.
        vm.exitPrivacyMode();
        QCOMPARE(vm.services()->count(), 1); // No event-loop turn is needed to hide it.
        QCOMPARE(vm.services()->cardAt(0)->server.id, QString("public"));
        QTest::qWait(200);
        QCOMPARE(vm.services()->count(), 1);
        QCOMPARE(vm.privacyCards()->count(), 2); // Management metadata keeps its existing policy.
    }
};

QTEST_MAIN(AppViewModelStateTest)
#include "AppViewModelStateTest.moc"
