#pragma once

#include "services/webdav/TsslStore.h"

#include <QFutureWatcher>
#include <QObject>
#include <QProcess>
#include <QString>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>

class QTemporaryDir;

struct EncryptedHlsPackageRequest final {
    QString sourcePath;
    QString outputDirectory;
    int segmentDurationSeconds { 6 };
};

struct EncryptedHlsPreparedPackage final {
    TsslPackage tsslPackage;
    QString manifestFileName;
    int segmentCount { 0 };
};

struct EncryptedHlsPackageResult final {
    QString outputDirectory;
    QString manifestPath;
    QByteArray identifier;
    QByteArray rootManifestDigest;
    int segmentCount { 0 };
};

namespace EncryptedHlsPackaging {

std::expected<EncryptedHlsPreparedPackage, QString> encryptHlsDirectory(
    const QString& directoryPath,
    const QString& sourceFileName,
    std::atomic_bool& cancelRequested,
    const std::function<void(double)>& progressCallback = {});

}

class EncryptedHlsPackager final : public QObject {
    Q_OBJECT

public:
    explicit EncryptedHlsPackager(TsslStore& store, QObject* parent = nullptr);
    ~EncryptedHlsPackager() override;

    bool isRunning() const;
    double progress() const;
    QString phase() const;
    QString ffmpegExecutable() const;

    std::expected<void, QString> start(const EncryptedHlsPackageRequest& request);
    void cancel();

signals:
    void runningChanged();
    void progressChanged();
    void phaseChanged();
    void completed(const EncryptedHlsPackageResult& result);
    void failed(const QString& error);
    void canceled();

private:
    void readFfmpegProgress();
    void readFfmpegDiagnostics();
    void handleFfmpegFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void beginEncryption();
    void handleEncryptionFinished();
    void setProgress(double value);
    void setPhase(QString phase);
    void finishFailure(QString error);
    void finishCanceled();
    void resetRunState();
    QString chooseOutputPath(const QString& outputDirectory, QByteArrayView rootManifestDigest) const;
    static QString locateFfmpegExecutable();

    TsslStore& m_store;
    QProcess m_ffmpeg;
    QFutureWatcher<std::expected<EncryptedHlsPreparedPackage, QString>> m_encryptionWatcher;
    std::unique_ptr<QTemporaryDir> m_stagingDirectory;
    std::atomic_bool m_cancelRequested { false };
    QByteArray m_progressBuffer;
    QByteArray m_diagnostics;
    QString m_outputDirectory;
    QString m_sourceFileName;
    QString m_finalOutputPath;
    qint64 m_durationMicroseconds { 0 };
    double m_progress { 0.0 };
    QString m_phase { QStringLiteral("idle") };
    bool m_running { false };
    bool m_terminalReported { false };
};
