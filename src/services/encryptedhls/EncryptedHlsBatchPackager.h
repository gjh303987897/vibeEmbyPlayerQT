#pragma once

#include "services/encryptedhls/EncryptedHlsPackager.h"

#include <QList>
#include <QObject>
#include <QString>

#include <expected>

struct EncryptedHlsBatchRequest final {
  QList<EncryptedHlsPackageRequest> packages;
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

  bool isRunning() const;
  double progress() const;
  QString phase() const;
  QString ffmpegExecutable() const;
  int totalCount() const;
  int processedCount() const;
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
  void startNext();
  void recordFailure(QString error);
  void finishCompleted();
  void finishCanceled();
  void setPhase(QString phase);

  EncryptedHlsPackager m_packager;
  EncryptedHlsBatchRequest m_request;
  EncryptedHlsBatchResult m_result;
  QString m_currentSourcePath;
  QString m_phase{QStringLiteral("idle")};
  int m_nextRequestIndex{0};
  int m_currentIndex{0};
  double m_currentProgress{0.0};
  bool m_running{false};
  bool m_cancelRequested{false};
};
