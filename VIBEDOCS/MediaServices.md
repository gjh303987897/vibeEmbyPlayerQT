# Media Services Design

## Layering

加密 HLS 还支持带 CBOR 开头索引的 `.m3u8sp` TAR 单文件容器；容器解析和 Range 读取位于 C++ service 层，QML 不接触 TAR 或密钥。

The first implementation follows:

QML -> `AppViewModel` -> service client -> `NetworkClient` -> Emby / Jellyfin server

QML does not make network requests and does not parse JSON.

## Key Classes

- `NetworkClient`
  - Wraps `QNetworkAccessManager`.
  - Supports JSON `GET` and `POST`.
  - Converts transport, HTTP and timeout failures into `NetworkError`.
  - Keeps certificate verification enabled unless the saved server explicitly allows
    self-signed certificates. The explicit policy ignores only the errors reported for
    that reply and never enters a nested event loop.

- `MediaServiceClient`
  - Abstract interface for login, library loading, item loading, server-wide search, continue-watching loading and item details.

- `MediaServerClientBase`
  - Shared URL building, authorization header construction, response parsing and image URL construction.
  - Parses media-library and item-list JSON on Qt Concurrent workers, then returns model-ready values to the client object's UI thread.

- `EmbyClient`
  - Uses Emby REST API paths, including the current-user root item search endpoint.

- `JellyfinClient`
  - Uses Jellyfin OpenAPI paths, including `GET /Items` for current-user root searches.

- `SessionRepository`
  - Owns SQLite persistence for servers and sessions.
  - Token persistence is intentionally centralized here to make later migration to platform secure storage easier.
  - Stores service cards separately from sessions so one server can have multiple accounts.
  - Service cards expose only non-sensitive data to QML: name, type, host, username, auto-login flag and session availability.

- `AppLogger`
  - Provides the first-version unified logging entry point.
  - Logs operation area, safe host names and counts.
  - Does not log password, token, cookie or token-bearing image URLs.

- `AppViewModel`
  - Owns screen state.
  - Selects Emby or Jellyfin client based on user-selected service type.
  - Exposes `ServiceCardListModel`, `MediaLibraryListModel`, continue-watching `MediaItemListModel`, library item `MediaItemListModel` and an independent media-server search result model to QML.
  - Owns the navigation state for service cards, service home, library item list and item details.

## Service Card Flow

- App launch opens the service-card page.
- Service-card actions are grouped in the upper-right toolbar immediately before
  the window controls as compact monochrome icon buttons. Their 16-pixel optical
  size and muted icon color match the adjacent window controls. Hover feedback
  fades a fixed-color button surface through opacity, avoiding transparent-black
  color interpolation and abrupt foreground changes at pointer entry or exit.
  Keep-alive tasks and history statistics share a stable overflow
  popover so the action order does not shift with window width; every icon-only
  action keeps an accessible name without displaying a hover tooltip. Mouse
  clicks do not retain a focus outline; keyboard tab focus remains available.
- Service cards derive their visual identity from the existing `serviceType` role. Emby, Jellyfin, WebDAV and IPTV each use a local vector-style mark and a stable accent color, so card rendering never depends on remote image assets.
- The reusable QML service icon and status-chip components keep the card hierarchy consistent across light and dark themes: service identity and account details stay in the primary row, while login and session state remain in a fixed footer row.
- Built-in entry cards and saved service cards share the same responsive size contract. The service page selects two or three columns from the available width, calculates one common card width and keeps every card at a fixed 156-pixel height; both the `GridLayout` and `GridView` consume those values so separate sections remain aligned while the window resizes.
- Hover, drag and edit states reuse the same service accent without changing the card's information layout or invoking service-layer logic.
- Opening any built-in service starts from the activated card's mapped window
  rectangle. Saved external services first keep the activated card's loading
  overlay visible while the asynchronous connection and initial page load
  complete; only then does the theme-aware surface expand to the content bounds
  before revealing the destination page. Returning to the service list runs the
  geometry in reverse toward the stored card rectangle. The overlay stays below
  the title bar, blocks repeated input only while visible, and follows the
  global page-transition preference.
