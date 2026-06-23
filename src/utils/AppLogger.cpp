#include "utils/AppLogger.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTextStream>

Q_LOGGING_CATEGORY(vibePlayerLog, "vibeplayer")

namespace AppLogger {
namespace {
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

void info(const QString& area, const QString& message)
{
    qCInfo(vibePlayerLog).noquote() << QStringLiteral("[%1]").arg(area) << message;
    appendLine(QStringLiteral("info"), area, message);
}

void warning(const QString& area, const QString& message)
{
    qCWarning(vibePlayerLog).noquote() << QStringLiteral("[%1]").arg(area) << message;
    appendLine(QStringLiteral("warning"), area, message);
}

}
