# Media Services Design

## Layering

The first implementation follows:

QML -> `AppViewModel` -> service client -> `NetworkClient` -> Emby / Jellyfin server

QML does not make network requests and does not parse JSON.

## Key Classes

- `NetworkClient`
  - Wraps `QNetworkAccessManager`.
  - Supports JSON `GET` and `POST`.
  - Converts transport, HTTP and timeout failures into `NetworkError`.
  - Emits a certificate confirmation signal before accepting self-signed certificates.

- `MediaServiceClient`
  - Abstract interface for login, library loading, item loading, continue-watching loading and item details.

- `MediaServerClientBase`
  - Shared URL building, authorization header construction, response parsing and image URL construction.

- `EmbyClient`
  - Uses Emby REST API paths.

- `JellyfinClient`
  - Uses Jellyfin OpenAPI paths.

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
  - Exposes `ServiceCardListModel`, `MediaLibraryListModel`, continue-watching `MediaItemListModel` and library item `MediaItemListModel` to QML.
  - Owns the navigation state for service cards, service home, library item list and item details.

## Service Card Flow

- App launch opens the service-card page.
- Adding a card stores service name, base URL, username, service type, certificate policy and auto-login preference.
- If a password is provided while saving, the card is logged in immediately and the token is persisted through `SessionRepository`.
- If auto-login is enabled, clicking a card attempts to restore the saved session and opens the service home.
- If auto-login is disabled or no session is available, clicking a card emits a password-required signal; QML shows a password dialog and the password is used only for that login request.
- Deleting a card can either soft-hide the card while preserving local data, or delete the server/session records.

## Service Home

- Emby / Jellyfin service home loads two independent sections:
  - continue watching
  - user libraries
- Continue-watching clicks open the item details page. Direct playback from this section is intentionally not implemented yet.
- Continue watching is rendered as a horizontal carousel with left / right scroll buttons and touchpad / mouse wheel scrolling.
- Continue-watching episode cards prefer the parent series primary image when the server returns `SeriesId` and `SeriesPrimaryImageTag`; otherwise they fall back to the item primary image.
- Continue-watching cards expose title, season / episode text and watched percentage through the C++ model and ViewModel formatting helpers.
- Continue playback uses `UserData.PlaybackPositionTicks` to resume from the server-reported position.
- When a continue-watching card opens the details page, the original resume ticks are retained as a fallback. Some server detail responses may omit or reset `PlaybackPositionTicks`, and the ViewModel must not overwrite a valid resume position from the continue-watching list with zero.

## Item Details

- Item details are fetched through the service layer, not QML.
- Details currently expose title, poster, backdrop, overview, rating, runtime, genres, people cards and playback progress.
- Primary image tags are read from both the legacy `PrimaryImageTag` field and the current `ImageTags.Primary` map. Image URLs are only created when the server reports a real image tag.
- Episode details also expose season / episode text through the ViewModel.
- Series details expose seasons and the selected season's episodes through `MediaItemListModel` instances owned by `AppViewModel`.
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

The first version relies on `QNetworkAccessManager` asynchronous requests.
No network work is performed by QML.

SQLite access is small and synchronous in this phase. Larger cache/index operations should move to a worker in later phases.

## Security

- Password is kept only in the login form and cleared after successful login.
- Token is not exposed as a standalone QML property.
- First-version poster URLs may contain `api_key` because QML `Image` cannot attach custom authorization headers.
- Token is saved in SQLite for the first version as an accepted temporary decision.
- Future migration target: Keychain on macOS, Credential Manager on Windows, Secret Service on Linux.

## Player Dependency Status

- Windows libmpv development files are installed under `third_party/mpv/dev`.
- Source: GitHub release `zhongfly/mpv-winbuild`, `mpv-dev-x86_64-20260619-git-2d5dfb343a.7z`.
- Download SHA-256: `efb530ca2b36a69c3f5be2d69fadbdf691274b48c0a3963ff771fbf7d9e0f1dd`, matching the release `sha256.txt`.
- The package provides `include/mpv/client.h`, `libmpv.dll.a` and `libmpv-2.dll`.
- CMake links `libmpv.dll.a` with clang-cl/lld and copies `libmpv-2.dll` to the runtime output directory.
- `PlayerController` is the only module that calls libmpv APIs.
- `MpvWidget` owns the native QWidget surface and initializes libmpv with the `wid` option for Window Embedding.
- QML player-page integration and real playback-link flow are still pending.
