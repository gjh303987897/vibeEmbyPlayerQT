#include "services/encryptedhls/EncryptedHlsSourcePlanner.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace {
constexpr qsizetype maximumSourceVideos = 4096;

QString pathKey(const QString &path) {
  auto key = QDir::cleanPath(path);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
  key = key.toCaseFolded();
#endif
  return key;
}

bool isSameOrChildPath(const QString &candidate, const QString &parent) {
  const auto relative = QDir(parent).relativeFilePath(candidate);
  return relative == QStringLiteral(".") ||
         (relative != QStringLiteral("..") &&
          !relative.startsWith(QStringLiteral("../")) &&
          !QDir::isAbsolutePath(relative));
}

QString canonicalPath(const QFileInfo &info) {
  const auto canonical = info.canonicalFilePath();
  return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                             : canonical);
}

QString uniqueFolderName(const QString &requestedName,
                         QSet<QString> &usedNames) {
  const auto baseName = requestedName.isEmpty()
                            ? QStringLiteral("selected_folder")
                            : requestedName;
  auto candidate = baseName;
  for (int suffix = 2; usedNames.contains(pathKey(candidate)); ++suffix) {
    candidate = baseName + QLatin1Char('_') + QString::number(suffix);
  }
  usedNames.insert(pathKey(candidate));
  return candidate;
}

std::expected<void, QString> appendSource(EncryptedHlsSourcePlan &plan,
                                          QSet<QString> &plannedFiles,
                                          const QFileInfo &fileInfo,
                                          const QString &outputDirectory) {
  if (!fileInfo.exists() || !fileInfo.isFile() || !fileInfo.isReadable() ||
      fileInfo.isSymLink() ||
      !EncryptedHlsSourcePlanner::isSupportedVideoFile(fileInfo.fileName())) {
    return {};
  }
  const auto sourcePath = canonicalPath(fileInfo);
  const auto sourceKey = pathKey(sourcePath);
  if (plannedFiles.contains(sourceKey)) {
    return {};
  }
  if (plan.sources.size() >= maximumSourceVideos) {
    return std::unexpected(QStringLiteral(
        "The selection contains more than 4096 supported videos"));
  }
  plannedFiles.insert(sourceKey);
  plan.sources.append(EncryptedHlsPlannedSource{
      .sourcePath = sourcePath,
      .outputDirectory = QDir::cleanPath(outputDirectory),
  });
  return {};
}
} // namespace

