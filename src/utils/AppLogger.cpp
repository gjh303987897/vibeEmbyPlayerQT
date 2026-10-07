#include "utils/AppLogger.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTextStream>
#include <QFileInfo>
#include <mutex>

Q_LOGGING_CATEGORY(vibePlayerLog, "vibeplayer")

namespace AppLogger {
namespace {
std::function<QString(const QString&)> messageSanitizer;
std::mutex sanitizerMutex;
std::mutex fileMutex;
QString sanitize(const QString& message)
{
    std::function<QString(const QString&)> sanitizer;
    { std::lock_guard lock(sanitizerMutex); sanitizer = messageSanitizer; }
    return sanitizer ? sanitizer(message) : message;
}
void appendLine(const QString& level, const QString& area, const QString& message)
{
    const auto logPath = qEnvironmentVariable("VIBEPLAYER_LOG_FILE");
    if (logPath.isEmpty()) {
        return;
    }

    std::lock_guard lock(fileMutex);
    constexpr qint64 maxLogBytes = 10 * 1024 * 1024;
    const auto path = QFileInfo(logPath).absoluteFilePath();
    auto line = (level + QStringLiteral(" [%1] ").arg(area) + message).toUtf8();
    if (line.size() > 64 * 1024) {
        qsizetype end = 64 * 1024;
        while (end > 0 && (static_cast<unsigned char>(line.at(end)) & 0xc0) == 0x80) --end;
        line.truncate(end);
    }
    line += '\n';
    if (QFileInfo(path).size() + line.size() > maxLogBytes) {
        const auto archive = [&](int index) { return path + QStringLiteral(".%1").arg(index); };
        const auto rotationFailed = [] { qCWarning(vibePlayerLog) << "Unable to rotate the application log; this file entry was not saved"; };
        if (QFile::exists(archive(3)) && !QFile::remove(archive(3))) { rotationFailed(); return; }
        for (int i = 2; i >= 1; --i) {
            if (QFile::exists(archive(i)) && !QFile::rename(archive(i), archive(i + 1))) { rotationFailed(); return; }
        }
        if (QFile::exists(path) && !QFile::rename(path, archive(1))) { rotationFailed(); return; }
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qCWarning(vibePlayerLog) << "Unable to open the application log:" << file.errorString();
        return;
    }

    if (file.write(line) != line.size())
        qCWarning(vibePlayerLog) << "Unable to write the application log:" << file.errorString();
}
}

void setSanitizer(std::function<QString(const QString&)> sanitizer)
{
    std::lock_guard lock(sanitizerMutex);
    messageSanitizer = std::move(sanitizer);
}

void info(const QString& area, const QString& message)
{
    const auto safe = sanitize(message);
    qCInfo(vibePlayerLog).noquote() << QStringLiteral("[%1]").arg(area) << safe;
    appendLine(QStringLiteral("info"), area, safe);
}

void warning(const QString& area, const QString& message)
{
    const auto safe = sanitize(message);
    qCWarning(vibePlayerLog).noquote() << QStringLiteral("[%1]").arg(area) << safe;
    appendLine(QStringLiteral("warning"), area, safe);
}

}
