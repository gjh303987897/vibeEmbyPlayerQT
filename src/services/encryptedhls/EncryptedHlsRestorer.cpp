#include "services/encryptedhls/EncryptedHlsRestorer.h"

#include "services/encryptedhls/EncryptedHlsPackager.h"
#include "utils/AppLogger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace {
QString safeFileName(QString name)
{
    // Use portable basenames even when restoring a package made on another OS.
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) name.chop(1);
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("restored-video");
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch()) name.prepend(QLatin1Char('_'));
    return name;
}

QString muxerFor(const QString& extension)
{
    static const QHash<QString, QString> muxers {
        { QStringLiteral("mp4"), QStringLiteral("mp4") },
        { QStringLiteral("m4v"), QStringLiteral("mp4") },
        { QStringLiteral("mkv"), QStringLiteral("matroska") },
        { QStringLiteral("mov"), QStringLiteral("mov") },
        { QStringLiteral("ts"), QStringLiteral("mpegts") },
        { QStringLiteral("m2ts"), QStringLiteral("mpegts") },
        { QStringLiteral("mts"), QStringLiteral("mpegts") },
        { QStringLiteral("avi"), QStringLiteral("avi") },
        { QStringLiteral("webm"), QStringLiteral("webm") },
        { QStringLiteral("flv"), QStringLiteral("flv") },
        { QStringLiteral("ogv"), QStringLiteral("ogg") },
        { QStringLiteral("mpeg"), QStringLiteral("mpeg") },
        { QStringLiteral("mpg"), QStringLiteral("mpeg") }
    };
    return muxers.value(extension);
}
}

EncryptedHlsRestorer::EncryptedHlsRestorer(TsslStore& store, QObject* parent)
    : QObject(parent), m_proxy(store, this)
{
    connect(&m_ffmpeg, &QProcess::readyReadStandardOutput, this, &EncryptedHlsRestorer::readProgress);
    connect(&m_ffmpeg, &QProcess::readyReadStandardError, this, &EncryptedHlsRestorer::readDiagnostics);
    connect(&m_ffmpeg, &QProcess::finished, this, &EncryptedHlsRestorer::processFinished);
    connect(&m_ffmpeg, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_running && m_itemActive && error == QProcess::FailedToStart) {
            if (m_cancelRequested) settleCurrent(QStringLiteral("canceled"));
            else failCurrent(QStringLiteral("FFmpeg could not start. Check the configured executable."));
        }
    });
    connect(&m_proxy, &EncryptedHlsPlaybackProxy::streamFailed, this,
            [this](const QString& sessionId, const QString& reason) {
        if (m_running && sessionId == m_sessionId) stopWithError(reason);
    });
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(120000);
    connect(&m_watchdog, &QTimer::timeout, this, [this]() {
        stopWithError(QStringLiteral("FFmpeg stopped responding while restoring the package."));
    });
}

EncryptedHlsRestorer::~EncryptedHlsRestorer()
{
    ++m_generation;
    m_running = false;
    m_ffmpeg.disconnect(this);
    if (m_ffmpeg.state() != QProcess::NotRunning) {
        m_ffmpeg.kill();
        m_ffmpeg.waitForFinished(3000);
    }
    cleanupCurrent();
}

double EncryptedHlsRestorer::progress() const
{
    if (m_request.sources.isEmpty()) return 0;
    return std::clamp((static_cast<double>(std::max(0, m_index)) + m_itemProgress)
                      / m_request.sources.size(), 0.0, 1.0);
}

std::expected<void, QString> EncryptedHlsRestorer::start(EncryptedHlsRestoreRequest request)
{
    if (m_running) return std::unexpected(QStringLiteral("A restoration batch is already running."));
    if (request.sources.isEmpty()) return std::unexpected(QStringLiteral("Choose at least one M3U8S or M3U8SP file."));
    if (request.format != QStringLiteral("original") && request.format != QStringLiteral("mkv") &&
        request.format != QStringLiteral("mp4"))
        return std::unexpected(QStringLiteral("Unsupported restoration output format."));
    const QFileInfo output(request.outputDirectory);
    if (!output.isDir() || !output.isWritable())
        return std::unexpected(QStringLiteral("Choose an existing writable output folder."));
    m_executable = EncryptedHlsPackager::locateFfmpegExecutable();
    if (m_executable.isEmpty()) return std::unexpected(QStringLiteral("FFmpeg is unavailable. Choose an FFmpeg executable first."));
    request.outputDirectory = output.canonicalFilePath();
    m_request = std::move(request);
    m_index = -1;
    m_itemProgress = 0;
    m_cancelRequested = false;
    m_running = true;
    m_phase = QStringLiteral("preparing");
    ++m_generation;
    AppLogger::info(QStringLiteral("encrypted-hls-restore"),
                    QStringLiteral("Started restoration batch: %1 files").arg(m_request.sources.size()));
    emit stateChanged();
    scheduleNext();
    return {};
}