- Adding a card stores service name, base URL, username, service type, certificate policy and auto-login preference.
- If a password is provided while saving, the card is logged in immediately and the token is persisted through `SessionRepository`.
- If auto-login is enabled, clicking a card attempts to restore the saved session and opens the service home.
- Service activation sets only the selected card and loading state on the
  click turn. After the feedback has rendered, saved-session restoration,
  WebDAV credential lookup, and IPTV playlist/channel reads run through
  `QtConcurrent` instead of blocking the GUI thread. Each SQLite worker creates
  and destroys its own thread-local connection, matching Qt's SQL threading
  contract. The resulting value data is applied to QML models on the GUI
  thread, while responses from a previously selected server are ignored.
- The initial media-home request fan-out remains deferred until after the home
  page is visible, so home model resets and network startup do not compete with
  the service-card feedback frame.
- While a saved service is opening, only the activated card is dimmed and
  covered by a service-colored spinner and loading label. The card ignores
  repeated clicks until the current operation finishes. The loading state is
  established before editor-bound service properties are synchronized, so a
  cold credential provider, SQLite connection, or first page initialization
  cannot delay the feedback's first frame.
- The first home data load for a saved Emby or Jellyfin service completes while
  the card remains in its loading state. Navigation enters the home page only
  after at least one valid media-data collection is received. If all initial
  collections are empty/invalid, the session is cleared and navigation returns
  to the service-card page with the most relevant error preserved (for example,
  HTTP 522). Partial initial failures do not block entry when another
  collection contains valid data; later refresh failures after a service has
  opened remain on the current page and only update its error state.
- The trendy and traditional media-home trees are both loaded on demand with
  asynchronous QML loaders. An inactive layout does not instantiate hidden
  list delegates or react to home-model resets.
- If auto-login is disabled or no session is available, clicking a card emits a password-required signal; QML shows a password dialog and the password is used only for that login request.
- Deleting a card can either soft-hide the card while preserving local data, or delete the server/session records.

## Service Home

- Emby / Jellyfin service home loads independent home models:
  - Emby suggested series
  - continue watching
  - user libraries
- The shared media home uses Emby suggested series for its cinematic featured
  backdrop and falls back to continue watching when suggestions are unavailable.
  Landscape continue-watching and library rails follow below. See
  `MediaHomeUi.md` for its presentation and interaction contract.
- Emby users can switch between the default trendy home and the original
  traditional home from application settings. The traditional presentation
  uses the standard toolbar, portrait continue-watching rail, and library grid;
  it shares the same service models and commands. Jellyfin remains on the
  trendy presentation.
- The home toolbar exposes an explicit return button, service identity, a long inline server-wide search field, and refresh. The old Home / Libraries / Settings segmented shortcuts are intentionally omitted.
- Search requests stay behind `MediaServiceClient`: `EmbyClient` uses the Emby user-items endpoint and `JellyfinClient` uses the official Jellyfin `GetItems` operation. QML keeps in-progress input local and synchronizes it to `AppViewModel` only on submission, avoiding C++ state notifications during IME composition.
- Emby search excludes individual episodes and requests only movies, series,
  and generic videos. The first page is limited to 36 lightweight result rows;
  details are still fetched through the existing details flow.
- Search results use their own paginated model, reuse the normal media poster and details flow, and retain the result list when returning from details.
- Continue-watching clicks open the item details page. Direct playback from this section is intentionally not implemented yet.
- Continue watching is rendered as a horizontal carousel with touchpad / mouse
  wheel scrolling. Jellyfin retains explicit left / right controls; Emby omits
  them in both layouts.
- Continue-watching episode cards prefer the parent series primary image when the server returns `SeriesId` and `SeriesPrimaryImageTag`; otherwise they fall back to the item primary image.
- Continue-watching cards expose title, parent series name, season / episode text and watched percentage through the C++ model and ViewModel formatting helpers.
- Emby continue-watching requests are sorted by `DatePlayed` descending as
  defined by the official API. The client keeps the first resumable episode
  for each `SeriesId` (falling back to a normalized series name), so one series
  appears only once and represents its most recent record.
- Continue playback uses `UserData.PlaybackPositionTicks` to resume from the server-reported position.
- When a continue-watching card opens the details page, the original resume ticks are retained as a fallback. Some server detail responses may omit or reset `PlaybackPositionTicks`, and the ViewModel must not overwrite a valid resume position from the continue-watching list with zero.

## Item Details

