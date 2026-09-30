#pragma once

#include "services/webdav/EncryptedHlsPlaybackProxy.h"

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <expected>
#include <memory>

struct EncryptedHlsRestoreRequest final {
    QStringList sources;
    QString outputDirectory;
    QString format { QStringLiteral("original") }; // original, mkv, mp4
};

// Authenticated streaming decryption followed by streamcopy. No plaintext
// segment tree is created and the playback proxy has an independent lifetime.
class EncryptedHlsRestorer final : public QObject {
    Q_OBJECT
public:
    explicit EncryptedHlsRestorer(TsslStore& store, QObject* parent = nullptr);
    ~EncryptedHlsRestorer() override;
    bool isRunning() const { return m_running; }
    double progress() const;
    QString phase() const { return m_phase; }
    std::expected<void, QString> start(EncryptedHlsRestoreRequest request);
    void cancel();

signals:
    void stateChanged();
    void itemChanged(int index, const QString& state, const QString& outputPath,
                     const QString& error, bool legacyName);
    void finished(bool canceled);

private:
    void scheduleNext();
    void startNext();
    void launch(EncryptedHlsPreparedStream prepared);
    void readProgress();
    void readDiagnostics();
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void failCurrent(QString error);
    void settleCurrent(const QString& state, const QString& error = {});
    void stopWithError(QString error);
    void cleanupCurrent();
    void finishBatch();

    EncryptedHlsPlaybackProxy m_proxy;
    QProcess m_ffmpeg;
    QTimer m_watchdog;
    EncryptedHlsRestoreRequest m_request;
    std::unique_ptr<QTemporaryDir> m_staging;
    QString m_executable;
    QString m_sessionId;
    QString m_outputPath;
    QString m_stagingPath;
    QString m_pendingError;
    QByteArray m_progressBuffer;
    QByteArray m_diagnostics;
    qint64 m_durationUs { 0 };
    qint64 m_frames { 0 };
    quint64 m_generation { 0 };
    int m_index { -1 };
    double m_itemProgress { 0 };
    QString m_phase { QStringLiteral("idle") };
    bool m_running { false };
    bool m_cancelRequested { false };
    bool m_legacyName { false };
    bool m_itemActive { false };
};