void EncryptedHlsRestorer::cancel()
{
    if (!m_running || m_cancelRequested) return;
    m_cancelRequested = true;
    ++m_generation; // stale preparation callbacks only revoke their session
    m_phase = QStringLiteral("canceling");
    emit stateChanged();
    if (m_ffmpeg.state() != QProcess::NotRunning) m_ffmpeg.kill();
    else {
        if (m_itemActive) settleCurrent(QStringLiteral("canceled"));
        else finishBatch();
    }
}

void EncryptedHlsRestorer::scheduleNext()
{
    const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation]() {
        if (m_running && generation == m_generation) startNext();
    });
}

void EncryptedHlsRestorer::startNext()
{
    if (!m_running) return;
    if (m_cancelRequested) { finishBatch(); return; }
    if (++m_index >= m_request.sources.size()) { finishBatch(); return; }
    m_itemProgress = 0;
    m_phase = QStringLiteral("preparing");
    m_legacyName = false;
    m_itemActive = true;
    const auto generation = m_generation;
    emit itemChanged(m_index, QStringLiteral("preparing"), {}, {}, false);
    emit stateChanged();
    if (!m_running || generation != m_generation) return;
    const QFileInfo source(m_request.sources.at(m_index));
    const auto extension = source.suffix().toLower();
    if (!source.isFile() || !source.isReadable() ||
        (extension != QStringLiteral("m3u8s") && extension != QStringLiteral("m3u8sp"))) {
        failCurrent(QStringLiteral("The input is not a readable M3U8S or M3U8SP file."));
        return;
    }
    m_proxy.prepareLocalStream(source.absoluteFilePath(), [this, generation](EncryptedHlsPrepareResult prepared) {
        if (!m_running || generation != m_generation) {
            if (prepared) m_proxy.revoke(prepared->sessionId);
            return;
        }
        if (!prepared) { failCurrent(prepared.error()); return; }
        launch(std::move(*prepared));
    });
}

void EncryptedHlsRestorer::launch(EncryptedHlsPreparedStream prepared)
{
    m_sessionId = std::move(prepared.sessionId);
    m_legacyName = prepared.sourceFileName.isEmpty();
    const auto sourceName = m_legacyName
        ? QFileInfo(m_request.sources.at(m_index)).completeBaseName() + QStringLiteral(".mkv")
        : prepared.sourceFileName;
    auto name = safeFileName(sourceName);
    if (m_request.format != QStringLiteral("original"))
        name = QFileInfo(name).completeBaseName() + QLatin1Char('.') + m_request.format;
    const auto extension = QFileInfo(name).suffix().toLower();
    const auto muxer = muxerFor(extension);
    if (muxer.isEmpty()) {
        failCurrent(QStringLiteral("The original container format is unsupported (%1). Select MKV or MP4 and retry.").arg(extension));
        return;
    }
    const QDir output(m_request.outputDirectory);
    m_outputPath = output.filePath(name);
    for (int suffix = 1; QFileInfo::exists(m_outputPath); ++suffix)
        m_outputPath = output.filePath(QStringLiteral("%1 (%2).%3")
            .arg(QFileInfo(name).completeBaseName()).arg(suffix).arg(QFileInfo(name).suffix()));
    m_staging = std::make_unique<QTemporaryDir>(output.filePath(QStringLiteral(".vibe-restore-XXXXXX")));
    if (!m_staging->isValid()) {
        failCurrent(QStringLiteral("Unable to create a temporary output directory. Check permissions and free space."));
        return;
    }
    m_stagingPath = m_staging->filePath(QStringLiteral("restored.") + extension);
    QStringList arguments { QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-n"),
        QStringLiteral("-nostats"), QStringLiteral("-xerror"), QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        QStringLiteral("-protocol_whitelist"), QStringLiteral("http,tcp"),
        QStringLiteral("-allowed_extensions"), QStringLiteral("ALL"), QStringLiteral("-f"), QStringLiteral("hls"),
        QStringLiteral("-i"), prepared.url.toString(QUrl::FullyEncoded),
        QStringLiteral("-map"), QStringLiteral("0:v:0"), QStringLiteral("-map"), QStringLiteral("0:a?"),
        QStringLiteral("-map"), QStringLiteral("0:s?"), QStringLiteral("-c"), QStringLiteral("copy"),
        QStringLiteral("-f"), muxer };
    if (muxer == QStringLiteral("mp4") || muxer == QStringLiteral("mov"))
        arguments << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    if (extension == QStringLiteral("m2ts") || extension == QStringLiteral("mts"))
        arguments << QStringLiteral("-mpegts_m2ts_mode") << QStringLiteral("1");
    arguments << m_stagingPath;
    m_phase = QStringLiteral("restoring");
    m_watchdog.start();
    m_ffmpeg.start(m_executable, arguments);
    emit itemChanged(m_index, m_phase, {}, {}, m_legacyName);
    emit stateChanged();
}

