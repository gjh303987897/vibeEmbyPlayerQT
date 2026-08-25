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

QByteArray identifierFromRandomBytes(QByteArrayView randomBytes)
{
    return QByteArray(randomBytes.data(), randomBytes.size())
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

int crfFor(EncryptedHlsVideoEncoding encoding, EncryptedHlsVideoQuality quality)
{
    if (encoding == EncryptedHlsVideoEncoding::H265) {
        switch (quality) {
        case EncryptedHlsVideoQuality::High: return 20;
        case EncryptedHlsVideoQuality::Balanced: return 23;
        case EncryptedHlsVideoQuality::Compact: return 28;
        }
    }
    switch (quality) {
    case EncryptedHlsVideoQuality::High: return 18;
    case EncryptedHlsVideoQuality::Balanced: return 20;
    case EncryptedHlsVideoQuality::Compact: return 24;
    }
    return 20;
}
}

namespace EncryptedHlsPackaging {

std::expected<QString, QString> probeVideoCodec(const QString& sourcePath,
                                                const QString& ffmpegExecutable)
{
    QProcess probe;
    probe.setProcessChannelMode(QProcess::SeparateChannels);
    probe.start(ffmpegExecutable,
                { QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"),
                  QStringLiteral("-i"), sourcePath,
                  QStringLiteral("-map"), QStringLiteral("0:v:0"),
                  QStringLiteral("-c:v"), QStringLiteral("copy"),
                  QStringLiteral("-an"), QStringLiteral("-f"), QStringLiteral("null"),
                  QStringLiteral("-") },
                QIODevice::ReadOnly);
    if (!probe.waitForStarted(5'000)) {
        return std::unexpected(QStringLiteral("Unable to inspect the source video codec: %1")
                                   .arg(probe.errorString()));
    }
    if (!probe.waitForFinished(60'000)) {
        probe.kill();
        probe.waitForFinished(2'000);
        return std::unexpected(QStringLiteral("Timed out while inspecting the source video codec"));
    }
    const auto diagnostics = QString::fromUtf8(probe.readAllStandardError());
    static const QRegularExpression codecPattern(
        QStringLiteral("Stream #[^\\n]*Video:\\s*([A-Za-z0-9_.-]+)"));
    const auto match = codecPattern.match(diagnostics);
    if (!match.hasMatch()) {
        return std::unexpected(QStringLiteral("Unable to determine the source video codec"));
    }
    auto codec = match.captured(1).toLower();
    if (codec == QStringLiteral("hevc")) {
        codec = QStringLiteral("h265");
    }
    return codec;
}

std::expected<QStringList, QString> buildFfmpegArguments(
    const EncryptedHlsPackageRequest& request,
    const QString& segmentPattern,
    const QString& manifestPath)
{
    if (request.sourcePath.isEmpty() || segmentPattern.isEmpty() || manifestPath.isEmpty()) {
        return std::unexpected(QStringLiteral("FFmpeg input and output paths must not be empty"));
    }
    if (request.segmentDurationSeconds < 2 || request.segmentDurationSeconds > 30) {
        return std::unexpected(QStringLiteral("HLS segment duration must be between 2 and 30 seconds"));
    }
    switch (request.videoQuality) {
    case EncryptedHlsVideoQuality::High:
    case EncryptedHlsVideoQuality::Balanced:
    case EncryptedHlsVideoQuality::Compact:
        break;
    default:
        return std::unexpected(QStringLiteral("Unsupported M3U8S video quality"));
    }

    const auto duration = QString::number(request.segmentDurationSeconds);
    QStringList arguments {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-y"),
        QStringLiteral("-i"), request.sourcePath,
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a:0?"),
        QStringLiteral("-sn"),
    };

    switch (request.videoEncoding) {
    case EncryptedHlsVideoEncoding::Copy:
        arguments.append({ QStringLiteral("-c:v"), QStringLiteral("copy") });
        break;
    case EncryptedHlsVideoEncoding::H264:
    case EncryptedHlsVideoEncoding::H265:
        arguments.append({
            QStringLiteral("-c:v"),
            request.videoEncoding == EncryptedHlsVideoEncoding::H264
                ? QStringLiteral("libx264")
                : QStringLiteral("libx265"),
            QStringLiteral("-preset"), QStringLiteral("veryfast"),
            QStringLiteral("-crf"), QString::number(crfFor(request.videoEncoding, request.videoQuality)),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
            QStringLiteral("-flags"), QStringLiteral("+cgop"),
        });
        arguments.append(request.videoEncoding == EncryptedHlsVideoEncoding::H264
                             ? QStringList { QStringLiteral("-sc_threshold"), QStringLiteral("0") }
                             : QStringList { QStringLiteral("-x265-params"), QStringLiteral("scenecut=0") });
        arguments.append({
            QStringLiteral("-force_key_frames"),
            QStringLiteral("expr:gte(t,n_forced*") + duration + QLatin1Char(')'),
        });
        break;
    case EncryptedHlsVideoEncoding::Auto:
        return std::unexpected(QStringLiteral("Automatic M3U8S video encoding must be resolved before FFmpeg starts"));
    default:
        return std::unexpected(QStringLiteral("Unsupported M3U8S video encoding mode"));
    }

    switch (request.audioEncoding) {
    case EncryptedHlsAudioEncoding::Copy:
        arguments.append({ QStringLiteral("-c:a"), QStringLiteral("copy") });
        break;
    case EncryptedHlsAudioEncoding::Aac:
        arguments.append({
            QStringLiteral("-c:a"), QStringLiteral("aac"),
            QStringLiteral("-b:a"), QStringLiteral("192k"),
        });
        break;
    default:
        return std::unexpected(QStringLiteral("Unsupported M3U8S audio encoding mode"));
    }

    const auto hlsFlags = request.videoEncoding == EncryptedHlsVideoEncoding::Copy
        ? QStringLiteral("temp_file")
        : QStringLiteral("independent_segments+temp_file");
    arguments.append({
        QStringLiteral("-f"), QStringLiteral("hls"),
        QStringLiteral("-hls_time"), duration,
        QStringLiteral("-hls_playlist_type"), QStringLiteral("vod"),
        QStringLiteral("-hls_segment_type"), QStringLiteral("mpegts"),
        QStringLiteral("-hls_flags"), hlsFlags,
        QStringLiteral("-hls_segment_filename"), segmentPattern,
        QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        QStringLiteral("-nostats"),
        manifestPath,
    });
    return arguments;
}

std::expected<void, QString> validateGeneratedVideoTrack(
    const QString& directoryPath,
    const QString& ffmpegExecutable,
    std::atomic_bool& cancelRequested)
{
    const auto manifestPath = QDir(directoryPath).filePath(QStringLiteral("index.m3u8"));
    if (!QFileInfo(manifestPath).isReadable()) {
        return std::unexpected(QStringLiteral("FFmpeg did not produce any TS segments"));
    }

    QProcess probe;
    probe.setProcessChannelMode(QProcess::SeparateChannels);
    probe.start(ffmpegExecutable,
                { QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"),
                  QStringLiteral("-v"), QStringLiteral("error"),
                  QStringLiteral("-allowed_extensions"), QStringLiteral("ALL"),
                  QStringLiteral("-i"), manifestPath,
                  QStringLiteral("-map"), QStringLiteral("0:v:0"),
                  QStringLiteral("-c:v"), QStringLiteral("copy"),
                  QStringLiteral("-an"), QStringLiteral("-f"),
                  QStringLiteral("null"), QStringLiteral("-") },
                QIODevice::ReadOnly);
    if (!probe.waitForStarted(5'000)) {
        return std::unexpected(QStringLiteral("Unable to validate the generated HLS video track"));
    }

    constexpr int maximumProbeWaitMilliseconds = 60'000;
    int elapsed = 0;
    while (!probe.waitForFinished(100)) {
        elapsed += 100;
        if (cancelRequested.load(std::memory_order_relaxed)) {
            probe.kill();
            probe.waitForFinished(2'000);
            return std::unexpected(QStringLiteral("Packaging was canceled"));
        }
        if (elapsed >= maximumProbeWaitMilliseconds) {
            probe.kill();
            probe.waitForFinished(2'000);
            return std::unexpected(QStringLiteral("Timed out while validating the generated HLS video track"));
        }
    }
    if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
        const auto detail = QString::fromUtf8(probe.readAllStandardError()).trimmed().left(2000);
        AppLogger::warning(QStringLiteral("encrypted-hls"),
                           detail.isEmpty()
                               ? QStringLiteral("Generated MPEG-TS segments do not expose a decodable video track")
                               : QStringLiteral("Generated MPEG-TS video validation failed: %1").arg(detail));
        return std::unexpected(
            QStringLiteral("The generated HLS segments contain no decodable video track. "
                           "The source video codec is not compatible with MPEG-TS stream copy; "
                           "choose H.264 or H.265 video encoding and try again."));
    }
    return {};
}

std::expected<EncryptedHlsPreparedPackage, QString> encryptHlsDirectory(
    const QString& directoryPath,
    const QString& sourceFileName,
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

    const auto sourceFileNameBytes = sourceFileName.toUtf8();
    if (sourceFileNameBytes.isEmpty() || sourceFileNameBytes.size() > 4096 ||
        sourceFileName.contains(QLatin1Char('/')) || sourceFileName.contains(QLatin1Char('\\'))) {
        identifier.fill('\0');
        return std::unexpected(QStringLiteral("The source filename cannot be represented safely in M3U8S metadata"));
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
    const auto sourceNameAad = TsslPackage::sourceFileNameAuthenticatedData(identifier);
    auto encryptedSourceFileName = AesGcmDecryptor::encryptAuthenticatedData(sourceFileNameBytes, sourceNameAad);
    if (!encryptedSourceFileName) {
        identifier.fill('\0');
        return std::unexpected(encryptedSourceFileName.error());
    }
    auto verifiedSourceFileName = AesGcmDecryptor::decryptAuthenticatedData(
        encryptedSourceFileName->bytes,
        encryptedSourceFileName->key,
        sourceNameAad);
    if (!verifiedSourceFileName || *verifiedSourceFileName != sourceFileNameBytes) {
        if (verifiedSourceFileName) {
            verifiedSourceFileName->fill('\0');
        }
        encryptedSourceFileName->key.fill('\0');
        encryptedSourceFileName->bytes.fill('\0');
        identifier.fill('\0');
        return std::unexpected(QStringLiteral("GCM authentication verification failed for the source filename"));
    }
    verifiedSourceFileName->fill('\0');

    auto manifest = HlsManifestValidator::insertM3u8sIdentifier(*plainManifest, identifier);
    if (!manifest) {
        encryptedSourceFileName->key.fill('\0');
        identifier.fill('\0');
        return std::unexpected(manifest.error());
    }
    manifest = HlsManifestValidator::insertEncryptedSourceFileName(*manifest, encryptedSourceFileName->bytes);
    if (!manifest) {
        encryptedSourceFileName->key.fill('\0');
        identifier.fill('\0');
        return std::unexpected(manifest.error());
    }
    if (auto validated = HlsManifestValidator::validate(*manifest); !validated) {
        encryptedSourceFileName->key.fill('\0');
        identifier.fill('\0');
        return std::unexpected(validated.error());
    }

    const auto manifestFileName = QStringLiteral("index.m3u8s");
    const auto manifestPath = directory.filePath(manifestFileName);
    if (auto written = writeFileAtomically(manifestPath, *manifest); !written) {
        encryptedSourceFileName->key.fill('\0');
        identifier.fill('\0');
        return std::unexpected(written.error());
    }
    if (!QFile::remove(sourceManifestPath)) {
        encryptedSourceFileName->key.fill('\0');
        identifier.fill('\0');
        return std::unexpected(QStringLiteral("Unable to remove the plaintext HLS manifest"));
    }

    TsslPackage package {
        .version = 3,
        .identifier = std::move(identifier),
        .rootManifestDigest = QCryptographicHash::hash(*manifest, QCryptographicHash::Sha256),
        .encryptedSourceFileName = std::move(encryptedSourceFileName->bytes),
        .sourceFileNameKey = std::move(encryptedSourceFileName->key),
        .segmentKeys = std::move(segmentKeys),
    };
    return EncryptedHlsPreparedPackage {
        .tsslPackage = std::move(package),
        .manifestFileName = manifestFileName,
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
    connect(&m_tarWatcher,
            &QFutureWatcher<std::expected<EncryptedHlsTarIndex, QString>>::finished,
            this,
            &EncryptedHlsPackager::handleTarFinished);
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
    if (m_tarWatcher.isRunning()) {
        m_tarWatcher.waitForFinished();
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
    m_sourceFileName = source.fileName();
    m_ffmpegExecutable = ffmpeg;
    m_containerFormat = request.containerFormat;
    m_finalOutputPath.clear();
    m_stagingDirectory = std::make_unique<QTemporaryDir>(
        QDir(m_outputDirectory).filePath(QStringLiteral(".m3u8s-staging-XXXXXX")));
    if (!m_stagingDirectory->isValid()) {
        resetRunState();
        return std::unexpected(QStringLiteral("Unable to create a temporary packaging directory"));
    }

    const auto segmentPattern = QDir(m_stagingDirectory->path()).filePath(QStringLiteral("segment_%06d.ts"));
    const auto manifestPath = QDir(m_stagingDirectory->path()).filePath(QStringLiteral("index.m3u8"));
    auto effectiveRequest = request;
    effectiveRequest.sourcePath = source.absoluteFilePath();
    effectiveRequest.outputDirectory = m_outputDirectory;
    if (effectiveRequest.videoEncoding == EncryptedHlsVideoEncoding::Auto) {
        const auto codec = EncryptedHlsPackaging::probeVideoCodec(source.absoluteFilePath(), ffmpeg);
        if (!codec) {
            resetRunState();
            return std::unexpected(codec.error());
        }
        const auto accepted = std::ranges::any_of(effectiveRequest.autoCopyVideoCodecs,
                                                   [&codec](const QString& candidate) {
                                                       return candidate == *codec;
                                                   });
        effectiveRequest.videoEncoding = accepted
            ? EncryptedHlsVideoEncoding::Copy
            : effectiveRequest.autoFallbackVideoEncoding;
        AppLogger::info(QStringLiteral("encrypted-hls"),
                        QStringLiteral("Automatic video encoding selected %1 for source codec %2")
                            .arg(effectiveRequest.videoEncoding == EncryptedHlsVideoEncoding::Copy
                                     ? QStringLiteral("copy")
                                     : effectiveRequest.videoEncoding == EncryptedHlsVideoEncoding::H265
                                         ? QStringLiteral("h265") : QStringLiteral("h264"),
                                 *codec));
    }
    auto arguments = EncryptedHlsPackaging::buildFfmpegArguments(
        effectiveRequest,
        segmentPattern,
        manifestPath);
    if (!arguments) {
        resetRunState();
        return std::unexpected(arguments.error());
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

    AppLogger::info(QStringLiteral("encrypted-hls"),
                    QStringLiteral("Starting local video segmentation for an M3U8S package"));
    m_ffmpeg.start(ffmpeg, *arguments, QIODevice::ReadOnly);
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
    const auto sourceFileName = m_sourceFileName;
    const auto ffmpegExecutable = m_ffmpegExecutable;
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
        [directoryPath, sourceFileName, ffmpegExecutable, this,
         progressCallback = std::move(progressCallback)]() {
            if (auto validated = EncryptedHlsPackaging::validateGeneratedVideoTrack(
                    directoryPath, ffmpegExecutable, m_cancelRequested); !validated) {
                return std::expected<EncryptedHlsPreparedPackage, QString>(
                    std::unexpected(validated.error()));
            }
            return EncryptedHlsPackaging::encryptHlsDirectory(directoryPath,
                                                              sourceFileName,
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
    if (m_cancelRequested.load(std::memory_order_relaxed)) {
        finishCanceled();
        return;
    }
    if (!prepared) {
        finishFailure(prepared.error());
        return;
    }

    m_pendingPreparedPackage = std::move(*prepared);
    m_finalOutputPath = chooseOutputPath(m_outputDirectory,
                                         m_pendingPreparedPackage.tsslPackage.rootManifestDigest,
                                         m_containerFormat);
    if (m_containerFormat == EncryptedHlsContainerFormat::DirectoryM3u8s) {
        setPhase(QStringLiteral("finalizing"));
        setProgress(0.96);
        if (auto saved = m_store.savePackage(m_pendingPreparedPackage.tsslPackage); !saved) {
            finishFailure(saved.error());
            return;
        }
        if (!QDir().rename(m_stagingDirectory->path(), m_finalOutputPath)) {
            m_store.deleteByRootDigest(m_pendingPreparedPackage.tsslPackage.rootManifestDigest);
            finishFailure(QStringLiteral("Unable to move the completed M3U8S package into the output directory"));
            return;
        }
        const EncryptedHlsPackageResult result {
            .outputDirectory = m_finalOutputPath,
            .manifestPath = QDir(m_finalOutputPath).filePath(m_pendingPreparedPackage.manifestFileName),
            .identifier = m_pendingPreparedPackage.tsslPackage.identifier,
            .rootManifestDigest = m_pendingPreparedPackage.tsslPackage.rootManifestDigest,
            .segmentCount = m_pendingPreparedPackage.segmentCount,
        };
        m_stagingDirectory.reset();
        m_terminalReported = true;
        m_running = false;
        setProgress(1.0);
        setPhase(QStringLiteral("completed"));
        emit runningChanged();
        AppLogger::info(QStringLiteral("encrypted-hls"),
                        QStringLiteral("Created an M3U8S directory package with %1 encrypted segments")
                            .arg(result.segmentCount));
        emit completed(result);
        return;
    }

    setPhase(QStringLiteral("archiving"));
    setProgress(0.96);
    const auto stagingPath = m_stagingDirectory->path();
    const auto manifestName = m_pendingPreparedPackage.manifestFileName;
    const auto outputPath = m_finalOutputPath;
    m_tarWatcher.setFuture(QtConcurrent::run([this, stagingPath, outputPath, manifestName]() {
        return EncryptedHlsTarContainer::build(stagingPath, outputPath, manifestName, &m_cancelRequested);
    }));
}

void EncryptedHlsPackager::handleTarFinished()
{
    if (!m_running || m_terminalReported) return;
    auto tar = m_tarWatcher.result();
    if (m_cancelRequested.load(std::memory_order_relaxed)) { finishCanceled(); return; }
    if (!tar) { finishFailure(tar.error()); return; }
    m_pendingPreparedPackage.tsslPackage.version = 4;
    m_pendingPreparedPackage.tsslPackage.containerFormat = QStringLiteral("m3u8sp-tar-index-v1");
    m_pendingPreparedPackage.tsslPackage.containerIndexSha256 = tar->sha256;
    m_pendingPreparedPackage.tsslPackage.containerLength = tar->containerLength;
    if (auto saved = m_store.savePackage(m_pendingPreparedPackage.tsslPackage); !saved) {
        QFile::remove(m_finalOutputPath);
        finishFailure(saved.error());
        return;
    }

    const EncryptedHlsPackageResult result {
        .outputDirectory = m_finalOutputPath,
        .manifestPath = m_finalOutputPath,
        .identifier = m_pendingPreparedPackage.tsslPackage.identifier,
        .rootManifestDigest = m_pendingPreparedPackage.tsslPackage.rootManifestDigest,
        .segmentCount = m_pendingPreparedPackage.segmentCount,
    };
    m_stagingDirectory.reset();
    m_terminalReported = true;
    m_running = false;
    setProgress(1.0);
    setPhase(QStringLiteral("completed"));
    emit runningChanged();
    AppLogger::info(QStringLiteral("encrypted-hls"),
                    QStringLiteral("Created an M3U8SP package with %1 encrypted segments")
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
    m_sourceFileName.clear();
    m_finalOutputPath.clear();
    m_progressBuffer.clear();
    m_diagnostics.clear();
}

QString EncryptedHlsPackager::chooseOutputPath(const QString& outputDirectory,
                                               QByteArrayView rootManifestDigest,
                                               EncryptedHlsContainerFormat format) const
{
    const auto digestHex = QByteArray(rootManifestDigest.data(), rootManifestDigest.size()).toHex();
    const auto baseName = QStringLiteral("m3u8s_%1").arg(QString::fromLatin1(digestHex));
    const auto extension = format == EncryptedHlsContainerFormat::DirectoryM3u8s
        ? QString()
        : QStringLiteral(".m3u8sp");
    auto candidate = QDir(outputDirectory).filePath(baseName + extension);
    for (int suffix = 2; QFileInfo::exists(candidate); ++suffix) {
        candidate = QDir(outputDirectory).filePath(baseName + QLatin1Char('_') + QString::number(suffix) +
                                                   extension);
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
