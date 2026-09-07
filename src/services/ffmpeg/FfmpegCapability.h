#pragma once

#include <QString>
#include <QStringList>
#include <QVersionNumber>

#include <expected>

// Startup capability probe for the external FFmpeg executable used by M3U8S
// packaging. The executable is intentionally NOT bundled or version-pinned
// (deferred dependency task tracked in TODO.md and deps/libmpv.lock.json),
// so every user machine may carry a different FFmpeg - or none at all. This
// probe answers, once per session, the three questions the UI needs:
//   1. is an FFmpeg reachable at all,
//   2. is it new enough for the packaging pipeline,
//   3. does it actually contain the encoders/muxers the pipeline requires.
// Results are logged so field reports carry the user's FFmpeg fingerprint.
// See VIBEDOCS/FfmpegCapability.md.
struct FfmpegCapability final {
    enum class State {
        Available,   // executable found, version satisfied, features complete
        Unavailable, // no executable found
        Incompatible, // found but too old or missing required features
    };

    State state { State::Unavailable };
    QString executable;
    QVersionNumber version;
    QStringList missingFeatures;
    QString detail; // human-readable probe summary for logs

    bool usable() const { return state == State::Available; }
};

namespace FfmpegCapabilityProbe {

// Synchronous; spawns ffmpeg child processes. Call from a worker thread.
// `executable` is the located ffmpeg path (empty = not found); locating it
// stays the caller's job so this module keeps no link-time dependency on the
// packaging stack.
FfmpegCapability run(const QString& executable);

// Pure parser exposed for tests: turns `ffmpeg -version` banner text into a
// version number (n7.1 / 7.1.1 / git-2024-05-01-abcdef date-stamped builds).
std::expected<QVersionNumber, QString> parseVersionBanner(const QString& bannerOutput);

// Pure classifier exposed for tests: matches encoders/muxers lists against
// the packaging pipeline requirements.
QStringList missingRequiredFeatures(const QStringList& encoders, const QStringList& muxers);

}
