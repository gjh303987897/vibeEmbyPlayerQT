#pragma once

#include "services/encryptedhls/EncryptedHlsPackager.h"

#include <QList>
#include <QObject>
#include <QString>

#include <expected>
#include <memory>
#include <vector>

struct EncryptedHlsBatchRequest final {
  QList<EncryptedHlsPackageRequest> packages;
  int parallelJobs{1};
};

struct EncryptedHlsBatchFailure final {
  QString sourcePath;
  QString error;
};

struct EncryptedHlsBatchResult final {
  int requestedCount{0};
  QList<EncryptedHlsPackageResult> packages;
  QList<EncryptedHlsBatchFailure> failures;
  int segmentCount{0};
};

class EncryptedHlsBatchPackager final : public QObject {
  Q_OBJECT

public:
  explicit EncryptedHlsBatchPackager(TsslStore &store,
                                     QObject *parent = nullptr);

  static int maximumParallelJobs();
  bool isRunning() const;
  double progress() const;
  QString phase() const;
  QString ffmpegExecutable() const;
  int totalCount() const;
  int processedCount() const;
  int activeCount() const;
  int currentIndex() const;
  QString currentSourcePath() const;

  std::expected<void, QString> start(EncryptedHlsBatchRequest request);
  void cancel();

signals:
  void runningChanged();
  void progressChanged();
  void phaseChanged();
  void currentChanged();
  void itemCompleted(const EncryptedHlsPackageResult &result);
  void itemFailed(const EncryptedHlsBatchFailure &failure);
  void completed(const EncryptedHlsBatchResult &result);
  void canceled(const EncryptedHlsBatchResult &result);

private:
  struct WorkerSlot final {
    std::unique_ptr<EncryptedHlsPackager> packager;
    QString sourcePath;
    int requestIndex{-1};
    bool active{false};
  };

  void ensureWorkerCount(int count);
  void connectWorker(WorkerSlot &worker);
  WorkerSlot *availableWorker();
  void scheduleDispatch();
  void dispatchAvailable();
  void releaseWorker(WorkerSlot &worker);
  void recordFailure(WorkerSlot &worker, QString error);
  void finishCompleted();
  void finishCanceled();
  void updateAggregatePhase();
  void setPhase(QString phase);

  TsslStore &m_store;
  std::vector<std::unique_ptr<WorkerSlot>> m_workers;
  EncryptedHlsBatchRequest m_request;
  EncryptedHlsBatchResult m_result;
  QString m_phase{QStringLiteral("idle")};
  int m_nextRequestIndex{0};
  int m_parallelJobs{1};
  quint64 m_runGeneration{0};
  bool m_running{false};
  bool m_cancelRequested{false};
};
