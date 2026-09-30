#include "app/FileDialogController.h"

#include "utils/AppLogger.h"

#include <QAbstractItemView>
#include <QFileDialog>
#include <QFileInfo>
#include <QTimer>

#include <utility>

FileDialogController::FileDialogController(QObject* parent)
    : QObject(parent)
{
}

FileDialogController::~FileDialogController()
{
    // A picker must not call back into a partially destroyed ViewModel.
    if (m_dialog) {
        disconnect(m_dialog.get(), nullptr, this, nullptr);
        m_dialog->reject();
    }
}

void FileDialogController::attachWindow(QWindow* window)
{
    if (m_window == window) {
        return;
    }
    cancel();
    disconnect(m_windowDestroyedConnection);
    disconnect(m_windowVisibilityConnection);
    m_window = window;
    if (window) {
        m_windowDestroyedConnection = connect(window, &QObject::destroyed, this, [this]() {
            cancel();
        });
        m_windowVisibilityConnection = connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
            if (!visible) {
                cancel();
            }
        });
    }
}

void FileDialogController::cancel()
{
    if (m_dialog) {
        m_dialog->reject();
    }
}

bool FileDialogController::isOpen() const
{
    return m_dialog != nullptr;
}

FileDialogController::MultiSelectionHandler FileDialogController::singleSelection(SelectionHandler handler)
{
    return [handler = std::move(handler)](const QStringList& paths) {
        handler(paths.isEmpty() ? QString{} : paths.constFirst());
    };
}

void FileDialogController::openFile(const QString& title, const QString& initialPath,
                                    const QString& filter, SelectionHandler handler)
{
    open(Mode::OpenFile, title, initialPath, filter, {}, singleSelection(std::move(handler)));
}

void FileDialogController::openFiles(const QString& title, const QString& initialPath,
                                     const QString& filter, MultiSelectionHandler handler)
{
    open(Mode::OpenFiles, title, initialPath, filter, {}, std::move(handler));
}

void FileDialogController::saveFile(const QString& title, const QString& initialPath,
                                    const QString& filter, const QString& suffix, SelectionHandler handler)
{
    open(Mode::SaveFile, title, initialPath, filter, suffix, singleSelection(std::move(handler)));
}

void FileDialogController::selectDirectory(const QString& title, const QString& initialPath,
                                           SelectionHandler handler)
{
    open(Mode::Directory, title, initialPath, {}, {}, singleSelection(std::move(handler)));
}

void FileDialogController::selectDirectories(const QString& title, const QString& initialPath,
                                             MultiSelectionHandler handler)
{
    open(Mode::Directories, title, initialPath, {}, {}, std::move(handler));
}

void FileDialogController::open(Mode mode, const QString& title, const QString& initialPath,
                               const QString& filter, const QString& suffix, MultiSelectionHandler handler)
{
    if (m_dialog) {
        // Repeated clicks cannot create concurrent native Shell dialogs.
        return;
    }
    if (!m_window) {
        AppLogger::warning(QStringLiteral("file-dialog"), QStringLiteral("Cannot open a picker without its owner window"));
        return;
    }

    m_dialog = std::make_unique<QFileDialog>();
    auto* dialog = m_dialog.get();
    dialog->setObjectName(QStringLiteral("fileSelectionDialog"));
    // Qt's public API only supports single-directory selection. Preserve batch
    // folder selection with the widget fallback, confined to this one mode.
    dialog->setOption(QFileDialog::DontUseNativeDialog, mode == Mode::Directories);
    dialog->setOption(QFileDialog::DontUseCustomDirectoryIcons, true);
    dialog->setAttribute(Qt::WA_QuitOnClose, false);
    dialog->setSupportedSchemes({QStringLiteral("file")});
    dialog->setWindowTitle(title);
    dialog->setAcceptMode(mode == Mode::SaveFile ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    const bool directoryMode = mode == Mode::Directory || mode == Mode::Directories;
    dialog->setFileMode(directoryMode ? QFileDialog::Directory
                        : mode == Mode::OpenFiles ? QFileDialog::ExistingFiles
                        : mode == Mode::SaveFile ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
    dialog->setOption(QFileDialog::ShowDirsOnly, directoryMode);
    if (!filter.isEmpty()) {
        dialog->setNameFilter(filter);
    }
    if (!initialPath.isEmpty()) {
        if (directoryMode || QFileInfo(initialPath).isDir()) {
            dialog->setDirectory(initialPath);
        } else {
            dialog->selectFile(initialPath);
        }
    }
    dialog->setDefaultSuffix(suffix);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->winId(); // Create the QWidget window without showing a blank fallback.
    dialog->windowHandle()->setTransientParent(m_window);

    if (mode == Mode::Directories) {
        const auto enableMultiSelection = [dialog]() {
            for (auto* view : dialog->findChildren<QAbstractItemView*>()) {
                view->setSelectionMode(QAbstractItemView::ExtendedSelection);
            }
        };
        enableMultiSelection();
        QTimer::singleShot(0, dialog, enableMultiSelection);
    }

    m_handler = std::move(handler);
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        QStringList paths;
        if (result == QDialog::Accepted) {
            for (const auto& url : dialog->selectedUrls()) {
                if (url.isLocalFile() && !url.toLocalFile().isEmpty()) {
                    paths.append(url.toLocalFile());
                }
            }
        }
        auto handler = std::move(m_handler);
        // Clear ownership before the continuation can navigate or open a picker.
        m_dialog.release();
        dialog->deleteLater();
        AppLogger::info(QStringLiteral("file-dialog"),
                        result == QDialog::Accepted ? QStringLiteral("Selection accepted (%1 paths)").arg(paths.size())
                                                    : QStringLiteral("Selection canceled"));
        if (handler) {
            handler(paths);
        }
    });
    AppLogger::info(QStringLiteral("file-dialog"),
                    mode == Mode::Directories ? QStringLiteral("Opening batch folder picker with Qt fallback")
                                              : QStringLiteral("Opening native-preferred picker asynchronously"));
    // QDialog::open() keeps the normal Qt event loop running. In Qt 6.7's
    // Windows backend the Shell picker runs on Qt's own STA dialog thread.
    dialog->open();
}
