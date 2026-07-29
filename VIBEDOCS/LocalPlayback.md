# Local Playback

## Scope

Local playback is intentionally a lightweight file player. It provides:

- A fixed `Local Playback` card on the media-source page.
- Multiple user-selected video folders.
- One-directory-at-a-time browsing of child folders and supported video files.
- Playback through the existing `PlayerController` and embedded libmpv window.

It does not recursively index folders, scrape metadata, download posters, or create a local media library.

## Architecture

The feature follows the existing UI/ViewModel/service/repository split:

- `Main.qml` displays the fixed card, configured roots, and current directory entries.
- `AppViewModel` owns navigation, path validation, playback context, and UI state.
- `LocalMediaService` enumerates a single directory on `QThreadPool` and filters supported video extensions.
- `LocalMediaRootListModel` and `LocalMediaItemListModel` expose presentation-only list data to QML.
- `SessionRepository` persists roots in the `local_media_roots` SQLite table.

QML never scans the filesystem or invokes libmpv directly.

## Directory Safety

- Added roots are stored as canonical absolute paths.
- Navigation is limited to the selected root and its descendants.
- Symbolic links are not returned as browsable entries, avoiding cycles and navigation outside a configured root.
- Removing a root deletes only its database configuration. It never deletes files or directories from disk.
- Missing or unreadable roots remain visible as unavailable so the user can remove them explicitly.

## Playback Isolation

`AppViewModel::PlaybackOrigin` distinguishes local playback from Emby, Jellyfin, IPTV, and WebDAV playback.

For a local video:

- The file is passed to the existing player as a `file://` URL.
- Playback start/progress/stop is not reported to an Emby or Jellyfin session.
- mpv network-byte callbacks are ignored for usage statistics.
- Leaving the player returns to the same local directory.

## Supported Video Files

The initial extension filter includes common libmpv-supported containers such as MKV, MP4, AVI, MOV, WebM, MPEG, M2TS/MTS, TS, VOB, WMV, FLV, OGV/OGM, 3GP/3G2, ASF, RM, and RMVB.

The extension list controls visibility only. Actual codec/container support remains determined by the bundled libmpv/FFmpeg build.

## Testing

`LocalMediaServiceTest` verifies:

- Directories and supported video files are listed.
- Unrelated files are filtered out.
- Missing directories return a traceable error.
- Asynchronous browsing delivers its result back to the owning thread.

Manual verification should cover adding/removing roots, nested navigation, unavailable paths, local playback, exiting back to the directory, subtitles, audio-track switching, and the existing HTTP/WebDAV/SMB playback regression set.
