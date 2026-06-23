#include "app/WindowAppearanceController.h"

#include <QQuickWindow>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>
#endif

WindowAppearanceController::WindowAppearanceController(QObject* parent)
    : QObject(parent)
{
}

void WindowAppearanceController::attachWindow(QObject* window)
{
    m_window = qobject_cast<QQuickWindow*>(window);
    apply();
}

void WindowAppearanceController::applyTheme(const QString& effectiveTheme)
{
    if (m_effectiveTheme == effectiveTheme) {
        return;
    }
    m_effectiveTheme = effectiveTheme;
    apply();
}

void WindowAppearanceController::apply()
{
#ifdef Q_OS_WIN
    if (!m_window) {
        return;
    }

    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!hwnd) {
        return;
    }

    const auto darkMode = m_effectiveTheme != QStringLiteral("light");
    const BOOL useDarkMode = darkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

    const COLORREF captionColor = darkMode ? RGB(15, 18, 23) : RGB(245, 247, 251);
    const COLORREF borderColor = darkMode ? RGB(48, 57, 69) : RGB(216, 224, 234);
    const COLORREF textColor = darkMode ? RGB(244, 247, 251) : RGB(21, 25, 34);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
#endif
}
