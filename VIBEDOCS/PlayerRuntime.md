# Player Runtime

## Scope

This document records the first libmpv runtime integration.

The current implementation covers:

- Windows libmpv runtime and development package installation.
- `PlayerController` as the only class that calls libmpv C APIs.
- Qt Quick player page integration through `MpvVideoItem`.
- Emby / Jellyfin direct stream URL generation with resume start position.
- basic playback controls: play, pause, stop, relative seek, absolute seek, volume and speed.
- basic playback state observation: pause state, position, duration, volume, speed and track list.
- subtitle and audio track list exposure to QML.
- player exit confirmation, immersive player fullscreen toggle and Esc behavior.
- Emby / Jellyfin playback start, progress and stop reporting.

The current implementation does not yet cover:

- external subtitle discovery and loading
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

`MpvVideoItem` is a QML-facing `QQuickItem` that creates a native child `QWindow`, keeps it aligned to the Qt Quick item geometry, and passes the child window id to `PlayerController`.

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
- subtitle and audio track switching through libmpv properties

No other module should include `mpv/client.h`.

## Player Page Behavior

The player page uses a full-page native video surface with player chrome floating above the top and bottom edges.

With libmpv Window Embedding, the video surface is a platform-native child window and can cover ordinary QML items that overlap it. The current layout avoids mixing player startup with overlay state: `MpvVideoItem` owns only the native video window, while the top title / exit bar and bottom playback controls live in two narrow transparent Qt Quick tool windows that follow the player page geometry.

The top and bottom chrome windows cover only their own control-bar heights. The center of the video is not covered by a transparent window, so pointer reveal and double-click fullscreen behavior still come from the main player page. The application enables `QQuickWindow::setDefaultAlphaBuffer(true)` before creating QML windows so the floating chrome remains transparent on Windows.

Controls include:

- exit playback
- fullscreen / exit fullscreen
- play / pause
- seek backward 15 seconds
- seek forward 15 seconds
- progress slider
- subtitle menu
- audio track menu
- playback speed menu
- volume slider

Controls use a semi-transparent player chrome. The native video window keeps a fixed full-page geometry whether controls are visible or hidden, so pause, progress, subtitle, audio, speed and volume controls do not resize the video surface. libmpv keeps the video aspect ratio inside that surface.

In normal mode and immersive player fullscreen, the player chrome auto-hides after a short idle delay. Moving or clicking in the video area shows the chrome again. Immersive fullscreen also hides the app's global header, removes page margins and makes the player fill the application content.

Exit playback is guarded by an inline confirmation state in the top player chrome. A separate QML dialog is intentionally avoided because the Window Embedding native video window can cover or intercept QML popups on some platforms. If the user confirms, QML reports playback stopped, calls `MpvVideoItem::stop()` and then `AppViewModel::closePlayerToDetails()`. This hides the embedded native video window immediately, stops mpv, destroys the native video window, clears the current playback URL, preserves the selected media item and returns to the media details page.

`MpvVideoItem` also stops and destroys the native mpv window when:

- its `source` becomes empty
- it leaves the scene
- it becomes invisible
- it is destroyed

This prevents `StackLayout` page retention or navigation away from leaving mpv playback running in the background.

`MpvVideoItem` must not initialize libmpv while the item is hidden or has zero size. If a playback URL arrives before the page is visible, the item records a pending playback request and retries when the item becomes visible or its geometry becomes valid. This prevents the second-playback black-screen case where the server stream is loaded but the native child window is not repainted until a later fullscreen geometry change.

Normal playback exit stops mpv and hides the native child window, but it does not destroy the mpv handle or the child window. The window is reused by the next playback session and is destroyed only when the QML item leaves the scene or is destroyed. `PlayerController` emits `videoOutputChanged()` on libmpv file-loaded, video-reconfig and playback-restart events; `MpvVideoItem` responds by syncing geometry and hide/show/raising the child window. Each successful `loadfile` request also schedules extra native-window refreshes at startup, so the repaint that previously only happened after pressing fullscreen happens automatically even if a platform delays video output events.

Esc behavior:

- In immersive player fullscreen or system fullscreen: exit fullscreen first.
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

During playback, `AppViewModel` reports:

- start: `POST /Sessions/Playing`
- progress: `POST /Sessions/Playing/Progress`
- stop: `POST /Sessions/Playing/Stopped`

The body includes the current item id, `PositionTicks`, direct-play method, seek capability and pause state where appropriate. Progress is reported periodically and after pause, seek, fast-forward and rewind actions.

This is the minimum direct-play path. More advanced playback should later query media sources and choose between static stream, transcoding, HLS, external subtitles, and stream indexes.

## Security Notes

The playback URL currently contains an access token because libmpv receives a URL directly. Logs must not print full playback URLs.

Future work should prefer passing authorization headers to libmpv when practical, or move playback URL construction behind a short-lived local proxy if needed for stricter token isolation.

## Headless Keep-Alive Playback

`PlayerController::initializeHeadless()` creates a separate libmpv handle for manually or automatically started Emby keep-alive playback. It does not set `wid` and uses `force-window=no`, `vo=null`, and `ao=null`, so no video surface or audio output is created.

The headless player emits the same playback-ended position signal used to accumulate actual elapsed time across multiple media items. It remains owned by `ScheduledPlaybackManager`; no manager or QML code calls libmpv directly.

Foreground playback always has priority. `AppViewModel` marks normal player, WebDAV, IPTV, and local verification playback as foreground activity. The scheduler stops and reports its current item, preserves elapsed seconds, waits, and selects another random item after foreground playback ends.
