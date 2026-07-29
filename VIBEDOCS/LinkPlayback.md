# Link Playback

## Scope

Link Playback is a built-in media-source entry beside Local Playback. The first version provides:

- One session-only URL input.
- Direct playback of HTTP and HTTPS media resources.
- Direct playback of remote HLS media and master manifests, normally using an `.m3u8` URL.
- Playback through the existing embedded `MpvVideoItem` and `PlayerController`.
- The normal player controls, buffering state, track selection, fullscreen behavior, and foreground-playback priority.

The first version does not persist links, parse IPTV channel playlists, accept embedded credentials, disable TLS verification, or configure cookies and custom HTTP headers. Multi-channel M3U sources remain part of the IPTV module.

## Architecture

The feature follows the existing UI/ViewModel/service split:

- `Main.qml` displays the fixed source card and the link input page.
- `AppViewModel` owns the input state, playback context, selected-item presentation, navigation, and user-facing error mapping.
- `LinkPlaybackService` strictly validates and normalizes the external URL and derives a display name without exposing query data.
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
- Link playback is not attributed to an IPTV or WebDAV service in usage statistics.
- Starting link playback preempts scheduled headless playback through the existing foreground-playback rule.
- Leaving the player returns to the Link Playback page and retains the input for the current application session.
- TLS verification remains enabled and uses the system CA bundle already configured by `PlayerController`.

Application logs record only generic link-playback operations. libmpv messages containing HTTP URLs or common credential markers are redacted before reaching `AppLogger`.

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

Manual verification should cover an HTTPS MP4, an HLS media playlist, an HLS master playlist with relative child URIs, buffering, seek behavior where supported, subtitle/audio track selection, player exit routing, invalid certificates, and regressions for Local, IPTV, WebDAV, and Emby/Jellyfin playback.
