#include "services/encryptedhls/EncryptedHlsPackager.h"

#include "services/webdav/AesGcmDecryptor.h"
#include "services/webdav/HlsManifestValidator.h"
#include "utils/AppLogger.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>

namespace {
constexpr qint64 maximumPlainSegmentBytes = 512LL * 1024 * 1024;

std::expected<QByteArray, QString> readBoundedFile(const QString& path, qint64 maximumBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Unable to open generated HLS file: %1").arg(file.errorString()));
    }
    if (file.size() <= 0 || file.size() > maximumBytes) {
        return std::unexpected(QStringLiteral("Generated HLS file is empty or exceeds its size limit: %1")
                                   .arg(QFileInfo(path).fileName()));
    }
    return file.readAll();
}

std::expected<void, QString> writeFileAtomically(const QString& path,
                                                 QByteArrayView bytes,
                                                 QFileDevice::Permissions permissions = {})
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return std::unexpected(QStringLiteral("Unable to create encrypted HLS file: %1").arg(file.errorString()));
    }
    if (permissions != QFileDevice::Permissions {}) {
        file.setPermissions(permissions);
    }
    if (file.write(bytes.data(), bytes.size()) != bytes.size()) {
        return std::unexpected(QStringLiteral("Unable to write encrypted HLS file: %1").arg(file.errorString()));
    }
    if (!file.commit()) {
        return std::unexpected(QStringLiteral("Unable to commit encrypted HLS file: %1").arg(file.errorString()));
    }
    if (permissions != QFileDevice::Permissions {}) {
        QFile::setPermissions(path, permissions);
    }
    return {};
}

QString sanitizedStem(const QString& sourcePath)
{
    auto stem = QFileInfo(sourcePath).completeBaseName().trimmed();
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    stem = stem.left(80);
    while (stem.startsWith(QLatin1Char('.')) || stem.endsWith(QLatin1Char('.'))) {
        if (stem.startsWith(QLatin1Char('.'))) {
            stem.remove(0, 1);
        }
        if (stem.endsWith(QLatin1Char('.'))) {
            stem.chop(1);
        }
    }
    return stem.isEmpty() ? QStringLiteral("video") : stem;
}

QByteArray identifierFromRandomBytes(QByteArrayView randomBytes)
{
    return QByteArray(randomBytes.data(), randomBytes.size())
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}
}

