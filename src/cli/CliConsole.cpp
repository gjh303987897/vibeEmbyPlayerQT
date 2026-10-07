#include "cli/CliConsole.h"
#include <QStringConverter>
#include <QProcess>
#include <QTextStream>
#include <csignal>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

std::atomic_bool CliConsole::canceled {false};
std::atomic_bool CliConsole::transferPaused {false};
namespace {
std::function<QStringList(const QString&)> completion;
std::function<QStringList()> history;
#ifdef Q_OS_WIN
BOOL WINAPI interrupt(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        CliConsole::canceled.store(true);
        return TRUE;
    }
    return FALSE;
}

#else
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
#endif
}

void CliConsole::setLineEditor(std::function<QStringList(const QString&)> complete,
                              std::function<QStringList()> entries)
{
    completion = std::move(complete); history = std::move(entries);
}

void CliConsole::initialize()
{
#ifdef Q_OS_WIN
    SetConsoleCtrlHandler(nullptr, FALSE);
    SetConsoleCtrlHandler(interrupt, TRUE);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
    std::signal(SIGINT, interrupt);
#endif
}

bool CliConsole::pollCancellation()
{
#ifndef Q_OS_WIN
    if (interrupted) { canceled.store(true); interrupted = 0; }
#endif
    return canceled.load();
}

bool CliConsole::interactive()
{
#ifdef Q_OS_WIN
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode);
#else
    return isatty(STDIN_FILENO);
#endif
}

int CliConsole::outputColumns()
{
#ifdef Q_OS_WIN
    CONSOLE_SCREEN_BUFFER_INFO info {};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
#else
    winsize size {};
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
        return size.ws_col;
#endif
    // Fixed, deterministic layout for redirected output and unknown terminals.
    return 120;
}

bool CliConsole::launchDesktop(const QString& program)
{
    QProcess desktop;
    desktop.setProgram(program);
#ifdef Q_OS_WIN
    // A launcher can itself inherit a hidden startup window from automation.
    // The no-argument desktop entry must explicitly show its GUI window.
    desktop.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->inheritHandles = false;
        arguments->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
        arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        arguments->startupInfo->wShowWindow = SW_SHOWNORMAL;
    });
#endif
    return desktop.startDetached();
}

std::optional<QString> CliConsole::readLine(const QString& prompt, bool hidden)
{
    QTextStream output(stderr);
    output.setEncoding(QStringConverter::Utf8);
    if (interactive()) { output << prompt; output.flush(); }
#ifdef Q_OS_WIN
    if (interactive()) {
        const auto input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD original = 0;
        GetConsoleMode(input, &original);
        if (!hidden && completion && prompt.startsWith("vibe")) {
            SetConsoleMode(input,original & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));
            struct Restore { HANDLE handle; DWORD mode; ~Restore() { SetConsoleMode(handle,mode); } } restore {input,original};
            QString line;
            const auto entries = history ? history() : QStringList();
            int historyIndex = entries.size(), cursor = 0, oldLength = 0;
            auto redraw = [&] {
                output << '\r' << prompt << line << QString(oldLength * 2 + 2,' ') << '\r' << prompt << line.left(cursor);
                output.flush(); oldLength = line.size();
            };
            while (true) {
                if (pollCancellation()) { canceled.store(false); output << '\n'; output.flush(); return QString(); }
                if (WaitForSingleObject(input,100) != WAIT_OBJECT_0) continue;
                INPUT_RECORD record {}; DWORD count = 0;
                if (!ReadConsoleInputW(input,&record,1,&count)) return std::nullopt;
                if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
                const auto& key = record.Event.KeyEvent;
                if (key.wVirtualKeyCode == VK_RETURN) { output << '\n'; output.flush(); return line; }
                if (key.wVirtualKeyCode == VK_TAB) {
                    auto matches = completion(line);
                    if (matches.size() == 1) { line = matches.front(); cursor = line.size(); }
                    else if (!matches.isEmpty()) {
                        output << '\n' << matches.join('\n') << '\n'; oldLength = 0;
                        auto common = matches.front();
                        for (const auto& item : matches) while (!item.startsWith(common) && !common.isEmpty()) common.chop(1);
                        if (common.size() > line.size()) { line = common; cursor = line.size(); }
                    }
                } else if (key.wVirtualKeyCode == VK_UP && historyIndex > 0) { line = entries[--historyIndex]; cursor = line.size(); }
                else if (key.wVirtualKeyCode == VK_DOWN) { line = ++historyIndex < entries.size() ? entries[historyIndex] : QString(); historyIndex = qMin(historyIndex,static_cast<int>(entries.size())); cursor = line.size(); }
                else if (key.wVirtualKeyCode == VK_LEFT) cursor = qMax(0,cursor - 1);
                else if (key.wVirtualKeyCode == VK_RIGHT) cursor = qMin(static_cast<int>(line.size()),cursor + 1);
                else if (key.wVirtualKeyCode == VK_HOME) cursor = 0;
                else if (key.wVirtualKeyCode == VK_END) cursor = line.size();
                else if (key.wVirtualKeyCode == VK_BACK && cursor > 0) line.remove(--cursor,1);
                else if (key.wVirtualKeyCode == VK_DELETE && cursor < line.size()) line.remove(cursor,1);
                else if (key.uChar.UnicodeChar >= 0x20) {
                    if (line.size() < 32768) line.insert(cursor++,QChar(key.uChar.UnicodeChar));
                }
                redraw();
            }
        }
        if (hidden) SetConsoleMode(input, original & ~ENABLE_ECHO_INPUT);
        QString line;
        wchar_t buffer[256];
        DWORD count = 0;
        bool ok = true;
        do {
            ok = ReadConsoleW(input, buffer, 256, &count, nullptr);
            if (!ok || count == 0) break;
            line += QString::fromWCharArray(buffer, count);
        } while (!line.endsWith('\n'));
        if (hidden) { SetConsoleMode(input, original); output << '\n'; output.flush(); }
        if (!ok || count == 0) return std::nullopt;
        while (line.endsWith('\n') || line.endsWith('\r')) line.chop(1);
        return line;
    }
#else
    termios original {};
    if (hidden && interactive()) {
        tcgetattr(STDIN_FILENO, &original);
        auto changed = original;
        changed.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &changed);
    }
#endif
    // Keep one stream: a new stream per command would discard buffered pipe input.
    static QTextStream input(stdin);
    input.setEncoding(QStringConverter::Utf8);
    const auto line = input.readLine();
#ifndef Q_OS_WIN
    if (hidden && interactive()) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &original); output << '\n'; output.flush(); }
#endif
    if (line.isNull()) return std::nullopt;
    return line;
}

void CliConsole::progress(const QString& text)
{
#ifndef Q_OS_WIN
    if (interrupted) { canceled.store(true); interrupted = 0; }
#endif
    QTextStream stream(stderr);
    stream.setEncoding(QStringConverter::Utf8);
    stream << text << '\n';
    stream.flush();
}
