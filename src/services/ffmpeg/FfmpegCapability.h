#pragma once

#include <QString>
#include <QStringList>
#include <QVersionNumber>

#include <expected>

// Startup capability probe for the external FFmpeg executable used by M3U8S
// packaging. The executable is intentionally NOT bundled or version-pinned
// (deferred dependency task tracked in TODO.md and deps/libmpv.lock.json),
// so every user machine may carry a different FFmpeg - or none at all. The probe
// answers, once per session, the two questions the UI needs:
//   1. is an FFmpeg reachable at all,
//   2. is it new enough for the packaging pipeline.
// The version floor is the only restriction: whether a build ships a specific
// encoder or muxer is deliberately not checked, because a stripped build reports
// its own failure far better inside the packaging error path than a table scan
// can at startup. Results are logged so field reports carry the user's FFmpeg
// fingerprint. See VIBEDOCS/FfmpegCapability.md.
struct FfmpegCapability final {
    enum class State {
        Available,    // executable found and the version floor satisfied
        Unavailable,  // no executable found, or it could not be run
        Incompatible, // found but older than the supported minimum
    };

    State state { State::Unavailable };
    QString executable;
    QVersionNumber version;
    QString detail; // human-readable probe summary for logs

    bool usable() const { return state == State::Available; }
};

namespace FfmpegCapabilityProbe {

// Synchronous; spawns an ffmpeg child process. Call from a worker thread.
// `executable` is the located ffmpeg path (empty = not found); locating it
// stays the caller's job so this module keeps no link-time dependency on the
// packaging stack.
FfmpegCapability run(const QString& executable);

// Pure parser exposed for tests: turns `ffmpeg -version` banner text into a
// version number (n7.1 / 7.1.1 / git-2024-05-01-abcdef date-stamped builds).
std::expected<QVersionNumber, QString> parseVersionBanner(const QString& bannerOutput);

}