void EncryptedHlsRestorer::readProgress()
{
    m_progressBuffer.append(m_ffmpeg.readAllStandardOutput());
    while (true) {
        const auto newline = m_progressBuffer.indexOf('\n');
        if (newline < 0) break;
        const auto line = m_progressBuffer.first(newline).trimmed();
        m_progressBuffer.remove(0, newline + 1);
        const auto separator = line.indexOf('=');
        const auto key = line.first(separator);
        const auto value = line.mid(separator + 1);
        if (key == "frame") m_frames = value.toLongLong();
        if (key == "out_time_us" && m_durationUs > 0)
            m_itemProgress = std::clamp(static_cast<double>(value.toLongLong()) / m_durationUs, 0.0, 0.99);
    }
    if (m_progressBuffer.size() > 65536) m_progressBuffer.clear();
    if (m_running) { m_watchdog.start(); emit stateChanged(); }
}

void EncryptedHlsRestorer::readDiagnostics()
{
    m_diagnostics.append(m_ffmpeg.readAllStandardError());
    if (m_durationUs == 0) {
        static const QRegularExpression duration(QStringLiteral("Duration: (\\d+):(\\d+):(\\d+(?:\\.\\d+)?)"));
        const auto match = duration.match(QString::fromUtf8(m_diagnostics));
        if (match.hasMatch()) m_durationUs = static_cast<qint64>((match.captured(1).toDouble() * 3600
            + match.captured(2).toDouble() * 60 + match.captured(3).toDouble()) * 1000000);
    }
    // Bound diagnostics and never display FFmpeg's raw input URL/session token.
    if (m_diagnostics.size() > 32768) m_diagnostics = m_diagnostics.last(32768);
    if (m_running) m_watchdog.start();
}

void EncryptedHlsRestorer::processFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_running) return;
    readDiagnostics();
    readProgress();
    if (m_cancelRequested) { settleCurrent(QStringLiteral("canceled")); return; }
    if (!m_pendingError.isEmpty()) { failCurrent(m_pendingError); return; }
    if (status != QProcess::NormalExit || exitCode != 0 || m_frames <= 0 || QFileInfo(m_stagingPath).size() <= 0) {
        // Detailed codec/muxer failures can include the loopback URL. Present a
        // stable actionable message, rather than leaking raw FFmpeg diagnostics.
        AppLogger::warning(QStringLiteral("encrypted-hls-restore"),
                           QStringLiteral("FFmpeg restoration exit code: %1; exit status: %2; video frames: %3")
                               .arg(exitCode).arg(static_cast<int>(status)).arg(m_frames));
        failCurrent(m_diagnostics.contains("No space left on device")
            ? QStringLiteral("The output disk is full. Free up space and retry.")
            : QStringLiteral("FFmpeg could not restore this package. Check that it is intact and try MKV for incompatible codecs."));
        return;
    }
    if (!QFile::rename(m_stagingPath, m_outputPath)) {
        failCurrent(QStringLiteral("Unable to save the restored video. The destination may already exist or the disk may be full."));
        return;
    }
    settleCurrent(QStringLiteral("completed"));
}

void EncryptedHlsRestorer::stopWithError(QString error)
{
    if (!m_running || m_cancelRequested || !m_pendingError.isEmpty()) return;
    m_pendingError = std::move(error);
    if (m_ffmpeg.state() != QProcess::NotRunning) m_ffmpeg.kill();
    else failCurrent(m_pendingError);
}

void EncryptedHlsRestorer::failCurrent(QString error)
{
    AppLogger::warning(QStringLiteral("encrypted-hls-restore"),
                       QStringLiteral("Restoration item %1 failed: %2").arg(m_index + 1).arg(error));
    settleCurrent(QStringLiteral("failed"), error);
}

void EncryptedHlsRestorer::settleCurrent(const QString& state, const QString& error)
{
    const auto output = state == QStringLiteral("completed") ? m_outputPath : QString {};
    cleanupCurrent();
    m_itemActive = false;
    m_itemProgress = 1;
    emit itemChanged(m_index, state, output, error, m_legacyName);
    emit stateChanged();
    if (m_cancelRequested) finishBatch();
    else scheduleNext();
}

void EncryptedHlsRestorer::cleanupCurrent()
{
    m_watchdog.stop();
    if (!m_sessionId.isEmpty()) m_proxy.revoke(std::exchange(m_sessionId, {}));
    m_staging.reset();
    m_outputPath.clear();
    m_stagingPath.clear();
    m_pendingError.clear();
    m_progressBuffer.clear();
    m_diagnostics.clear();
    m_durationUs = 0;
    m_frames = 0;
}

void EncryptedHlsRestorer::finishBatch()
{
    if (!m_running) return;
    if (m_cancelRequested)
        for (int i = std::max(0, m_index + 1); i < m_request.sources.size(); ++i)
            emit itemChanged(i, QStringLiteral("canceled"), {}, {}, false);
    m_running = false;
    m_phase = m_cancelRequested ? QStringLiteral("canceled") : QStringLiteral("completed");
    AppLogger::info(QStringLiteral("encrypted-hls-restore"),
                    m_cancelRequested ? QStringLiteral("Restoration batch canceled") : QStringLiteral("Restoration batch finished"));
    emit stateChanged();
    emit finished(m_cancelRequested);
}
