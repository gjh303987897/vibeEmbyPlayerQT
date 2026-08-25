#pragma once

#include "services/webdav/TsslStore.h"
#include "services/encryptedhls/EncryptedHlsTarContainer.h"

#include <QFutureWatcher>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>

class QTemporaryDir;

enum class EncryptedHlsVideoEncoding {
    Copy,
    H264,
    H265,
    Auto,
};

enum class EncryptedHlsAudioEncoding {
    Copy,
    Aac,
};

enum class EncryptedHlsVideoQuality {
    High,
    Balanced,
    Compact,
};

enum class EncryptedHlsContainerFormat {
    DirectoryM3u8s,
    TarM3u8sp,
};

struct EncryptedHlsPackageRequest final {
    QString sourcePath;
    QString outputDirectory;
    int segmentDurationSeconds { 6 };
    EncryptedHlsVideoEncoding videoEncoding { EncryptedHlsVideoEncoding::H264 };
    QStringList autoCopyVideoCodecs { QStringLiteral("h264"), QStringLiteral("h265") };
    EncryptedHlsVideoEncoding autoFallbackVideoEncoding { EncryptedHlsVideoEncoding::H264 };
    EncryptedHlsAudioEncoding audioEncoding { EncryptedHlsAudioEncoding::Aac };
    EncryptedHlsVideoQuality videoQuality { EncryptedHlsVideoQuality::Balanced };
    EncryptedHlsContainerFormat containerFormat { EncryptedHlsContainerFormat::TarM3u8sp };
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

std::expected<QStringList, QString> buildFfmpegArguments(
    const EncryptedHlsPackageRequest& request,
    const QString& segmentPattern,
    const QString& manifestPath);

std::expected<QString, QString> probeVideoCodec(const QString& sourcePath,
                                                const QString& ffmpegExecutable);

std::expected<void, QString> validateGeneratedVideoTrack(
    const QString& directoryPath,
    const QString& ffmpegExecutable,
    std::atomic_bool& cancelRequested);

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
    void handleTarFinished();
    void setProgress(double value);
    void setPhase(QString phase);
    void finishFailure(QString error);
    void finishCanceled();
    void resetRunState();
    QString chooseOutputPath(const QString& outputDirectory,
                             QByteArrayView rootManifestDigest,
                             EncryptedHlsContainerFormat format) const;
    static QString locateFfmpegExecutable();

    TsslStore& m_store;
    QProcess m_ffmpeg;
    QFutureWatcher<std::expected<EncryptedHlsPreparedPackage, QString>> m_encryptionWatcher;
    QFutureWatcher<std::expected<EncryptedHlsTarIndex, QString>> m_tarWatcher;
    EncryptedHlsPreparedPackage m_pendingPreparedPackage;
    std::unique_ptr<QTemporaryDir> m_stagingDirectory;
    std::atomic_bool m_cancelRequested { false };
    QByteArray m_progressBuffer;
    QByteArray m_diagnostics;
    QString m_outputDirectory;
    QString m_sourceFileName;
    QString m_ffmpegExecutable;
    QString m_finalOutputPath;
    EncryptedHlsContainerFormat m_containerFormat { EncryptedHlsContainerFormat::TarM3u8sp };
    qint64 m_durationMicroseconds { 0 };
    double m_progress { 0.0 };
    QString m_phase { QStringLiteral("idle") };
    bool m_running { false };
    bool m_terminalReported { false };
};
