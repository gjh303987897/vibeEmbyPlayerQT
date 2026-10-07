#include "utils/AppLogger.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTextStream>

Q_LOGGING_CATEGORY(vibePlayerLog, "vibeplayer")

namespace AppLogger {
namespace {
std::function<QString(const QString&)> messageSanitizer;
void appendLine(const QString& level, const QString& area, const QString& message)
{
    const auto logPath = qEnvironmentVariable("VIBEPLAYER_LOG_FILE");
    if (logPath.isEmpty()) {
        return;
    }

    QFile file(logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    stream << level << ' ' << QStringLiteral("[%1] ").arg(area) << message << '\n';
}
}

void setSanitizer(std::function<QString(const QString&)> sanitizer)
{
    messageSanitizer = std::move(sanitizer);
}

void info(const QString& area, const QString& message)
{
    const auto safe = messageSanitizer ? messageSanitizer(message) : message;
    qCInfo(vibePlayerLog).noquote() << QStringLiteral("[%1]").arg(area) << safe;
    appendLine(QStringLiteral("info"), area, safe);
}

void warning(const QString& area, const QString& message)
{
    const auto safe = messageSanitizer ? messageSanitizer(message) : message;
    qCWarning(vibePlayerLog).noquote() << QStringLiteral("[%1]").arg(area) << safe;
    appendLine(QStringLiteral("warning"), area, safe);
}

}
