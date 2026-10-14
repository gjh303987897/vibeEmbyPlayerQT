#pragma once

#include <QString>
#include <QStringList>
#include <QtTypes>

#include <atomic>
#include <expected>
#include <functional>

// Pure file workflow. Callers run this module on a worker and import packages
// only after extract() has validated every entry in the archive.
namespace TsslBackupArchive {
inline constexpr qint64 maxPackageBytes = 64LL * 1024 * 1024;
inline constexpr qint64 maximumArchiveBytes = 2LL * 1024 * 1024 * 1024;
inline constexpr qint64 maximumExpandedBytes = 2LL * 1024 * 1024 * 1024;
inline constexpr int maximumEntries = 10000;

bool isArchiveName(const QString& name);
QString newFileName();
QStringList discover(const QString& directory);

std::expected<int, QString> create(const QStringList& files, const QString& destination,
    const std::atomic_bool* canceled = nullptr,
    const std::function<void(int, int)>& progress = {});
std::expected<QStringList, QString> extract(const QString& archive, const QString& destinationDirectory,
    const std::atomic_bool* canceled = nullptr,
    const std::function<void(int, int)>& progress = {});
}
