#include "app/FileDialogController.h"

#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>

#include <memory>

namespace {
QFileDialog* activePicker()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("fileSelectionDialog") && widget->isVisible()) {
            return qobject_cast<QFileDialog*>(widget);
        }
    }
    return nullptr;
}
}

class FileDialogControllerTest final : public QObject {
    Q_OBJECT

private slots:
    void cleanup()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void modesPreferNativeAndKeepQuickWindowOwnership()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QWindow window;
        FileDialogController picker;
        picker.attachWindow(&window);
        for (int mode = 0; mode < 5; ++mode) {
            int completions = 0;
            auto single = [&completions](const QString& path) { QVERIFY(path.isEmpty()); ++completions; };
            auto multiple = [&completions](const QStringList& paths) { QVERIFY(paths.isEmpty()); ++completions; };
            switch (mode) {
            case 0: picker.openFile(QStringLiteral("Open"), directory.path(), QStringLiteral("Videos (*.mp4)"), single); break;
            case 1: picker.openFiles(QStringLiteral("Open files"), directory.path(), {}, multiple); break;
            case 2: picker.saveFile(QStringLiteral("Save"), directory.filePath(QStringLiteral("backup")),
                                    QStringLiteral("TSSL (*.tssl)"), QStringLiteral("tssl"), single); break;
            case 3: picker.selectDirectory(QStringLiteral("Folder"), directory.path(), single); break;
            case 4: picker.selectDirectories(QStringLiteral("Folders"), directory.path(), multiple); break;
            }
            QVERIFY(picker.isOpen());
            auto* dialog = activePicker();
            QVERIFY(dialog);
            QCOMPARE(dialog->windowHandle()->transientParent(), &window);
            QCOMPARE(dialog->windowModality(), Qt::WindowModal);
            QCOMPARE(dialog->testOption(QFileDialog::DontUseNativeDialog), mode == 4);
            QVERIFY(!dialog->testOption(QFileDialog::DontConfirmOverwrite));
            QVERIFY(!dialog->testAttribute(Qt::WA_QuitOnClose));
            QCOMPARE(dialog->supportedSchemes(), QStringList{QStringLiteral("file")});
            if (mode == 3 || mode == 4) {
                QCOMPARE(dialog->directory().absolutePath(), directory.path());
                QVERIFY(dialog->testOption(QFileDialog::ShowDirsOnly));
            }
            picker.cancel();
            QVERIFY(!picker.isOpen());
            QCOMPARE(completions, 1);
            cleanup();
        }
    }

    void openKeepsTimersRunningAndIgnoresDuplicateRequests()
    {
        QWindow window;
        FileDialogController picker;
        picker.attachWindow(&window);
        int completions = 0;
        picker.openFiles(QStringLiteral("Upload"), {}, {}, [&completions](const QStringList& paths) {
            QVERIFY(paths.isEmpty());
            ++completions;
        });
        auto* first = activePicker();
        QVERIFY(first);
        picker.selectDirectory(QStringLiteral("Duplicate"), {}, [&completions](const QString&) {
            completions += 100;
        });
        QCOMPARE(activePicker(), first);
        bool timerRan = false;
        QTimer::singleShot(0, &picker, [&]() { timerRan = true; picker.cancel(); });
        QTRY_VERIFY(timerRan);
        QCOMPARE(completions, 1);
        picker.cancel();
        QCOMPARE(completions, 1);
    }

    void acceptsFileThenAllowsAnotherPickerInContinuation()
    {
        QTemporaryDir directory;
        const auto filePath = directory.filePath(QStringLiteral("video.mp4"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QWindow window;
        FileDialogController picker;
        picker.attachWindow(&window);
        QString selected;
        QString selectedDirectory;
        picker.openFile(QStringLiteral("Video"), filePath, QStringLiteral("Videos (*.mp4)"), [&](const QString& path) {
            selected = path;
            picker.selectDirectory(QStringLiteral("Folder"), directory.path(), [&](const QString& path) {
                selectedDirectory = path;
            });
        });
        auto* dialog = activePicker();
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection));
        QTRY_COMPARE(selected, filePath);
        QVERIFY(picker.isOpen());
        dialog = activePicker();
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection));
        QTRY_COMPARE(selectedDirectory, directory.path());
        QVERIFY(!picker.isOpen());
    }

    void saveAddsSuffixWithoutWritingOnAcceptance()
    {
        QTemporaryDir directory;
        QWindow window;
        FileDialogController picker;
        picker.attachWindow(&window);
        QString selected;
        picker.saveFile(QStringLiteral("Export"), directory.filePath(QStringLiteral("backup")),
                        QStringLiteral("TSSL (*.tssl)"), QStringLiteral("tssl"),
                        [&](const QString& path) { selected = path; });
        auto* dialog = activePicker();
        QVERIFY(dialog);
        QCOMPARE(dialog->defaultSuffix(), QStringLiteral("tssl"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection));
        QTRY_COMPARE(selected, directory.filePath(QStringLiteral("backup.tssl")));
        QVERIFY(!QFile::exists(selected));
    }

    void multipleSelectionReturnsAllFilesAndFolders_data()
    {
        QTest::addColumn<bool>("folders");
        QTest::newRow("files") << false;
        QTest::newRow("folders") << true;
    }

    void multipleSelectionReturnsAllFilesAndFolders()
    {
        QFETCH(bool, folders);
        QTemporaryDir directory;
        QStringList expected;
        for (const auto& name : {QStringLiteral("first"), QStringLiteral("second")}) {
            const auto path = directory.filePath(name);
            if (folders) {
                QVERIFY(QDir().mkpath(path));
            } else {
                QFile file(path);
                QVERIFY(file.open(QIODevice::WriteOnly));
            }
            expected.append(path);
        }
        QWindow window;
        FileDialogController picker;
        picker.attachWindow(&window);
        QStringList selected;
        auto complete = [&](const QStringList& paths) { selected = paths; };
        if (folders) {
            picker.selectDirectories(QStringLiteral("Folders"), directory.path(), complete);
        } else {
            picker.openFiles(QStringLiteral("Files"), directory.path(), {}, complete);
        }
        auto* dialog = activePicker();
        QVERIFY(dialog);
        dialog->setViewMode(QFileDialog::Detail);
        auto* view = dialog->findChild<QTreeView*>();
        QVERIFY(view);
        auto* model = qobject_cast<QFileSystemModel*>(view->model());
        QVERIFY(model);
        QTRY_VERIFY(model->index(expected[0]).isValid() && model->index(expected[1]).isValid());
        view->selectionModel()->select(model->index(expected[0]), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        view->selectionModel()->select(model->index(expected[1]), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection));
        QTRY_COMPARE(selected.size(), 2);
        selected.sort();
        QCOMPARE(selected, expected);
    }

    void ownerHideAndDestructionCancelSelection()
    {
        auto window = std::make_unique<QWindow>();
        window->show();
        FileDialogController picker;
        picker.attachWindow(window.get());
        int completions = 0;
        auto complete = [&completions](const QString& path) { QVERIFY(path.isEmpty()); ++completions; };
        picker.selectDirectory(QStringLiteral("Hide"), {}, complete);
        window->hide();
        QVERIFY(!picker.isOpen());
        QCOMPARE(completions, 1);
        picker.selectDirectory(QStringLiteral("Destroy"), {}, complete);
        window.reset();
        QVERIFY(!picker.isOpen());
        QCOMPARE(completions, 2);
    }

    void controllerDestructionDoesNotRunBusinessCallback()
    {
        QWindow window;
        int completions = 0;
        auto picker = std::make_unique<FileDialogController>();
        picker->attachWindow(&window);
        picker->openFiles(QStringLiteral("Close app"), {}, {}, [&](const QStringList&) { ++completions; });
        QVERIFY(activePicker());
        picker.reset();
        QCOMPARE(completions, 0);
        QVERIFY(!activePicker());
    }
};

QTEST_MAIN(FileDialogControllerTest)
#include "FileDialogControllerTest.moc"
