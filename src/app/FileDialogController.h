#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QWindow>

#include <functional>
#include <memory>

class QFileDialog;

// Owns asynchronous file pickers for the Quick window. QFileDialog uses the
// platform native implementation by default without replacing the Quick UI.
class FileDialogController final : public QObject {
    Q_OBJECT

public:
    using SelectionHandler = std::function<void(const QString&)>;
    using MultiSelectionHandler = std::function<void(const QStringList&)>;

    explicit FileDialogController(QObject* parent = nullptr);
    ~FileDialogController() override;

    void attachWindow(QWindow* window);
    void cancel();
    bool isOpen() const;

    void openFile(const QString& title, const QString& initialPath,
                  const QString& filter, SelectionHandler handler);
    void openFiles(const QString& title, const QString& initialPath,
                   const QString& filter, MultiSelectionHandler handler);
    void saveFile(const QString& title, const QString& initialPath,
                  const QString& filter, const QString& suffix, SelectionHandler handler);
    void selectDirectory(const QString& title, const QString& initialPath,
                         SelectionHandler handler);
    void selectDirectories(const QString& title, const QString& initialPath,
                           MultiSelectionHandler handler);

private:
    enum class Mode { OpenFile, OpenFiles, SaveFile, Directory, Directories };
    void open(Mode mode, const QString& title, const QString& initialPath,
              const QString& filter, const QString& suffix, MultiSelectionHandler handler);
    static MultiSelectionHandler singleSelection(SelectionHandler handler);

    QPointer<QWindow> m_window;
    std::unique_ptr<QFileDialog> m_dialog;
    MultiSelectionHandler m_handler;
    QMetaObject::Connection m_windowDestroyedConnection;
    QMetaObject::Connection m_windowVisibilityConnection;
};
