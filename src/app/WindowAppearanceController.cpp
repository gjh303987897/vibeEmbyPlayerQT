#include "app/WindowAppearanceController.h"

#include <QColor>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWindow>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>

namespace {
void refreshNonClientFrame(HWND hwnd, bool active)
{
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // A focus change makes DWM recompute the caption state. Reproduce only
    // that non-client transition while leaving the real window focus intact.
    const auto activeState = active ? TRUE : FALSE;
    SendMessageW(hwnd, WM_NCACTIVATE, active ? FALSE : TRUE, 0);
    SendMessageW(hwnd, WM_NCACTIVATE, activeState, 0);

    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    DwmFlush();
}
}
#endif

WindowAppearanceController::WindowAppearanceController(QObject* parent)
    : QObject(parent)
{
}

void WindowAppearanceController::attachWindow(QObject* window)
{
    if (m_window) {
        disconnect(m_window, nullptr, this, nullptr);
    }

    m_window = qobject_cast<QQuickWindow*>(window);
    if (!m_window) {
        return;
    }

    connect(m_window, &QWindow::visibleChanged, this, [this](bool) {
        scheduleApply();
    });
    connect(m_window, &QWindow::visibilityChanged, this, [this](QWindow::Visibility) {
        scheduleApply();
    });

    apply();
    scheduleApply();
}

void WindowAppearanceController::applyTheme(const QString& effectiveTheme)
{
    if (m_effectiveTheme == effectiveTheme) {
        return;
    }
    m_effectiveTheme = effectiveTheme;
    apply();
    scheduleApply();
}

void WindowAppearanceController::apply()
{
    if (!m_window) {
        return;
    }

    const auto darkMode = m_effectiveTheme != QStringLiteral("light");
    m_window->setColor(darkMode ? QColor(15, 18, 23) : QColor(245, 247, 251));
    if (auto* contentItem = m_window->contentItem()) {
        contentItem->update();
    }
    m_window->update();

#ifdef Q_OS_WIN
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!hwnd) {
        return;
    }

    const BOOL useDarkMode = darkMode ? TRUE : FALSE;
    const auto darkResult = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
    if (FAILED(darkResult)) {
        constexpr DWORD legacyImmersiveDarkModeAttribute = 19;
        DwmSetWindowAttribute(hwnd, legacyImmersiveDarkModeAttribute, &useDarkMode, sizeof(useDarkMode));
    }

    const COLORREF captionColor = darkMode ? RGB(15, 18, 23) : RGB(245, 247, 251);
    const COLORREF borderColor = darkMode ? RGB(48, 57, 69) : RGB(216, 224, 234);
    const COLORREF textColor = darkMode ? RGB(244, 247, 251) : RGB(21, 25, 34);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));

    refreshNonClientFrame(hwnd, m_window->isActive());
#endif
}

void WindowAppearanceController::scheduleApply()
{
    if (!m_window) {
        return;
    }

    QTimer::singleShot(0, this, &WindowAppearanceController::apply);
    QTimer::singleShot(120, this, &WindowAppearanceController::apply);
}
