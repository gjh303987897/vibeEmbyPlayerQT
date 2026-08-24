# Link Playback

## Scope

Link Playback is a built-in media-source entry beside Local Playback. The first version provides:

- One URL input whose current value remains available while navigating back from the player.
- Direct playback of HTTP and HTTPS media resources.
- Direct playback of remote HLS media and master manifests, normally using an `.m3u8` URL.
- Persistent playback history grouped by the local playback date, with replay and individual deletion.
- Link-playback download traffic included in the existing daily usage history.
- Playback through the existing embedded `MpvVideoItem` and `PlayerController`.
- The normal player controls, buffering state, track selection, fullscreen behavior, and foreground-playback priority.

The module does not parse IPTV channel playlists, accept embedded credentials, disable TLS verification, or configure cookies and custom HTTP headers. Multi-channel M3U sources remain part of the IPTV module.

## Architecture

The feature follows the existing UI/ViewModel/service split:

- `Main.qml` displays the fixed source card and the link input page.
- `AppViewModel` owns the input state, playback context, selected-item presentation, navigation, and user-facing error mapping.
- `LinkPlaybackService` strictly validates and normalizes the external URL and derives a display name without exposing query data.
- `SessionRepository` stores replayable URLs and timestamps in SQLite.
- `LinkPlaybackHistoryListModel` exposes only record ids, safe display addresses, names, dates, and times to QML; full URLs stay in C++.
- `MpvVideoItem` forwards the normalized URL to the existing `PlayerController`.
- `PlayerController` remains the only module that calls libmpv APIs.

QML does not parse URLs, perform network requests, or call libmpv.

## URL Boundary

Accepted links must:

- Parse as an absolute URL in Qt strict mode.
- Use the `http` or `https` scheme.
- Include a host.
- Fit within the bounded input length.
- Omit URL user information; embedded usernames and passwords are rejected.

File extensions are not used as a media allowlist. CDN and signed playback URLs often have no extension, while libmpv/FFmpeg determines the actual container or HLS format when it opens the URL.

HLS manifests stay remote. This preserves the playlist URL as the base for relative variant, rendition, key, and media-segment URIs as required by RFC 8216. Link Playback does not download an HLS manifest and rewrite it as a local file.

## Playback Isolation

`AppViewModel::PlaybackOrigin::Link` keeps link playback separate from MediaServer, IPTV, WebDAV, and Local playback.

- Link playback does not report start, progress, or stop events to Emby or Jellyfin.
- libmpv network samples are attributed to the stable built-in `Link` source and appear as normal download traffic in daily usage statistics.
- Starting link playback preempts scheduled headless playback through the existing foreground-playback rule.
- Leaving the player returns to the Link Playback page and retains the input for the current application session.
- TLS verification remains enabled and uses the system CA bundle already configured by `PlayerController`.

Application logs record only generic link-playback operations. libmpv messages containing HTTP URLs or common credential markers are redacted before reaching `AppLogger`.

## Playback History

Each link playback that reaches libmpv's started state creates a separate record in `link_playback_history` and the unified `playback_history` table with the same UUID. Invalid URLs and requests that fail before playback starts do not create history.

The legacy link-history row contains:

- A UUID record id.
- The normalized, fully encoded playback URL.
- The user's local calendar date at playback time.
- A UTC timestamp used for deterministic newest-first ordering.
- The privacy-mode state at playback time.

Selecting a history record validates the stored URL again before playback. Replaying the same normalized URL replaces its previous row with the latest occurrence, while different URLs remain independent. Deleting a record removes that URL's latest occurrence. History is not time-pruned.

The complete URL is required for replay and can include signed query parameters. QML receives only a display address with query and fragment removed. Full URLs and query data are never written to application logs. Embedded usernames and passwords remain rejected before persistence. Normal mode excludes private link records; privacy mode includes both normal and private records. Deleting either representation removes both rows in one repository operation so the two history views cannot drift.

## Usage Statistics

`PlayerController` samples libmpv's network cache rate and emits byte deltas through the existing `playbackNetworkBytes` signal. During link playback, `AppViewModel` maps those samples to the stable built-in source:

- Service id: `builtin-link-playback`
- Service type: `Link`
- Traffic category: normal playback traffic

The existing pending-usage buffer, periodic SQLite flush, privacy-mode partition,
configurable history retention (30 days by default), daily history list, and
total download summary are reused unchanged.

## Official References

- Qt `QUrl`: https://doc.qt.io/qt-6/qurl.html
- mpv manual, URL input and `loadfile`: https://mpv.io/manual/master/
- HTTP Live Streaming, RFC 8216: https://datatracker.ietf.org/doc/rfc8216/

## Testing

`LinkPlaybackServiceTest` verifies:

- HTTP direct-media URLs are accepted.
- HTTPS HLS and extensionless URLs are accepted.
- Fragments are removed while encoded queries are preserved.
- Empty, relative, malformed, unsupported-scheme, local-file, and embedded-credential inputs are rejected.
- Display names use only the URL path or host and exclude query data.
- Display addresses remove query data before entering QML.

`LinkPlaybackHistoryTest` verifies:

- SQLite initialization creates the history storage path.
- Distinct URLs are loaded newest first and preserve replay URL encoding.
- Replaying the same normalized URL keeps only its latest occurrence.
- Private occurrences remain hidden in normal mode and become visible in privacy mode.
- One history occurrence can be deleted without removing another.
- The list model resolves records by their stable id.
- Link playback download bytes are stored under the `Link` daily usage source.

Manual verification should cover an HTTPS MP4, an HLS media playlist, an HLS master playlist with relative child URIs, buffering, seek behavior where supported, subtitle/audio track selection, player exit routing, invalid certificates, and regressions for Local, IPTV, WebDAV, and Emby/Jellyfin playback.
