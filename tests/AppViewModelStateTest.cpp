#include "viewmodels/AppViewModel.h"
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