namespace EncryptedHlsPackaging {

std::expected<EncryptedHlsPreparedPackage, QString> encryptHlsDirectory(
    const QString& directoryPath,
    const QString& outputStem,
    std::atomic_bool& cancelRequested,
    const std::function<void(double)>& progressCallback)
{
    const QDir directory(directoryPath);
    const auto segmentFiles = directory.entryInfoList({ QStringLiteral("segment_*.ts") },
                                                       QDir::Files | QDir::Readable,
                                                       QDir::Name);
    if (segmentFiles.isEmpty()) {
        return std::unexpected(QStringLiteral("FFmpeg did not produce any TS segments"));
    }

    auto randomIdentifierBytes = AesGcmDecryptor::secureRandomBytes(3072);
    if (!randomIdentifierBytes) {
        return std::unexpected(randomIdentifierBytes.error());
    }
    auto identifier = identifierFromRandomBytes(*randomIdentifierBytes);
    randomIdentifierBytes->fill('\0');
    if (identifier.size() != TsslPackage::identifierLength) {
        identifier.fill('\0');
        return std::unexpected(QStringLiteral("Unable to generate a 4096-character M3U8S identifier"));
    }

    QHash<QString, QByteArray> segmentKeys;
    segmentKeys.reserve(segmentFiles.size());
    for (qsizetype index = 0; index < segmentFiles.size(); ++index) {
        if (cancelRequested.load(std::memory_order_relaxed)) {
            identifier.fill('\0');
            return std::unexpected(QStringLiteral("Packaging was canceled"));
        }
        const auto& segmentInfo = segmentFiles.at(index);
        auto plaintext = readBoundedFile(segmentInfo.absoluteFilePath(), maximumPlainSegmentBytes);
        if (!plaintext) {
            identifier.fill('\0');
            return std::unexpected(plaintext.error());
        }

        auto encrypted = AesGcmDecryptor::encryptTsSegment(*plaintext);
        if (!encrypted) {
            plaintext->fill('\0');
            identifier.fill('\0');
            return std::unexpected(encrypted.error());
        }

        auto verified = AesGcmDecryptor::decryptTsSegment(encrypted->bytes, encrypted->key);
        if (!verified || *verified != *plaintext) {
            plaintext->fill('\0');
            if (verified) {
                verified->fill('\0');
            }
            encrypted->key.fill('\0');
            encrypted->bytes.fill('\0');
            identifier.fill('\0');
            return std::unexpected(QStringLiteral("GCM authentication verification failed for %1")
                                       .arg(segmentInfo.fileName()));
        }
        verified->fill('\0');
        plaintext->fill('\0');

        if (auto written = writeFileAtomically(segmentInfo.absoluteFilePath(), encrypted->bytes); !written) {
            encrypted->key.fill('\0');
            encrypted->bytes.fill('\0');
            identifier.fill('\0');
            return std::unexpected(written.error());
        }
        encrypted->bytes.fill('\0');
        segmentKeys.insert(segmentInfo.fileName(), std::move(encrypted->key));
        if (progressCallback) {
            progressCallback(static_cast<double>(index + 1) / static_cast<double>(segmentFiles.size()));
        }
    }

    const auto sourceManifestPath = directory.filePath(QStringLiteral("index.m3u8"));
    auto plainManifest = readBoundedFile(sourceManifestPath, 4 * 1024 * 1024);
    if (!plainManifest) {
        identifier.fill('\0');
        return std::unexpected(plainManifest.error());
    }
    auto manifest = HlsManifestValidator::insertM3u8sIdentifier(*plainManifest, identifier);
    if (!manifest) {
        identifier.fill('\0');
        return std::unexpected(manifest.error());
    }
    if (auto validated = HlsManifestValidator::validate(*manifest); !validated) {
        identifier.fill('\0');
        return std::unexpected(validated.error());
    }

    const auto manifestFileName = outputStem + QStringLiteral(".m3u8s");
    const auto manifestPath = directory.filePath(manifestFileName);
    if (auto written = writeFileAtomically(manifestPath, *manifest); !written) {
        identifier.fill('\0');
        return std::unexpected(written.error());
    }
    if (!QFile::remove(sourceManifestPath)) {
        identifier.fill('\0');
        return std::unexpected(QStringLiteral("Unable to remove the plaintext HLS manifest"));
    }

    TsslPackage package {
        .identifier = std::move(identifier),
        .rootManifestDigest = QCryptographicHash::hash(*manifest, QCryptographicHash::Sha256),
        .segmentKeys = std::move(segmentKeys),
    };
    return EncryptedHlsPreparedPackage {
        .tsslPackage = std::move(package),
        .manifestFileName = manifestFileName,
        .tsslFileName = outputStem + QStringLiteral(".tssl"),
        .segmentCount = static_cast<int>(segmentFiles.size()),
    };
}

}

EncryptedHlsPackager::EncryptedHlsPackager(TsslStore& store, QObject* parent)
    : QObject(parent)
    , m_store(store)
{
    m_ffmpeg.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_ffmpeg, &QProcess::readyReadStandardOutput, this, &EncryptedHlsPackager::readFfmpegProgress);
    connect(&m_ffmpeg, &QProcess::readyReadStandardError, this, &EncryptedHlsPackager::readFfmpegDiagnostics);
    connect(&m_ffmpeg,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            &EncryptedHlsPackager::handleFfmpegFinished);
    connect(&m_ffmpeg, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_running && !m_terminalReported) {
            finishFailure(QStringLiteral("Unable to start FFmpeg: %1").arg(m_ffmpeg.errorString()));
        }
    });
    connect(&m_encryptionWatcher,
            &QFutureWatcher<std::expected<EncryptedHlsPreparedPackage, QString>>::finished,
            this,
            &EncryptedHlsPackager::handleEncryptionFinished);
}

EncryptedHlsPackager::~EncryptedHlsPackager()
{
    m_cancelRequested.store(true, std::memory_order_relaxed);
    if (m_ffmpeg.state() != QProcess::NotRunning) {
        m_ffmpeg.kill();
        m_ffmpeg.waitForFinished(3000);
    }
    if (m_encryptionWatcher.isRunning()) {
        m_encryptionWatcher.waitForFinished();
    }
}

