#include "services/encryptedhls/EncryptedHlsBatchPackager.h"

#include "utils/AppLogger.h"

#include <QFileInfo>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <ranges>
#include <utility>

namespace {
// FFmpeg may create codec threads of its own, so bound the outer worker pool
// to keep a user-selected batch from oversubscribing the machine.
constexpr int maximumConcurrentPackagingJobs = 4;
}

EncryptedHlsBatchPackager::EncryptedHlsBatchPackager(TsslStore &store,
                                                      QObject *parent)
    : QObject(parent), m_store(store) {
  ensureWorkerCount(1);
}

int EncryptedHlsBatchPackager::maximumParallelJobs() {
  return std::clamp(QThread::idealThreadCount(), 1,
                    maximumConcurrentPackagingJobs);
}

bool EncryptedHlsBatchPackager::isRunning() const { return m_running; }

double EncryptedHlsBatchPackager::progress() const {
  if (m_result.requestedCount <= 0) {
    return 0.0;
  }
  const auto processed = m_result.packages.size() + m_result.failures.size();
  auto activeProgress = 0.0;
  for (const auto &worker : m_workers) {
    if (worker->active) {
      activeProgress += worker->packager->progress();
    }
  }
  return std::clamp((static_cast<double>(processed) + activeProgress) /
                        static_cast<double>(m_result.requestedCount),
                    0.0, 1.0);
}

QString EncryptedHlsBatchPackager::phase() const { return m_phase; }

QString EncryptedHlsBatchPackager::ffmpegExecutable() const {
  return m_workers.empty() ? QString()
                           : m_workers.front()->packager->ffmpegExecutable();
}

int EncryptedHlsBatchPackager::totalCount() const {
  return m_result.requestedCount;
}

int EncryptedHlsBatchPackager::processedCount() const {
  return m_result.packages.size() + m_result.failures.size();
}

int EncryptedHlsBatchPackager::activeCount() const {
  return static_cast<int>(std::ranges::count_if(
      m_workers, [](const auto &worker) { return worker->active; }));
}

int EncryptedHlsBatchPackager::currentIndex() const {
  const auto worker = std::ranges::find_if(
      m_workers, [](const auto &candidate) { return candidate->active; });
  return worker == m_workers.end() ? 0 : (*worker)->requestIndex + 1;
}

QString EncryptedHlsBatchPackager::currentSourcePath() const {
  const auto worker = std::ranges::find_if(
      m_workers, [](const auto &candidate) { return candidate->active; });
  return worker == m_workers.end() ? QString() : (*worker)->sourcePath;
}

std::expected<void, QString>
EncryptedHlsBatchPackager::start(EncryptedHlsBatchRequest request) {
  if (m_running || std::ranges::any_of(m_workers, [](const auto &worker) {
        return worker->packager->isRunning();
      })) {
    return std::unexpected(
        QStringLiteral("Another M3U8S batch is already being created"));
  }
  if (request.packages.isEmpty()) {
    return std::unexpected(
        QStringLiteral("Choose at least one local video file"));
  }

  m_parallelJobs = std::clamp(request.parallelJobs, 1, maximumParallelJobs());
  ensureWorkerCount(m_parallelJobs);
  ++m_runGeneration;
  m_request = std::move(request);
  m_result = EncryptedHlsBatchResult{
      .requestedCount = static_cast<int>(m_request.packages.size()),
  };
  m_nextRequestIndex = 0;
  m_cancelRequested = false;
  m_running = true;
  setPhase(QStringLiteral("idle"));
  emit runningChanged();
  emit progressChanged();

  AppLogger::info(QStringLiteral("encrypted-hls"),
                  QStringLiteral("Starting an M3U8S batch with %1 source files and %2 parallel jobs")
                      .arg(m_result.requestedCount)
                      .arg(m_parallelJobs));
  dispatchAvailable();
  return {};
}

void EncryptedHlsBatchPackager::cancel() {
  if (!m_running) {
    return;
  }
  m_cancelRequested = true;
  setPhase(QStringLiteral("canceling"));
  for (const auto &worker : m_workers) {
    if (worker->active) {
      worker->packager->cancel();
    }
  }
  if (activeCount() == 0) {
    finishCanceled();
  }
}

void EncryptedHlsBatchPackager::ensureWorkerCount(int count) {
  while (static_cast<int>(m_workers.size()) < count) {
    auto worker = std::make_unique<WorkerSlot>();
    worker->packager = std::make_unique<EncryptedHlsPackager>(m_store);
    connectWorker(*worker);
    m_workers.push_back(std::move(worker));
  }
  while (static_cast<int>(m_workers.size()) > count) {
    m_workers.pop_back();
  }
}

