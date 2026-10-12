#pragma once

#include <QCoreApplication>
#include <QString>

#ifdef Q_OS_WIN
#include <qt_windows.h>

// Check the actual Windows Shell window, not QFileDialog's hidden QWidget host.
inline bool nativeFileDialogVisible(const QString& title)
{
    struct Search { QString title; bool found{false}; } search{title};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(parameter);
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != static_cast<DWORD>(QCoreApplication::applicationPid()) || !IsWindowVisible(window))
            return TRUE;
        wchar_t className[64]{};
        GetClassNameW(window, className, 64);
        if (QString::fromWCharArray(className) != QStringLiteral("#32770")) return TRUE;
        wchar_t caption[512]{};
        GetWindowTextW(window, caption, 512);
        if (QString::fromWCharArray(caption) != search.title) return TRUE;
        search.found = true;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
#endif
