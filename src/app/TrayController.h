#pragma once

#include <QObject>
#include <QPointer>
#include <QSystemTrayIcon>

class QAction;
class QIcon;
class QMenu;
class QQuickWindow;

class TrayController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool minimizeToTray READ minimizeToTray WRITE setMinimizeToTray NOTIFY minimizeToTrayChanged)
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY trayAvailableChanged)

public:
    explicit TrayController(QObject* parent = nullptr);
    ~TrayController() override;

    bool minimizeToTray() const;
    void setMinimizeToTray(bool value);
    bool trayAvailable() const;
    void setIcon(const QIcon& icon);

    Q_INVOKABLE void attachWindow(QObject* window);
    Q_INVOKABLE void hideToTray();
    Q_INVOKABLE void restoreWindow();
    Q_INVOKABLE void quitApplication();

signals:
    void minimizeToTrayChanged();
    void trayAvailableChanged();

private:
    void updateTrayVisibility();

    bool m_minimizeToTray { true };
    QPointer<QQuickWindow> m_window;
    QSystemTrayIcon m_trayIcon;
    QMenu* m_menu { nullptr };
    QAction* m_restoreAction { nullptr };
    QAction* m_quitAction { nullptr };
};