bool EncryptedHlsPackager::isRunning() const
{
    return m_running;
}

double EncryptedHlsPackager::progress() const
{
    return m_progress;
}

QString EncryptedHlsPackager::phase() const
{
    return m_phase;
}

QString EncryptedHlsPackager::ffmpegExecutable() const
{
    return locateFfmpegExecutable();
}

std::expected<void, QString> EncryptedHlsPackager::start(const EncryptedHlsPackageRequest& request)
{
    if (m_running) {
        return std::unexpected(QStringLiteral("Another M3U8S package is already being created"));
    }
    const QFileInfo source(request.sourcePath);
    if (!source.exists() || !source.isFile() || !source.isReadable()) {
        return std::unexpected(QStringLiteral("Choose a readable local video file"));
    }
    const QFileInfo output(request.outputDirectory);
    if (!output.exists() || !output.isDir() || !output.isWritable()) {
        return std::unexpected(QStringLiteral("Choose a writable output directory"));
    }
    if (request.segmentDurationSeconds < 2 || request.segmentDurationSeconds > 30) {
        return std::unexpected(QStringLiteral("HLS segment duration must be between 2 and 30 seconds"));
    }
    const auto ffmpeg = locateFfmpegExecutable();
    if (ffmpeg.isEmpty()) {
        return std::unexpected(QStringLiteral("FFmpeg was not found. Install FFmpeg or place it next to the application"));
    }

    m_outputDirectory = output.absoluteFilePath();
    m_outputStem = sanitizedStem(source.absoluteFilePath());
    m_finalOutputPath = chooseOutputPath(m_outputDirectory, m_outputStem);
    m_stagingDirectory = std::make_unique<QTemporaryDir>(
        QDir(m_outputDirectory).filePath(QStringLiteral(".m3u8s-staging-XXXXXX")));
    if (!m_stagingDirectory->isValid()) {
        resetRunState();
        return std::unexpected(QStringLiteral("Unable to create a temporary packaging directory"));
    }

    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_terminalReported = false;
    m_durationMicroseconds = 0;
    m_progressBuffer.clear();
    m_diagnostics.clear();
    m_running = true;
    emit runningChanged();
    setProgress(0.01);
    setPhase(QStringLiteral("segmenting"));

    const auto segmentPattern = QDir(m_stagingDirectory->path()).filePath(QStringLiteral("segment_%06d.ts"));
    const auto manifestPath = QDir(m_stagingDirectory->path()).filePath(QStringLiteral("index.m3u8"));
    const auto duration = QString::number(request.segmentDurationSeconds);
    const QStringList arguments {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-y"),
        QStringLiteral("-i"), source.absoluteFilePath(),
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a:0?"),
        QStringLiteral("-sn"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"),
        QStringLiteral("-preset"), QStringLiteral("veryfast"),
        QStringLiteral("-crf"), QStringLiteral("20"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-flags"), QStringLiteral("+cgop"),
        QStringLiteral("-sc_threshold"), QStringLiteral("0"),
        QStringLiteral("-force_key_frames"), QStringLiteral("expr:gte(t,n_forced*") + duration + QLatin1Char(')'),
        QStringLiteral("-c:a"), QStringLiteral("aac"),
        QStringLiteral("-b:a"), QStringLiteral("192k"),
        QStringLiteral("-f"), QStringLiteral("hls"),
        QStringLiteral("-hls_time"), duration,
        QStringLiteral("-hls_playlist_type"), QStringLiteral("vod"),
        QStringLiteral("-hls_flags"), QStringLiteral("independent_segments+temp_file"),
        QStringLiteral("-hls_segment_filename"), segmentPattern,
        QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        QStringLiteral("-nostats"),
        manifestPath,
    };

    AppLogger::info(QStringLiteral("encrypted-hls"),
                    QStringLiteral("Starting local video segmentation for an M3U8S package"));
    m_ffmpeg.start(ffmpeg, arguments, QIODevice::ReadOnly);
    return {};
}

void EncryptedHlsPackager::cancel()
{
    if (!m_running) {
        return;
    }
    m_cancelRequested.store(true, std::memory_order_relaxed);
    setPhase(QStringLiteral("canceling"));
    if (m_ffmpeg.state() != QProcess::NotRunning) {
        m_ffmpeg.terminate();
        QTimer::singleShot(2000, this, [this]() {
            if (m_ffmpeg.state() != QProcess::NotRunning) {
                m_ffmpeg.kill();
            }
        });
    }
}

void EncryptedHlsPackager::readFfmpegProgress()
{
    m_progressBuffer.append(m_ffmpeg.readAllStandardOutput());
    qsizetype newline = -1;
    while ((newline = m_progressBuffer.indexOf('\n')) >= 0) {
        auto line = m_progressBuffer.first(newline).trimmed();
        m_progressBuffer.remove(0, newline + 1);
        if (!line.startsWith("out_time_us=")) {
            continue;
        }
        bool ok = false;
        const auto elapsed = line.sliced(12).toLongLong(&ok);
        if (ok && m_durationMicroseconds > 0) {
            setProgress(std::clamp(0.02 + 0.63 * static_cast<double>(elapsed) /
                                      static_cast<double>(m_durationMicroseconds),
                                  0.02,
                                  0.65));
        }
    }
}

void EncryptedHlsPackager::readFfmpegDiagnostics()
{
    m_diagnostics.append(m_ffmpeg.readAllStandardError());
    if (m_diagnostics.size() > 256 * 1024) {
        m_diagnostics = m_diagnostics.last(256 * 1024);
    }
    if (m_durationMicroseconds > 0) {
        return;
    }
    static const QRegularExpression durationPattern(
        QStringLiteral("Duration:\\s*(\\d{2}):(\\d{2}):(\\d{2}(?:\\.\\d+)?)"));
    const auto match = durationPattern.match(QString::fromUtf8(m_diagnostics));
    if (!match.hasMatch()) {
        return;
    }
    const auto hours = match.captured(1).toLongLong();
    const auto minutes = match.captured(2).toLongLong();
    const auto seconds = match.captured(3).toDouble();
    m_durationMicroseconds = static_cast<qint64>(((hours * 60 + minutes) * 60 + seconds) * 1'000'000.0);
}

void EncryptedHlsPackager::handleFfmpegFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    readFfmpegProgress();
    readFfmpegDiagnostics();
    if (!m_running || m_terminalReported) {
        return;
    }
    if (m_cancelRequested.load(std::memory_order_relaxed)) {
        finishCanceled();
        return;
    }
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        auto detail = QString::fromUtf8(m_diagnostics.last(4000)).trimmed();
        finishFailure(detail.isEmpty()
                          ? QStringLiteral("FFmpeg failed to create HLS segments")
                          : QStringLiteral("FFmpeg failed: %1").arg(detail));
        return;
    }
    beginEncryption();
}