- Item details are fetched through the service layer, not QML.
- Details currently expose title, poster, backdrop, overview, rating, runtime, genres, people cards and playback progress.
- The details page uses the landscape backdrop as a full-width hero image. Presentation-only side and bottom gradients preserve artwork brightness while giving the overlaid title, primary playback action, metadata and overview enough contrast; the normal application toolbar is hidden only on this page and replaced by floating navigation controls. The redundant three-button shortcut row below the title is intentionally omitted.
- Episode hero backgrounds prefer the parent series `ParentBackdropItemId` and all `ParentBackdropImageTags`, so episode thumbnails are not promoted into full-page artwork. Multiple series backdrops rotate every 12 seconds through the existing two-layer crossfade; if the server provides no series backdrop, the series primary poster is the only fallback.
- The cinematic details layout remains theme-aware: its hero gradient terminates in `theme.bg`, lower-page cards and labels use the shared theme tokens, and the primary playback treatment switches between the warm light-on-dark reference style and the normal light-theme primary action color.
- The overview dialog opened from the top-right ellipsis uses the same theme tokens as the details page. Its modal overlay, header/footer surfaces, text hierarchy, close control, scrollbar and primary dismiss action all adapt independently to light and dark modes.
- Primary image tags are read from both the legacy `PrimaryImageTag` field and the current `ImageTags.Primary` map. Image URLs are only created when the server reports a real image tag.
- Episode details also expose season / episode text through the ViewModel.
- Details parse the optional Emby `ImageTags.Logo` title artwork. If an episode has no direct logo, the service layer resolves `ParentLogoItemId` and `ParentLogoImageTag`, which normally point to the parent series logo. QML receives only the resolved logo URL and falls back to the normal item title until the image is ready or when no logo exists. When an episode shows inherited series artwork, its episode name remains visible as a separate text label.
- Series details expose seasons and the selected season's episodes through `MediaItemListModel` instances owned by `AppViewModel`.
- The selected item ID is exposed read-only so QML can highlight the active episode in both the circular episode index and the horizontal landscape episode rail.
- Switching between episodes keeps the loaded season and episode models in place instead of clearing and refetching them. Episode-detail requests use a generation guard so rapid selections cannot apply stale responses, while QML retains the current backdrop until the next image is ready and then crossfades between the two layers.
- Episode thumbnails use lightweight rounded corner covers without adding another runtime QML module, and the horizontal thumbnail rail resolves the selected item ID through `MediaItemListModel` so it can smoothly center the newly selected episode even when that delegate started outside the visible range.
- Episode details opened from continue-watching also expose the parent series seasons and selected season episodes when the server response includes `SeriesId`.
- QML only renders the season selector and episode cards; Emby / Jellyfin season and episode requests stay inside the service layer.
- Episode cards and episode details fall back to the parent series primary image when the episode has no primary image of its own. Failed image requests display the normal placeholder instead of an empty card.
- People are parsed into `MediaPerson` entries with name, role / credited-as text, type and primary image URL. `AppViewModel` exposes them through `PersonListModel` so QML can render horizontal cast cards with a photo above the name and role.

## Library Navigation And Pagination

- Returning from item details reuses the current library model instead of reloading the directory.
- QML requests the next page only after an actual grid movement ends at the bottom; showing the library page again does not count as a pagination action.
- `AppViewModel` marks the directory exhausted when a response contains fewer rows than the requested page size, or when a page contains no new media IDs.
- `MediaItemListModel::appendItems` filters duplicate non-empty media IDs so overlapping server pages cannot create repeated cards.

## Error Handling

C++ service methods use `std::expected`:

- `LoginResult`
- `LibraryResult`
- `ItemResult`
- `NetworkResult`

The ViewModel converts errors into user-facing messages.

## Threading

Requests rely on asynchronous `QNetworkAccessManager`. Potentially large
library/item JSON responses are parsed through Qt Concurrent before their
results are applied to QML-facing models. No network or response parsing work
is performed by QML.

SQLite access is small and synchronous in this phase. Larger cache/index operations should move to a worker in later phases.

## Encrypted HLS

