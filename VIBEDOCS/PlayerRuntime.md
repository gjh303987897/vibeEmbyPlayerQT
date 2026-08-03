# Player Runtime

## Scope

This document records the first libmpv runtime integration.

The current implementation covers:

- Windows libmpv runtime and development package installation.
- `PlayerController` as the only class that calls libmpv C APIs.
- Qt Quick player page integration through `MpvVideoItem`.
- Emby / Jellyfin direct stream URL generation with resume start position.
- HTTP/HTTPS direct-media and HLS link playback with strict URL validation, persistent replay history, and daily traffic accounting.
- basic playback controls: play, pause, stop, relative seek, absolute seek, volume and speed.
- basic playback state observation: pause state, position, duration, volume, speed and track list.
- subtitle and audio track list exposure to QML.
- manual loading and immediate selection of a local external subtitle file.
- player exit confirmation, immersive player fullscreen toggle and Esc behavior.
- Emby / Jellyfin playback start, progress and stop reporting.
- Unified playback history for Emby, Jellyfin, WebDAV, IPTV, local files, and direct links.

The current implementation does not yet cover:

- automatic external subtitle discovery
- rich playback error mapping
- WebDAV / SMB / IPTV playback validation

## Runtime Package

Windows runtime files are stored under `third_party/mpv`.

- Source release: `zhongfly/mpv-winbuild`
- Dev asset: `mpv-dev-x86_64-20260619-git-2d5dfb343a.7z`
- Dev SHA-256: `efb530ca2b36a69c3f5be2d69fadbdf691274b48c0a3963ff771fbf7d9e0f1dd`
- Runtime asset: `mpv-x86_64-20260619-git-2d5dfb343a.7z`
- Runtime SHA-256: `eaa0479b67270b5a1d3f0c6d9a5b6b5749322e5e8848bba544b921669d5d207a`

The development package provides:

- `include/mpv/client.h`
- `libmpv.dll.a`
- `libmpv-2.dll`

CMake links `libmpv.dll.a` and copies `libmpv-2.dll` into the executable output directory on Windows.

## Window Embedding

The project uses libmpv Window Embedding through the `wid` option.

`MpvVideoItem` is a QML-facing `QQuickItem` that creates a platform-native video host, keeps it aligned to the Qt Quick item geometry, and passes the host window id to `PlayerController`. Windows uses a real `WS_CHILD` HWND owned by the main Qt Quick window; other platforms use a child `QWindow`.

QML owns only the page layout and buttons. It does not call libmpv directly.

`PlayerController` owns:

- mpv handle creation and destruction
- `wid` option setup
- play, pause, resume, stop
- relative seek and absolute seek
- volume
- speed
- playback property observation through `mpv_observe_property`
- subtitle and audio track parsing from `track-list`
- audio tag parsing from the libmpv `metadata` node map, with stale values cleared before each replacement load
- subtitle and audio track switching through libmpv properties
- asynchronous external subtitle loading through libmpv `sub-add`

No other module should include `mpv/client.h`.

## Player Page Behavior

The player page uses a full-page native video surface with player chrome floating above the top and bottom edges.

With libmpv Window Embedding, the video surface is a platform-native child window and can cover ordinary QML items that overlap it. The current layout avoids mixing player startup with overlay state: `MpvVideoItem` owns only the native video window, while the top title / exit bar and bottom playback controls live in two narrow transparent Qt Quick tool windows that follow the player page geometry.

The top and bottom chrome windows cover only their own control-bar heights. The center of the video is not covered by a transparent window, so pointer reveal and double-click fullscreen behavior still come from the main player page. The application enables `QQuickWindow::setDefaultAlphaBuffer(true)` before creating QML windows so the floating chrome remains transparent on Windows.

The player view deliberately skips the application's translated/scaled page-entry animation. The embedded video surface and chrome bars are separate native windows, while the animation transforms only the Qt Quick item hierarchy. Calculating their global geometry during that transform would leave controls offset after opening playback until an unrelated main-window move or resize resynchronized them. Other pages retain the normal transition animation, and the player chrome is raised and resynchronized after the stable player layout is selected.

