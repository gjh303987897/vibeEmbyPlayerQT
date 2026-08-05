#include "services/encryptedhls/EncryptedHlsBatchPackager.h"

#include "utils/AppLogger.h"

#include <QFileInfo>
#include <QTimer>

#include <algorithm>
#include <utility>

EncryptedHlsBatchPackager::EncryptedHlsBatchPackager(TsslStore &store,
                                                     QObject *parent)
    : QObject(parent), m_packager(store, this) {
  connect(&m_packager, &EncryptedHlsPackager::progressChanged, this, [this]() {
    m_currentProgress = m_packager.progress();
    emit progressChanged();
  });
  connect(&m_packager, &EncryptedHlsPackager::phaseChanged, this,
          [this]() { setPhase(m_packager.phase()); });
  connect(&m_packager, &EncryptedHlsPackager::completed, this,
          [this](const EncryptedHlsPackageResult &result) {
            if (!m_running) {
              return;
            }
            m_result.packages.append(result);
            m_result.segmentCount += result.segmentCount;
            m_currentProgress = 0.0;
            emit itemCompleted(result);
            emit progressChanged();
            QTimer::singleShot(0, this, &EncryptedHlsBatchPackager::startNext);
          });
  connect(&m_packager, &EncryptedHlsPackager::failed, this,
          [this](const QString &error) {
            if (!m_running) {
              return;
            }
            recordFailure(error);
            QTimer::singleShot(0, this, &EncryptedHlsBatchPackager::startNext);
          });
  connect(&m_packager, &EncryptedHlsPackager::canceled, this, [this]() {
    if (m_running) {
      finishCanceled();
    }
  });
}

bool EncryptedHlsBatchPackager::isRunning() const { return m_running; }

double EncryptedHlsBatchPackager::progress() const {
  if (m_result.requestedCount <= 0) {
    return 0.0;
  }
  const auto processed = m_result.packages.size() + m_result.failures.size();
  return std::clamp((static_cast<double>(processed) + m_currentProgress) /
                        static_cast<double>(m_result.requestedCount),
                    0.0, 1.0);
}

QString EncryptedHlsBatchPackager::phase() const { return m_phase; }

QString EncryptedHlsBatchPackager::ffmpegExecutable() const {
  return m_packager.ffmpegExecutable();
}

int EncryptedHlsBatchPackager::totalCount() const {
  return m_result.requestedCount;
}

int EncryptedHlsBatchPackager::processedCount() const {
  return m_result.packages.size() + m_result.failures.size();
}

int EncryptedHlsBatchPackager::currentIndex() const { return m_currentIndex; }

QString EncryptedHlsBatchPackager::currentSourcePath() const {
  return m_currentSourcePath;
}

std::expected<void, QString>
EncryptedHlsBatchPackager::start(EncryptedHlsBatchRequest request) {
  if (m_running || m_packager.isRunning()) {
    return std::unexpected(
        QStringLiteral("Another M3U8S batch is already being created"));
  }
  if (request.packages.isEmpty()) {
    return std::unexpected(
        QStringLiteral("Choose at least one local video file"));
  }

  m_request = std::move(request);
  m_result = EncryptedHlsBatchResult{
      .requestedCount = static_cast<int>(m_request.packages.size()),
  };
  m_currentSourcePath.clear();
  m_nextRequestIndex = 0;
  m_currentIndex = 0;
  m_currentProgress = 0.0;
  m_cancelRequested = false;
  m_running = true;
  setPhase(QStringLiteral("idle"));
  emit runningChanged();
  emit progressChanged();

  AppLogger::info(QStringLiteral("encrypted-hls"),
                  QStringLiteral("Starting an M3U8S batch with %1 source files")
                      .arg(m_result.requestedCount));
  startNext();
  return {};
}

void EncryptedHlsBatchPackager::cancel() {
  if (!m_running) {
    return;
  }
  m_cancelRequested = true;
  setPhase(QStringLiteral("canceling"));
  if (m_packager.isRunning()) {
    m_packager.cancel();
  } else {
    finishCanceled();
  }
}

void EncryptedHlsBatchPackager::startNext() {
  if (!m_running) {
    return;
  }
  if (m_cancelRequested) {
    finishCanceled();
    return;
  }
  if (m_nextRequestIndex >= m_request.packages.size()) {
    finishCompleted();
    return;
  }

  const auto &request = m_request.packages.at(m_nextRequestIndex);
  m_currentIndex = m_nextRequestIndex + 1;
  ++m_nextRequestIndex;
  m_currentSourcePath = request.sourcePath;
  m_currentProgress = 0.0;
  emit currentChanged();
  emit progressChanged();

  if (auto started = m_packager.start(request); !started) {
    recordFailure(started.error());
    QTimer::singleShot(0, this, &EncryptedHlsBatchPackager::startNext);
  }
}

void EncryptedHlsBatchPackager::recordFailure(QString error) {
  const EncryptedHlsBatchFailure failure{
      .sourcePath = m_currentSourcePath,
      .error = std::move(error),
  };
  m_result.failures.append(failure);
  m_currentProgress = 0.0;
  AppLogger::warning(
      QStringLiteral("encrypted-hls"),
      QStringLiteral("M3U8S batch item failed (%1): %2")
          .arg(QFileInfo(failure.sourcePath).fileName(), failure.error));
  emit itemFailed(failure);
  emit progressChanged();
}

void EncryptedHlsBatchPackager::finishCompleted() {
  if (!m_running) {
    return;
  }
  m_running = false;
  m_currentSourcePath.clear();
  m_currentIndex = 0;
  m_currentProgress = 0.0;
  setPhase(QStringLiteral("completed"));
  emit currentChanged();
  emit progressChanged();
  emit runningChanged();
  AppLogger::info(
      QStringLiteral("encrypted-hls"),
      QStringLiteral("Finished an M3U8S batch: %1 succeeded, %2 failed")
          .arg(m_result.packages.size())
          .arg(m_result.failures.size()));
  emit completed(m_result);
}

void EncryptedHlsBatchPackager::finishCanceled() {
  if (!m_running) {
    return;
  }
  m_running = false;
  m_currentSourcePath.clear();
  m_currentIndex = 0;
  m_currentProgress = 0.0;
  setPhase(QStringLiteral("canceled"));
  emit currentChanged();
  emit progressChanged();
  emit runningChanged();
  AppLogger::info(
      QStringLiteral("encrypted-hls"),
      QStringLiteral("Canceled an M3U8S batch after %1 of %2 source files")
          .arg(processedCount())
          .arg(totalCount()));
  emit canceled(m_result);
}

void EncryptedHlsBatchPackager::setPhase(QString phase) {
  if (m_phase == phase) {
    return;
  }
  m_phase = std::move(phase);
  emit phaseChanged();
}