void EncryptedHlsBatchPackager::connectWorker(WorkerSlot &worker) {
  auto *slot = &worker;
  connect(worker.packager.get(), &EncryptedHlsPackager::progressChanged, this,
          [this, slot]() {
            if (slot->active) {
              emit progressChanged();
            }
          });
  connect(worker.packager.get(), &EncryptedHlsPackager::phaseChanged, this,
          [this, slot]() {
            if (slot->active) {
              updateAggregatePhase();
            }
          });
  connect(worker.packager.get(), &EncryptedHlsPackager::completed, this,
          [this, slot](const EncryptedHlsPackageResult &result) {
            if (!m_running || !slot->active) {
              return;
            }
            m_result.packages.append(result);
            m_result.segmentCount += result.segmentCount;
            releaseWorker(*slot);
            emit itemCompleted(result);
            emit currentChanged();
            emit progressChanged();
            updateAggregatePhase();
            scheduleDispatch();
          });
  connect(worker.packager.get(), &EncryptedHlsPackager::failed, this,
          [this, slot](const QString &error) {
            if (!m_running || !slot->active) {
              return;
            }
            recordFailure(*slot, error);
            emit currentChanged();
            updateAggregatePhase();
            scheduleDispatch();
          });
  connect(worker.packager.get(), &EncryptedHlsPackager::canceled, this,
          [this, slot]() {
            if (!m_running || !slot->active) {
              return;
            }
            releaseWorker(*slot);
            m_cancelRequested = true;
            emit currentChanged();
            emit progressChanged();
            updateAggregatePhase();
            scheduleDispatch();
          });
}

EncryptedHlsBatchPackager::WorkerSlot *
EncryptedHlsBatchPackager::availableWorker() {
  const auto worker = std::ranges::find_if(
      m_workers, [](const auto &candidate) { return !candidate->active; });
  return worker == m_workers.end() ? nullptr : worker->get();
}

void EncryptedHlsBatchPackager::scheduleDispatch() {
  const auto generation = m_runGeneration;
  QTimer::singleShot(0, this, [this, generation]() {
    if (generation != m_runGeneration) {
      return;
    }
    dispatchAvailable();
  });
}

void EncryptedHlsBatchPackager::dispatchAvailable() {
  if (!m_running) {
    return;
  }
  if (m_cancelRequested) {
    if (activeCount() == 0) {
      finishCanceled();
    }
    return;
  }

  while (!m_cancelRequested &&
         m_nextRequestIndex < m_request.packages.size()) {
    auto *worker = availableWorker();
    if (!worker) {
      break;
    }

    const auto requestIndex = m_nextRequestIndex++;
    const auto &request = m_request.packages.at(requestIndex);
    worker->requestIndex = requestIndex;
    worker->sourcePath = request.sourcePath;
    worker->active = true;

    if (auto started = worker->packager->start(request); !started) {
      recordFailure(*worker, started.error());
    }
  }

  emit currentChanged();
  emit progressChanged();
  updateAggregatePhase();

  if (m_cancelRequested) {
    if (activeCount() == 0) {
      finishCanceled();
    }
  } else if (m_nextRequestIndex >= m_request.packages.size() &&
             activeCount() == 0) {
    finishCompleted();
  }
}

void EncryptedHlsBatchPackager::releaseWorker(WorkerSlot &worker) {
  worker.active = false;
  worker.requestIndex = -1;
  worker.sourcePath.clear();
}

void EncryptedHlsBatchPackager::recordFailure(WorkerSlot &worker,
                                              QString error) {
  const EncryptedHlsBatchFailure failure{
      .sourcePath = worker.sourcePath,
      .error = std::move(error),
  };
  m_result.failures.append(failure);
  releaseWorker(worker);
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

void EncryptedHlsBatchPackager::updateAggregatePhase() {
  if (!m_running) {
    return;
  }
  if (m_cancelRequested) {
    setPhase(QStringLiteral("canceling"));
    return;
  }
  if (activeCount() > 1) {
    setPhase(QStringLiteral("parallel"));
    return;
  }
  const auto worker = std::ranges::find_if(
      m_workers, [](const auto &candidate) { return candidate->active; });
  setPhase(worker == m_workers.end() ? QStringLiteral("idle")
                                     : (*worker)->packager->phase());
}

void EncryptedHlsBatchPackager::setPhase(QString phase) {
  if (m_phase == phase) {
    return;
  }
  m_phase = std::move(phase);
  emit phaseChanged();
}