Interactive window resizing uses a retargetable, critically damped native geometry animation. Qt Quick and `QWindow` geometry notifications update one continuously running target; the host follows at approximately 60 frames per second with a short 75 ms smoothing constant, then snaps to the final pixel when both distance and velocity are negligible. The motion has no overshoot or bounce, remains responsive when the user reverses direction, and uses a black QML backing surface so growth never exposes the normal page background. On Windows the host class deliberately omits `CS_HREDRAW` and `CS_VREDRAW`, and ordinary animation frames do not force invalidation; this lets mpv resize its own child surface without a full-window erase between frames. Initial visibility, file-load, and video-reconfiguration events retain the immediate refresh path needed for delayed video-output initialization.

Controls include:

- exit playback
- fullscreen / exit fullscreen
- play / pause
- seek backward 15 seconds
- seek forward 15 seconds
- progress slider
- subtitle menu
- load external subtitle
- audio track menu
- playback speed menu
- volume slider

Progress sliders keep a local preview position while the pointer is held. They submit one absolute exact seek when the pointer is released instead of sending an exact seek for every drag movement. The preview remains stable until libmpv reports `playback-restart` (or the seek timeout fires), preventing asynchronous `time-pos` updates from pulling the handle back and avoiding repeated native-window refreshes during a drag.

Controls use a semi-transparent player chrome. The native video window keeps a fixed full-page geometry whether controls are visible or hidden, so pause, progress, subtitle, audio, speed and volume controls do not resize the video surface. libmpv keeps the video aspect ratio inside that surface.

The subtitle menu can open even when the current video has no subtitle tracks. Its load action opens Qt Quick's asynchronous file dialog. `MpvVideoItem` accepts only a local file URL, and `PlayerController` canonicalizes and verifies the file before issuing an asynchronous libmpv `sub-add` command with the `select` flag. libmpv remains responsible for subtitle parsing and adds a successful load to the observed `track-list`; QML never calls libmpv directly.

In normal mode and immersive player fullscreen, the player chrome auto-hides after a short idle delay. Moving or clicking in the video area shows the chrome again. Immersive fullscreen also hides the app's global header, removes page margins and makes the player fill the application content.

Exit playback is guarded by an inline confirmation state in the top player chrome. A separate QML dialog is intentionally avoided because the Window Embedding native video window can cover or intercept QML popups on some platforms. If the user confirms, QML reports playback stopped, calls `MpvVideoItem::stop()` and then `AppViewModel::closePlayerToDetails()`. This hides the embedded native video window immediately, stops mpv, destroys the native video window, clears the current playback URL, preserves the selected media item and returns to the media details page.

In embedded-video mode, `MpvVideoItem` also stops and destroys the native mpv window when:

- its `source` becomes empty
- it leaves the scene
- it becomes invisible
- it is destroyed

This prevents `StackLayout` page retention or navigation away from leaving video playback running in the background. Audio-only WebDAV playback is the deliberate exception: it has no native video window and remains active when the full player page becomes invisible so the same controller can power the mini player.

`MpvVideoItem` must not initialize libmpv while the item is hidden or has zero size. If a playback URL arrives before the page is visible, the item records a pending playback request and retries when the item becomes visible or its geometry becomes valid. This prevents the second-playback black-screen case where the server stream is loaded but the native child window is not repainted until a later fullscreen geometry change.

Normal playback exit stops mpv and hides the native child window, but it does not destroy the mpv handle or the child window. The window is reused by the next playback session and is destroyed only when the QML item leaves the scene or is destroyed. `PlayerController` emits `videoOutputChanged()` on libmpv file-loaded and video-reconfig events; `MpvVideoItem` responds by syncing geometry and hide/show/raising the child window. A playback-restart event still completes seek/loading state, but it does not invalidate or reposition the native child window because an ordinary seek does not rebuild the video output. Each successful `loadfile` request also schedules extra native-window refreshes at startup, so the repaint that previously only happened after pressing fullscreen happens automatically even if a platform delays video output events.

Esc behavior:

- In immersive player fullscreen or system fullscreen: exit fullscreen first.
- During WebDAV audio playback: minimize the full audio page and keep playback active.
- Outside fullscreen: open the exit playback confirmation dialog.

The player page must not print or display token-bearing playback URLs.

## Playback URL Flow

The detail page calls `AppViewModel::playSelectedItem()`.

`AppViewModel` asks the active `MediaServiceClient` for a playback request and switches to the `player` view if URL generation succeeds.

`EmbyClient` and `JellyfinClient` use:

- `/Videos/{id}/stream`
- `static=true`
- `api_key=<token>`

