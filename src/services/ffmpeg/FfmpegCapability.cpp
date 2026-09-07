#include "services/ffmpeg/FfmpegCapability.h"

#include "utils/AppLogger.h"

#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>

namespace {

// The packaging pipeline (EncryptedHlsPackaging::buildFfmpegArguments)
// requires these encoders and muxers by name; `copy` is a mux-side passthrough
// and needs no encoder entry. 5.0 is the documented floor: the pipeline's
// HLS flags (independent_segments, temp_file) and force_key_frames usage are
// stable from 5.x onward, and older branches lack fixes we implicitly rely on
// for +cgop segment keyframe alignment.
constexpr int minimumMajorVersion = 5;
constexpr std::initializer_list<const char*> requiredEncoders { "libx264", "libx265", "aac" };
constexpr std::initializer_list<const char*> requiredMuxers { "hls" };

constexpr int startTimeoutMs = 5'000;
constexpr int finishTimeoutMs = 15'000;

struct ProcessOutput {
    bool ok { false };
    QString output;
    QString error;
};

ProcessOutput runFfmpeg(const QString& executable, const QStringList& arguments)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(executable, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(startTimeoutMs)) {
        return {.error = QStringLiteral("Unable to start %1: %2").arg(executable, process.errorString()) };
    }
    if (!process.waitForFinished(finishTimeoutMs)) {
        process.kill();
        process.waitForFinished(2'000);
        return {.error = QStringLiteral("%1 timed out").arg(executable) };
    }
    // -version / -encoders / -muxers exit 0; tolerate nonzero anyway because
    // the banner is still printed and some builds warn on exotic consoles.
    return {
        .ok = true,
        .output = QString::fromLocal8Bit(process.readAllStandardOutput()),
        .error = QString::fromLocal8Bit(process.readAllStandardError()),
    };
}

// `ffmpeg -encoders` / `-muxers` print two-column tables below a header.
// Encoder rows look like " V..... libx264  desc"; muxer rows like
// "  hls         Apple HLS". Collect the name column of every table row.
QStringList tableNames(const QString& listing)
{
    QStringList names;
    const auto lines = listing.split(QLatin1Char('\n'));
    for (const auto& rawLine : lines) {
        const auto line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('-')) ||
            line.startsWith(QStringLiteral("Encoders"), Qt::CaseInsensitive) ||
            line.startsWith(QStringLiteral("File m"), Qt::CaseInsensitive) || // "File muxers:"
            line.startsWith(QStringLiteral("Supported")) ||
            line.startsWith(QStringLiteral("Codecs:")) ||
            line.startsWith(QStringLiteral("Stream")) ||
            line.startsWith(QStringLiteral("Disposition"))) {
            continue;
        }
        const auto columns = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (columns.size() >= 2) {
            names.append(columns.at(1));
        }
    }
    return names;
}

}

namespace FfmpegCapabilityProbe {

std::expected<QVersionNumber, QString> parseVersionBanner(const QString& bannerOutput)
{
    // Shapes seen in the wild:
    //   ffmpeg version 7.1.1 Copyright ...
    //   ffmpeg version 7.1.1-essentials_build-www.gyan.dev Copyright ...
    //   ffmpeg version n7.1 Copyright ...                        (BtbN tag builds)
    //   ffmpeg version git-2024-05-01-abcdef Copyright ...       (date-stamped)
    static const QRegularExpression pattern(
        QStringLiteral(R"(ffmpeg\s+version\s+n?(\d+)(?:\.(\d+))?(?:\.(\d+))?)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(bannerOutput);
    if (!match.hasMatch()) {
        // Date-stamped git builds (git-2024-05-01-...) carry no comparable
        // numeric version; callers treat nullopt as "skip version gate and
        // trust the feature probe".
        return std::unexpected(QStringLiteral("No numeric version in ffmpeg banner"));
    }
    bool ok = false;
    const auto major = match.captured(1).toInt(&ok);
    if (!ok) {
        return std::unexpected(QStringLiteral("Malformed ffmpeg version"));
    }
    return QVersionNumber(major,
                          match.captured(2).isEmpty() ? 0 : match.captured(2).toInt(),
                          match.captured(3).isEmpty() ? 0 : match.captured(3).toInt());
}

QStringList missingRequiredFeatures(const QStringList& encoders, const QStringList& muxers)
{
    QStringList missing;
    for (const auto* encoder : requiredEncoders) {
        if (!encoders.contains(QLatin1String(encoder), Qt::CaseInsensitive)) {
            missing.append(QStringLiteral("encoder:%1").arg(QLatin1String(encoder)));
        }
    }
    for (const auto* muxer : requiredMuxers) {
        if (!muxers.contains(QLatin1String(muxer), Qt::CaseInsensitive)) {
            missing.append(QStringLiteral("muxer:%1").arg(QLatin1String(muxer)));
        }
    }
    return missing;
}

FfmpegCapability run(const QString& executable)
{
    FfmpegCapability capability;
    if (executable.isEmpty()) {
        capability.detail = QStringLiteral("FFmpeg executable not found (application directory and PATH)");
        AppLogger::warning(QStringLiteral("ffmpeg"), capability.detail);
        return capability;
    }
    capability.executable = executable;

    const auto versionRun = runFfmpeg(executable, { QStringLiteral("-hide_banner"), QStringLiteral("-version") });
    if (!versionRun.ok) {
        capability.state = FfmpegCapability::State::Unavailable;
        capability.detail = versionRun.error;
        AppLogger::warning(QStringLiteral("ffmpeg"), capability.detail);
        return capability;
    }
    const auto banner = versionRun.output.isEmpty() ? versionRun.error : versionRun.output;
    if (auto version = parseVersionBanner(banner)) {
        capability.version = *version;
        if (capability.version.majorVersion() < minimumMajorVersion) {
            capability.state = FfmpegCapability::State::Incompatible;
            capability.detail = QStringLiteral("FFmpeg %1 is older than the supported minimum %2.0")
                                    .arg(capability.version.toString(), QString::number(minimumMajorVersion));
            AppLogger::warning(QStringLiteral("ffmpeg"), capability.detail);
            return capability;
        }
    }

    const auto encoderRun = runFfmpeg(executable, { QStringLiteral("-hide_banner"), QStringLiteral("-encoders") });
    const auto muxerRun = runFfmpeg(executable, { QStringLiteral("-hide_banner"), QStringLiteral("-muxers") });
    if (!encoderRun.ok || !muxerRun.ok) {
        capability.state = FfmpegCapability::State::Incompatible;
        capability.detail = encoderRun.ok ? muxerRun.error : encoderRun.error;
        AppLogger::warning(QStringLiteral("ffmpeg"), capability.detail);
        return capability;
    }
    capability.missingFeatures = missingRequiredFeatures(tableNames(encoderRun.output),
                                                         tableNames(muxerRun.output));
    if (!capability.missingFeatures.isEmpty()) {
        capability.state = FfmpegCapability::State::Incompatible;
        capability.detail = QStringLiteral("FFmpeg build is missing required features: %1")
                                .arg(capability.missingFeatures.join(QLatin1Char(', ')));
        AppLogger::warning(QStringLiteral("ffmpeg"), capability.detail);
        return capability;
    }

    capability.state = FfmpegCapability::State::Available;
    capability.detail = QStringLiteral("FFmpeg %1 at %2 satisfies the M3U8S packaging pipeline")
                            .arg(capability.version.isNull() ? QStringLiteral("unknown version")
                                                             : capability.version.toString(),
                                 executable);
    AppLogger::info(QStringLiteral("ffmpeg"), capability.detail);
    return capability;
}

}
