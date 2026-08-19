#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>
#include <expected>

struct EncryptedHlsPlannedSource final {
  QString sourcePath;
  QString outputDirectory;
};

struct EncryptedHlsSourcePlan final {
  QList<EncryptedHlsPlannedSource> sources;
};

namespace EncryptedHlsSourcePlanner {

std::expected<EncryptedHlsSourcePlan, QString>
plan(const QStringList &selectedPaths, const QString &outputRoot,
     std::atomic_bool &cancelRequested);

bool isSupportedVideoFile(const QString &path);

} // namespace EncryptedHlsSourcePlanner
