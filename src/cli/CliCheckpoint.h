#pragma once
#include "cli/CliConsole.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>

namespace Cli {
inline QString fileHash(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        while (CliConsole::transferPaused.load() && !CliConsole::canceled.load()) QThread::msleep(20);
        if (CliConsole::canceled.load()) return {};
        const auto block = file.read(1024 * 1024);
        if (block.isEmpty()) return {};
        hash.addData(block);
    }
    return QString::fromLatin1(hash.result().toHex());
}
inline QString checkpointFile(const QString& directory, const QString& key)
{
    return QDir(directory).filePath(QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(),QCryptographicHash::Sha256).toHex()) + ".json");
}
inline QJsonObject checkpoint(const QString& directory, const QString& key)
{
    if (directory.isEmpty()) return {};
    QFile file(checkpointFile(directory,key));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}
inline bool saveCheckpoint(const QString& directory, const QString& key, const QJsonObject& value)
{
    if (directory.isEmpty()) return true;
    if (!QDir().mkpath(directory)) return false;
    QSaveFile file(checkpointFile(directory,key));
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
}
