#include "services/ffmpeg/FfmpegCapability.h"

#include "utils/AppLogger.h"

#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace {

// The only contract enforced at startup is the version floor: the packaging
// pipeline's HLS flags (independent_segments, temp_file) and force_key_frames
// usage are stable from 5.x onward, and older branches lack fixes we implicitly
// rely on for +cgop segment keyframe alignment. Whether a build actually ships
// libx264/libx265/aac/hls is deliberately NOT probed - a stripped build fails in
// the packaging error path, where the ffmpeg diagnostics explain it better than
// a table scan can, and startup does not pay for two more child processes.
constexpr int minimumMajorVersion = 5;

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
    // -version exits 0; tolerate nonzero anyway because the banner is still
    // printed and some builds warn on exotic consoles.
    return {
        .ok = true,
        .output = QString::fromLocal8Bit(process.readAllStandardOutput()),
        .error = QString::fromLocal8Bit(process.readAllStandardError()),
    };
}

} // namespace

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
        // numeric version; the caller treats nullopt as "skip the version floor"
        // because such a build is by definition newer than the floor.
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
    // With -hide_banner the first line is "ffmpeg version <build> Copyright ...".
    // Requiring that identity before trusting anything else is what separates
    // FFmpeg from the binaries that ship beside it (ffprobe.exe and ffplay.exe sit
    // in the same folder on every Windows package, and the M3U8S page lets the user
    // pick an executable by hand): without it a wrong pick parses as "no numeric
    // version" and would be waved through as a date-stamped build.
    static const QRegularExpression ffmpegBannerPattern(
        QStringLiteral(R"(^\s*ffmpeg\s+version\b)"), QRegularExpression::CaseInsensitiveOption);
    auto bannerLine = banner.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (bannerLine.size() > 120) {
        bannerLine = bannerLine.left(117) + QStringLiteral("...");
    }
    if (!ffmpegBannerPattern.match(bannerLine).hasMatch()) {
        capability.state = FfmpegCapability::State::Incompatible;
        capability.detail = QStringLiteral("%1 is not FFmpeg (banner: %2); point the FFmpeg setting at ffmpeg.exe")
                                .arg(QFileInfo(executable).fileName(), bannerLine);
        AppLogger::warning(QStringLiteral("ffmpeg"), capability.detail);
        return capability;
    }

    const auto version = parseVersionBanner(banner);
    if (version) {
        capability.version = *version;
        if (capability.version.majorVersion() < minimumMajorVersion) {
            capability.state = FfmpegCapability::State::Incompatible;
            capability.detail = QStringLiteral("FFmpeg %1 at %2 is older than the supported minimum %3.0")
                                    .arg(capability.version.toString(), executable,
                                         QString::number(minimumMajorVersion));
            AppLogger::warning(QStringLiteral("ffmpeg"), capability.detail);
            return capability;
        }
        capability.state = FfmpegCapability::State::Available;
        capability.detail = QStringLiteral("FFmpeg %1 at %2 satisfies the M3U8S minimum version %3.0")
                                .arg(capability.version.toString(), executable,
                                     QString::number(minimumMajorVersion));
        AppLogger::info(QStringLiteral("ffmpeg"), capability.detail);
        return capability;
    }

    // Unparseable banner: a date-stamped git build. Those post-date the floor by
    // construction, so accept it and say in the detail why no version is shown.
    capability.state = FfmpegCapability::State::Available;
    capability.detail = QStringLiteral("FFmpeg at %1 reports no numeric version (date-stamped build); "
                                       "the %2.0 version floor was skipped")
                            .arg(executable, QString::number(minimumMajorVersion));
    AppLogger::info(QStringLiteral("ffmpeg"), capability.detail);
    return capability;
}

}
