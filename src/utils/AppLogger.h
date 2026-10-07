#pragma once

#include <QString>
#include <functional>

namespace AppLogger {
// Configure once at startup, before starting worker threads.
void setSanitizer(std::function<QString(const QString&)> sanitizer);
void info(const QString& area, const QString& message);
void warning(const QString& area, const QString& message);
}