namespace EncryptedHlsSourcePlanner {

bool isSupportedVideoFile(const QString &path) {
  static const QSet<QString> extensions{
      QStringLiteral("mp4"), QStringLiteral("mkv"),  QStringLiteral("mov"),
      QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("m4v"),
      QStringLiteral("ts"),  QStringLiteral("mts"),  QStringLiteral("m2ts"),
  };
  return extensions.contains(QFileInfo(path).suffix().toLower());
}

std::expected<EncryptedHlsSourcePlan, QString>
plan(const QStringList &selectedPaths, const QString &outputRoot,
     std::atomic_bool &cancelRequested) {
  const QFileInfo outputInfo(outputRoot);
  if (!outputInfo.exists() || !outputInfo.isDir() || !outputInfo.isWritable()) {
    return std::unexpected(
        QStringLiteral("Choose a writable M3U8S output directory"));
  }
  const auto normalizedOutputRoot = canonicalPath(outputInfo);

  QList<QString> selectedDirectories;
  QList<QString> selectedFiles;
  QSet<QString> selectedPathKeys;
  for (const auto &selectedPath : selectedPaths) {
    if (cancelRequested.load(std::memory_order_relaxed)) {
      return std::unexpected(QStringLiteral("Source discovery was canceled"));
    }
    const QFileInfo info(selectedPath);
    if (!info.exists() || (!info.isFile() && !info.isDir()) ||
        !info.isReadable() || info.isSymLink()) {
      return std::unexpected(
          QStringLiteral("A selected M3U8S source is unavailable: %1")
              .arg(info.fileName().isEmpty() ? selectedPath : info.fileName()));
    }
    const auto normalized = canonicalPath(info);
    const auto key = pathKey(normalized);
    if (selectedPathKeys.contains(key)) {
      continue;
    }
    selectedPathKeys.insert(key);
    if (info.isDir()) {
      if (isSameOrChildPath(normalized, normalizedOutputRoot)) {
        return std::unexpected(QStringLiteral(
            "A selected source folder is inside the M3U8S output directory"));
      }
      selectedDirectories.append(normalized);
    } else {
      selectedFiles.append(normalized);
    }
  }

  std::ranges::sort(selectedDirectories,
                    [](const QString &left, const QString &right) {
                      if (left.size() != right.size()) {
                        return left.size() < right.size();
                      }
                      return left.compare(right, Qt::CaseInsensitive) < 0;
                    });

  EncryptedHlsSourcePlan result;
  QSet<QString> plannedFiles;
  QSet<QString> usedFolderNames;
  for (const auto &selectedDirectory : selectedDirectories) {
    if (cancelRequested.load(std::memory_order_relaxed)) {
      return std::unexpected(QStringLiteral("Source discovery was canceled"));
    }
    const QFileInfo selectedInfo(selectedDirectory);
    const auto outputFolderName =
        uniqueFolderName(selectedInfo.fileName(), usedFolderNames);
    const auto directoryOutputRoot =
        QDir(normalizedOutputRoot).filePath(outputFolderName);
    QList<QString> pendingDirectories{selectedDirectory};
    while (!pendingDirectories.isEmpty()) {
      if (cancelRequested.load(std::memory_order_relaxed)) {
        return std::unexpected(QStringLiteral("Source discovery was canceled"));
      }
      const auto currentDirectory = pendingDirectories.takeLast();
      if (isSameOrChildPath(currentDirectory, normalizedOutputRoot)) {
        continue;
      }
      const auto relativeDirectory =
          QDir(selectedDirectory).relativeFilePath(currentDirectory);
      const auto currentOutputDirectory =
          relativeDirectory == QStringLiteral(".")
              ? directoryOutputRoot
              : QDir(directoryOutputRoot).filePath(relativeDirectory);
      const QDir directory(currentDirectory);
      const auto files =
          directory.entryInfoList(QDir::Files | QDir::Readable | QDir::Hidden | QDir::System |
                                      QDir::NoSymLinks | QDir::NoDotAndDotDot,
                                  QDir::Name | QDir::IgnoreCase);
      for (const auto &fileInfo : files) {
        if (auto appended = appendSource(result, plannedFiles, fileInfo,
                                         currentOutputDirectory);
            !appended) {
          return std::unexpected(appended.error());
        }
      }
      const auto children = directory.entryInfoList(
          QDir::Dirs | QDir::Readable | QDir::Hidden | QDir::System |
              QDir::NoSymLinks | QDir::NoDotAndDotDot,
          QDir::Name | QDir::IgnoreCase | QDir::Reversed);
      for (const auto &child : children) {
        const auto childPath = canonicalPath(child);
        if (!isSameOrChildPath(childPath, normalizedOutputRoot)) {
          pendingDirectories.append(childPath);
        }
      }
    }
  }

  for (const auto &selectedFile : selectedFiles) {
    if (cancelRequested.load(std::memory_order_relaxed)) {
      return std::unexpected(QStringLiteral("Source discovery was canceled"));
    }
    if (!isSupportedVideoFile(selectedFile)) {
      return std::unexpected(
          QStringLiteral("The selected file is not a supported video: %1")
              .arg(QFileInfo(selectedFile).fileName()));
    }
    if (auto appended =
            appendSource(result, plannedFiles, QFileInfo(selectedFile),
                         normalizedOutputRoot);
        !appended) {
      return std::unexpected(appended.error());
    }
  }

  if (result.sources.isEmpty()) {
    return std::unexpected(QStringLiteral(
        "No supported videos were found in the selected sources"));
  }

  QSet<QString> outputDirectories;
  for (const auto &source : result.sources) {
    outputDirectories.insert(source.outputDirectory);
  }
  for (const auto &directory : outputDirectories) {
    if (cancelRequested.load(std::memory_order_relaxed)) {
      return std::unexpected(QStringLiteral("Source discovery was canceled"));
    }
    if (!QDir().mkpath(directory)) {
      return std::unexpected(
          QStringLiteral("Unable to create an M3U8S output folder: %1")
              .arg(QDir::toNativeSeparators(directory)));
    }
    const QFileInfo info(directory);
    if (!info.isDir() || !info.isWritable()) {
      return std::unexpected(
          QStringLiteral("An M3U8S output folder is not writable: %1")
              .arg(QDir::toNativeSeparators(directory)));
    }
  }
  return result;
}

} // namespace EncryptedHlsSourcePlanner
