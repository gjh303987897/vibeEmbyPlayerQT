# Local Playback

## Scope

Local playback is intentionally a lightweight file player. It provides:

- A fixed `Local Playback` card on the media-source page.
- Multiple user-selected video folders.
- One-directory-at-a-time browsing of child folders and supported video files.
- Direct playback of one local video dropped anywhere on the main application window.
- Authenticated local playback of encrypted `.m3u8s` HLS packages and indexed `.m3u8sp` TAR containers when their TSSL is available.
- Playback through the existing `PlayerController` and embedded libmpv window.

It does not recursively index folders, scrape metadata, download posters, or create a local media library.

## Architecture

The feature follows the existing UI/ViewModel/service/repository split:

- `Main.qml` displays the fixed card, configured roots, and current directory entries.
- `AppViewModel` owns navigation, path validation, playback context, and UI state.
- `LocalMediaService` resolves, validates, and enumerates a single directory on `QThreadPool`, then filters supported video extensions.
- `LocalMediaService` also validates a dropped URL as a canonical, readable, supported local video file before playback.
- `LocalMediaRootListModel` and `LocalMediaItemListModel` expose presentation-only list data to QML.
- `SessionRepository` persists roots in the `local_media_roots` SQLite table.

QML never scans the filesystem or invokes libmpv directly.

The add-folder flow uses the non-blocking Qt Quick `FolderDialog`. After the
user accepts a folder, canonical-path resolution, readability checks, root
boundary validation, and directory enumeration all run on the worker pool.
The UI thread only persists the validated root and updates the list models.
This avoids intermittent UI stalls caused by slow disks, disconnected paths,
or Windows shell/file-system calls. Stored roots are not synchronously probed
when the local page opens; a failed asynchronous browse marks that root as
unavailable until the next refresh or successful selection.

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

For `.m3u8s`, `AppViewModel` asynchronously asks
`EncryptedHlsPlaybackProxy` to verify the root digest, identifier, encrypted
source basename, and local TSSL. libmpv receives a localhost `.m3u8` URL while
the proxy reads package files on worker threads and releases TS plaintext only
after complete AES-GCM tag verification. The real manifest path is retained
separately for history replay. Ordinary local videos keep the direct `file://`
path.

A dropped video is not indexed, copied, or added to the configured root list. It uses the same local playback context, but leaving the player returns to the media-source page because there is no active browsed directory to restore.

## Supported Video Files

The extension filter includes `.m3u8s` and common libmpv-supported containers such as MKV, MP4, AVI, MOV, WebM, MPEG, M2TS/MTS, TS, VOB, WMV, FLV, OGV/OGM, 3GP/3G2, ASF, RM, and RMVB. Generated `segment_NNNNNN.ts` files are hidden when browsing an M3U8S package directory.

The extension list controls visibility only. Actual codec/container support remains determined by the bundled libmpv/FFmpeg build.

## Testing

`LocalMediaServiceTest` verifies:

- Directories and supported video files are listed.
- Unrelated files are filtered out.
- Missing directories return a traceable error.
- A requested directory outside its configured canonical root is rejected.
- Asynchronous browsing delivers its result back to the owning thread.
- Dropped local video URLs resolve to canonical paths while unsupported and remote URLs are rejected.
- `.m3u8s` manifests are discoverable while generated encrypted TS segments are not presented as standalone videos.

Manual verification should cover adding/removing roots, nested navigation, unavailable paths, directory playback, dropping supported and unsupported files, exiting back to the expected page, manual external subtitles, audio-track switching, and the existing HTTP/WebDAV/SMB playback regression set.