void EncryptedHlsPackager::beginEncryption()
{
    setProgress(0.65);
    setPhase(QStringLiteral("encrypting"));
    const auto directoryPath = m_stagingDirectory->path();
    const auto outputStem = m_outputStem;
    QPointer<EncryptedHlsPackager> owner(this);
    auto progressCallback = [owner](double value) {
        if (!owner) {
            return;
        }
        QMetaObject::invokeMethod(owner, [owner, value]() {
            if (owner) {
                owner->setProgress(0.65 + std::clamp(value, 0.0, 1.0) * 0.30);
            }
        });
    };
    m_encryptionWatcher.setFuture(QtConcurrent::run(
        [directoryPath, outputStem, this, progressCallback = std::move(progressCallback)]() {
            return EncryptedHlsPackaging::encryptHlsDirectory(directoryPath,
                                                              outputStem,
                                                              m_cancelRequested,
                                                              progressCallback);
        }));
}

void EncryptedHlsPackager::handleEncryptionFinished()
{
    if (!m_running || m_terminalReported) {
        return;
    }
    auto prepared = m_encryptionWatcher.result();
    if (!prepared) {
        if (m_cancelRequested.load(std::memory_order_relaxed)) {
            finishCanceled();
        } else {
            finishFailure(prepared.error());
        }
        return;
    }

    setPhase(QStringLiteral("finalizing"));
    setProgress(0.96);
    if (auto saved = m_store.savePackage(prepared->tsslPackage); !saved) {
        finishFailure(saved.error());
        return;
    }
    const auto stagedTsslPath = QDir(m_stagingDirectory->path()).filePath(prepared->tsslFileName);
    if (auto exported = m_store.exportByRootDigest(prepared->tsslPackage.rootManifestDigest, stagedTsslPath);
        !exported) {
        m_store.deleteByRootDigest(prepared->tsslPackage.rootManifestDigest);
        finishFailure(exported.error());
        return;
    }
    if (QFileInfo::exists(m_finalOutputPath) || !QDir().rename(m_stagingDirectory->path(), m_finalOutputPath)) {
        m_store.deleteByRootDigest(prepared->tsslPackage.rootManifestDigest);
        finishFailure(QStringLiteral("Unable to move the completed M3U8S package into the output directory"));
        return;
    }

    const EncryptedHlsPackageResult result {
        .outputDirectory = m_finalOutputPath,
        .manifestPath = QDir(m_finalOutputPath).filePath(prepared->manifestFileName),
        .tsslPath = QDir(m_finalOutputPath).filePath(prepared->tsslFileName),
        .identifier = prepared->tsslPackage.identifier,
        .rootManifestDigest = prepared->tsslPackage.rootManifestDigest,
        .segmentCount = prepared->segmentCount,
    };
    m_stagingDirectory.reset();
    m_terminalReported = true;
    m_running = false;
    setProgress(1.0);
    setPhase(QStringLiteral("completed"));
    emit runningChanged();
    AppLogger::info(QStringLiteral("encrypted-hls"),
                    QStringLiteral("Created an M3U8S package with %1 encrypted segments")
                        .arg(result.segmentCount));
    emit completed(result);
}

