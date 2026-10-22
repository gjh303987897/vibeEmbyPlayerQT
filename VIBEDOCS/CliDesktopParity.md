# CLI and desktop workflow parity

The CLI exposes desktop business workflows without initializing QML in normal
commands. Command definitions/help are in `CliCommand`; handlers live in
`CliDesktop.cpp`, `CliMedia.cpp` and `CliVideo.cpp`. All commands keep the existing
JSON envelope, exit codes, isolated `--data-dir`, privacy PIN verification and
script deletion confirmation.

## CliDesktop: folders, history, schedules and probes

`local list/add/remove/ls/play` use `SessionRepository`'s `local_media_roots` and
`LocalMediaService`. Canonical root validation and enumeration run on a worker.
Traversal and linked files cannot escape the selected root. Removing a root
removes its configuration only. Local list does not probe slow/offline roots.

`playback-history list/show/remove/clear/stats/play` use the desktop's global
history and daily usage tables. `history` continues to mean Shell input history.
Lists support source/date/pagination; clear requires an explicit date. Hidden
private records cannot be accessed by guessing their IDs. JSON does not include
replay URLs. Replay resolves the current service and credentials; stored absolute
WebDAV URLs must belong to that service's configured root. Completed records
restart at zero, otherwise saved position is used. IPTV reconnects live.

`schedule list/show/add/update/remove` edit the desktop's Emby keep-alive tasks.
Types are manual, daily, weekly, monthly and custom_monthly; times/days follow
`ScheduledPlaybackSchedule`, durations are 5..720 minutes, equivalent schedules
are rejected. A saved Emby login is required, matching the desktop editor.

`iptv streams SERVICE CHANNEL [--probe|--refresh]` reads merged channels and
optionally measures their routes serially. Fresh quality is reused; refresh
forces measurement. Cancellation stops the shared probe before returning 130.
Only metadata and stable stream IDs are returned. This explicit command waits
until its route measurements finish; it does not scan all channels at import.

## CliIptvPlayback: independent playback session

The playback host's `IptvPlayback` class loads the selected channel and ranks it
through the shared selector. A failed route is marked in cache, and a queued
callback loads the next not-yet-tried actual URL. Initial loading without a
playback-start event times out after 20 seconds per route. Each URL is tried once
per automatic round. Manual `player stream ID` starts a new round, while clicking
an already playing route does not reload it. Channel replacement and stop cancel
old probe/fallback callbacks by generation.

`player streams --probe` starts asynchronous detailed probing for this channel;
subsequent status calls read the latest results. Only one shared `IptvProbe`
operates at a time, with 150 ms between routes and the same byte/request/process
limits as the desktop. Ordinary play performs shallow probes of at most 36
routes. Actual PlayerController media information wins over probe results.
Metadata snapshots are coalesced for two seconds and written by one background
repository connection at a time; final cleanup waits for the last snapshot.

The host uses `PlayerController::setLiveStream`, so stale END_FILE events do not
terminate newer routes. It never calls libmpv directly. Non-IPTV media retain the
existing playback/proxy preparation flow. Local/HTTP/WebDAV/Emby/Jellyfin plays
record history only after actual playback starts and update saved progress.
Local playback does not contribute network-service usage, matching the desktop;
network/service playback contributes daily usage and scheduled playback records
its received traffic in the separate keep-alive counters.

## CliScheduleHost: explicit scheduling lifecycle

`schedule watch` starts the existing playback executable in a separate
`--schedule-host` mode with a user-only local socket. `schedule run ID` starts a
manual task there; `status/stop` inspect/stop current work, `unwatch` stops and exits
the host. `schedule resolve --run true|false` handles missed tasks using the desktop
manager. Automatic scheduling requires this host or the desktop to remain open;
it does not install an operating-system service or wake a sleeping computer.

The host directly instantiates `ScheduledPlaybackManager`, its headless
PlayerController and the existing Emby client. It reloads tasks every ten seconds,
validates cached private access every second and stops private work if the PIN
changes. A removed task or a source moved out of accessible services stops on
the next request or one-second check. Secrets remain in memory and user-only IPC. It yields when the CLI
foreground player is active. Foreground desktop playback remains managed by the
desktop's own scheduler.

The shared manager holds `<database>.scheduled-playback.lock` while executing a
task, preventing simultaneous desktop/CLI scheduled playback. Stop, completion,
failure and destruction release it. Automatic policy/checkpoint persistence uses
the same tables/settings as the desktop. `task` still refers to file-processing
jobs, not calendar schedules. No additional distributable executable is needed.

## Shared video settings

`iptv/streamPreference` accepts quality/dataSaver. Video packaging exposes
`--segment-duration 2..30` and saved `m3u8s/segmentDuration` (default six seconds).
The desktop reads/writes the same setting. `--segment-duration` changes only the
current CLI request. Local packaging ignores a saved WebDAV temporary-directory
setting; an explicit `--temp` remains honored and validated. WebDAV output keeps
its existing temporary-directory behavior.

## Verification

`CliIntegrationTest` exercises empty read-only profiles, Chinese local roots,
traversal protection, confirmation, task CRUD/validation, scheduling IPC/lock,
shared HLS probe caching, real missing-primary fallback, manual stream selection,
actual dimensions, metadata persistence, history/replay/stats/privacy and local
packaging with a stale saved temporary path. Existing desktop, scheduler, TLS,
TSSL and transfer regressions remain applicable. Tests use generated media and
local HTTP fixtures; real NAS/provider accounts are not implied by these tests.

Official references: [Qt QProcess](https://doc.qt.io/qt-6/qprocess.html),
[Emby Items API](https://dev.emby.media/reference/RestAPI/ItemsService.html),
[QLockFile](https://doc.qt.io/qt-6/qlockfile.html). No new server API is invented.
