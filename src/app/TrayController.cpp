#include "app/TrayController.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QQuickWindow>
#include <QStyle>

TrayController::TrayController(QObject* parent)
    : QObject(parent)
    , m_trayIcon(this)
{
    m_menu = new QMenu();
    m_restoreAction = m_menu->addAction(QStringLiteral("Show vibePlayerQT"));
    m_quitAction = m_menu->addAction(QStringLiteral("Quit"));

    connect(m_restoreAction, &QAction::triggered, this, &TrayController::restoreWindow);
    connect(m_quitAction, &QAction::triggered, this, &TrayController::quitApplication);
    connect(&m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            restoreWindow();
        }
    });

    m_trayIcon.setIcon(QApplication::style()->standardIcon(QStyle::SP_MediaPlay));
    m_trayIcon.setToolTip(QStringLiteral("vibePlayerQT"));
    m_trayIcon.setContextMenu(m_menu);
    updateTrayVisibility();
}

TrayController::~TrayController()
{
    delete m_menu;
}

bool TrayController::minimizeToTray() const
{
    return m_minimizeToTray;
}

bool TrayController::trayAvailable() const
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayController::setIcon(const QIcon& icon)
{
    if (!icon.isNull()) {
        m_trayIcon.setIcon(icon);
    }
}

void TrayController::setMinimizeToTray(bool value)
{
    if (m_minimizeToTray == value) {
        return;
    }
    m_minimizeToTray = value;
    updateTrayVisibility();
    emit minimizeToTrayChanged();
}

void TrayController::attachWindow(QObject* window)
{
    m_window = qobject_cast<QQuickWindow*>(window);
}

void TrayController::hideToTray()
{
    if (!m_minimizeToTray || !trayAvailable() || !m_window) {
        return;
    }
    m_window->hide();
    updateTrayVisibility();
}

void TrayController::restoreWindow()
{
    if (!m_window) {
        return;
    }

    m_window->show();
    m_window->raise();
    m_window->requestActivate();
}

void TrayController::quitApplication()
{
    QApplication::quit();
}

void TrayController::updateTrayVisibility()
{
    if (m_minimizeToTray && trayAvailable()) {
        m_trayIcon.show();
    } else {
        m_trayIcon.hide();
    }
}