void EncryptedHlsPackager::setProgress(double value)
{
    const auto normalized = std::clamp(value, 0.0, 1.0);
    if (qFuzzyCompare(m_progress, normalized)) {
        return;
    }
    m_progress = normalized;
    emit progressChanged();
}

void EncryptedHlsPackager::setPhase(QString phase)
{
    if (m_phase == phase) {
        return;
    }
    m_phase = std::move(phase);
    emit phaseChanged();
}

void EncryptedHlsPackager::finishFailure(QString error)
{
    if (m_terminalReported) {
        return;
    }
    m_terminalReported = true;
    m_running = false;
    m_stagingDirectory.reset();
    setPhase(QStringLiteral("failed"));
    emit runningChanged();
    AppLogger::warning(QStringLiteral("encrypted-hls"), QStringLiteral("M3U8S packaging failed: %1").arg(error));
    emit failed(error);
}

void EncryptedHlsPackager::finishCanceled()
{
    if (m_terminalReported) {
        return;
    }
    m_terminalReported = true;
    m_running = false;
    m_stagingDirectory.reset();
    setPhase(QStringLiteral("canceled"));
    emit runningChanged();
    AppLogger::info(QStringLiteral("encrypted-hls"), QStringLiteral("Canceled M3U8S packaging"));
    emit canceled();
}

void EncryptedHlsPackager::resetRunState()
{
    m_stagingDirectory.reset();
    m_outputDirectory.clear();
    m_outputStem.clear();
    m_finalOutputPath.clear();
    m_progressBuffer.clear();
    m_diagnostics.clear();
}

QString EncryptedHlsPackager::chooseOutputPath(const QString& outputDirectory, const QString& stem) const
{
    const auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const auto baseName = stem + QStringLiteral("_m3u8s_") + timestamp;
    auto candidate = QDir(outputDirectory).filePath(baseName);
    for (int suffix = 2; QFileInfo::exists(candidate); ++suffix) {
        candidate = QDir(outputDirectory).filePath(baseName + QLatin1Char('_') + QString::number(suffix));
    }
    return candidate;
}

QString EncryptedHlsPackager::locateFfmpegExecutable()
{
#if defined(Q_OS_WIN)
    constexpr auto executableName = "ffmpeg.exe";
#else
    constexpr auto executableName = "ffmpeg";
#endif
    const auto bundled = QDir(QCoreApplication::applicationDirPath()).filePath(QLatin1String(executableName));
    if (QFileInfo(bundled).isExecutable()) {
        return QFileInfo(bundled).absoluteFilePath();
    }
    const auto located = QStandardPaths::findExecutable(QLatin1String(executableName));
    return located.isEmpty() ? QString() : QFileInfo(located).absoluteFilePath();
}