- Local and WebDAV `.m3u8s` items use a dedicated loopback proxy and do not change the existing single-file proxy.
- Root and child playlists are digest-verified and returned without URI rewriting.
- AES-256-GCM TS decryption runs through Qt Concurrent, and plaintext is released only after tag verification succeeds.
- Local package resources are canonicalized, constrained to the package root, and read outside the UI thread.
- TSSL v3 authenticates and recovers the original source basename; v2 remains playback-compatible without name recovery.
- TSSL parsing, local storage, restore/export behavior and package constraints are documented in `EncryptedHlsM3u8s.md`.

## M3U8S Manager

- The service page includes a built-in `M3U8S Video Manager` entry. It is a
  local tool and does not create a saved remote-service card.
- `AppViewModel` owns file dialogs, manager navigation, progress state, and
  TSSL actions. `EncryptedHlsPackager` owns FFmpeg and encryption work, while
  `TsslPackageListModel` exposes only non-secret package metadata to QML.
- Video conversion is asynchronous. FFmpeg runs through `QProcess`, and the
  AES-GCM phase runs through Qt Concurrent, so neither operation blocks the UI
  thread.
- The manager can create a package, cancel active work, open its latest output,
  list local TSSL files, restore/import a TSSL, export a recovery copy, and
  delete a local package.
- TSSL v2/v3 and the root `.m3u8s` carry the same strict 4096-character identifier.
  Playback additionally checks the root-manifest digest before accepting the
  pair.
- WebDAV exports preserve the source folder hierarchy relative to the temporary
  staging root (for example, `SelectedFolder/Subfolder/<package>`). Missing
  remote collections are created parent-first with WebDAV `MKCOL` before their
  files are uploaded.
- The M3U8S batch-management dialog keeps selection lookups in an indexed QML
  set, uses an animated accent selection state, and defers selection cleanup
  until the next opening so canceling a large partial selection does not block
  the dialog close animation.

## Security

- Password is kept only in the login form and cleared after successful login.
- Token is not exposed as a standalone QML property.
- First-version poster URLs may contain `api_key` because QML `Image` cannot attach custom authorization headers.
- Token is saved in SQLite for the first version as an accepted temporary decision.
- Future migration target: Keychain on macOS, Credential Manager on Windows, Secret Service on Linux.
- History statistics resolve privacy from the service's current `servers.private_mode` value. A service moved into privacy mode hides all of its retained history from normal mode, including prior watch records and keep-alive traffic; deleted services fall back to the privacy flag stored with each statistic.
- In normal mode, service cards, keep-alive tasks, and Emby source selectors omit private services. After the user unlocks privacy mode, these views include both normal and private services; private service cards and tasks carry a visible `Private` label. History remains privacy-filtered so private records are only included after the unlock.

## Player Dependency Status

- Windows libmpv development files are installed under `third_party/mpv/dev`.
- Source: GitHub release `zhongfly/mpv-winbuild`, `mpv-dev-x86_64-20260619-git-2d5dfb343a.7z`.
- Download SHA-256: `efb530ca2b36a69c3f5be2d69fadbdf691274b48c0a3963ff771fbf7d9e0f1dd`, matching the release `sha256.txt`.
- The package provides `include/mpv/client.h`, `libmpv.dll.a` and `libmpv-2.dll`.
- CMake links `libmpv.dll.a` with clang-cl/lld and copies `libmpv-2.dll` to the runtime output directory.
- `PlayerController` is the only module that calls libmpv APIs.
- `MpvWidget` owns the native QWidget surface and initializes libmpv with the `wid` option for Window Embedding.
- QML player-page integration and real playback-link flow are still pending.

## Manual Keep-Alive Playback Integration

The service-card page exposes a keep-alive task view for saved Emby sessions. QML edits presentation state only; `AppViewModel` validates input and updates `ScheduledPlaybackTaskListModel`, while `ScheduledPlaybackManager` owns manual start and continuation logic. No automatic timer is active in the current version.

`SessionRepository` stores tasks in `scheduled_playback_tasks`. `EmbyClient` supplies random playable items and existing playback-report APIs. The background task does not alter the selected foreground service or media library models. Headless-player network bytes are attributed to the configured Emby source and written into dedicated keep-alive traffic columns in the daily usage statistics pipeline. History preserves both dimensions: normal versus keep-alive traffic, and download versus upload traffic. The page shows aggregate download/upload cards and the directional split for normal, keep-alive, total, and per-service daily records. Keep-alive duration is not counted as user watch time.

See `ScheduledPlayback.md` for detailed status, persistence, preemption, and failure rules.
