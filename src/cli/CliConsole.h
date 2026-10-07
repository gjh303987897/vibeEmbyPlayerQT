#pragma once
#include <QString>
#include <atomic>
#include <optional>
#include <functional>
#include <QStringList>

namespace CliConsole {
extern std::atomic_bool canceled;
extern std::atomic_bool transferPaused;
void initialize();
bool pollCancellation();
bool interactive();
int outputColumns();
bool launchDesktop(const QString& program);
std::optional<QString> readLine(const QString& prompt, bool hidden = false);
void progress(const QString& text);
void setLineEditor(std::function<QStringList(const QString&)> completion,
                   std::function<QStringList()> history);
}