The playback request also carries a resume start position derived from `UserData.PlaybackPositionTicks`. `MpvVideoItem` passes this to libmpv as a file-local `start=<seconds>` option when loading the URL.

For HTTPS playback, `PlayerController` configures libmpv with the PEM bundle produced by `TlsCertificateStore`. The bundle mirrors the operating-system trusted CA certificates exposed by Qt, allowing libmpv's FFmpeg/OpenSSL backend to validate the same public certificate authorities used by the application network layer. TLS verification remains enabled by default. A server saved with the explicit self-signed-certificate trust option disables verification only for that server's foreground or scheduled playback request.

During playback, `AppViewModel` reports:

- start: `POST /Sessions/Playing`
- progress: `POST /Sessions/Playing/Progress`
- stop: `POST /Sessions/Playing/Stopped`

The body includes the current item id, `PositionTicks`, direct-play method, seek capability and pause state where appropriate. Progress is reported periodically and after pause, seek, fast-forward and rewind actions.

The same player lifecycle feeds Global Playback History. A history occurrence is created only after libmpv reports playback started. Progress updates are throttled before reaching SQLite, while stop and playback-ended events persist the final position. End of file, or reaching at least 97 percent of a known duration, marks the occurrence complete.

This is the minimum direct-play path. More advanced server playback should later query media sources and choose between static stream, transcoding, HLS, server-provided external subtitles, and stream indexes.

## Security Notes

The playback URL currently contains an access token because libmpv receives a URL directly. Logs must not print full playback URLs.

Future work should prefer passing authorization headers to libmpv when practical, or move playback URL construction behind a short-lived local proxy if needed for stricter token isolation.

## Headless Keep-Alive Playback

`PlayerController::initializeHeadless()` creates a separate libmpv handle for manually or automatically started Emby keep-alive playback. It does not set `wid` and uses `force-window=no`, `vo=null`, and `ao=null`, so no video surface or audio output is created.

`MpvVideoItem::audioOnly` uses the same initialization path with audio output explicitly enabled for interactive WebDAV music playback. This variant still uses `force-window=no` and `vo=null`, but leaves `ao` on libmpv's normal output so cover art or other video tracks cannot create a native player window while audio remains audible. Switching between audio-only and embedded-video playback tears down and reinitializes the libmpv handle because these options are pre-initialization options.

Embedded audio artwork stays inside the player layer. `PlayerController` recognizes the official `track-list/N/albumart` flag, requests the decoded cover frame with `screenshot-raw video rgba`, validates dimensions and byte layout, bounds the stored image to 1024 pixels, and exposes only a temporary local `QUrl` through `MpvVideoItem`. The previous temporary image is removed before a replacement track is shown, so QML never downloads or parses the source audio file itself.

The full WebDAV audio page and draggable mini player share this single `MpvVideoItem`. Returning from the full page changes only `AppViewModel::currentView`; it does not clear the playback URL or create a second mpv instance. The mini player remains synchronized with pause, position, duration, loading, buffering, queue changes, embedded audio metadata and cover artwork, and only its explicit exit action invokes the normal stop-and-clear path.

Audio loading uses libmpv file lifecycle events instead of the video loading overlay. `MPV_EVENT_START_FILE` resets stale position/duration and starts the indicator; `MPV_EVENT_FILE_LOADED` or `MPV_EVENT_PLAYBACK_RESTART` clears it. When `loadfile replace` switches tracks, the old file's `MPV_END_FILE` with `STOP` reason does not clear the new request's loading state or network accounting.

`PlayerController::playbackEnded` reports the final position together with whether libmpv reached EOF and whether playback failed. The WebDAV audio queue advances only on EOF or failure, so a user-initiated stop or a `loadfile replace` during manual track selection cannot accidentally skip another track.

The headless player emits the same playback-ended position signal used to accumulate actual elapsed time across multiple media items. It remains owned by `ScheduledPlaybackManager`; no manager or QML code calls libmpv directly.

Foreground playback always has priority. `AppViewModel` marks normal player, WebDAV, IPTV, link, and local verification playback as foreground activity. The scheduler stops and reports its current item, preserves elapsed seconds, waits, and selects another random item after foreground playback ends.

Foreground link playback reuses the normal `playbackNetworkBytes` sampling path. Its received bytes are assigned to the built-in `Link` usage source, while its watch time and playback lifecycle remain isolated from Emby/Jellyfin reporting.
