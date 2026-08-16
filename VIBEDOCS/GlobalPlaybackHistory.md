# Global Playback History

## Scope

Global Playback History is the third built-in entry beside Local Playback and Link Playback. It presents replayable playback occurrences from:

- Emby
- Jellyfin
- WebDAV
- IPTV
- Local Playback
- Link Playback

Its service-selector card uses the same `Built in` status chip as Local
Playback and Link Playback so all application-owned sources are identified
consistently.

An occurrence is persisted only after libmpv reports that playback actually started. A URL validation attempt, media-details request, password prompt, or playback request that fails before that point does not create history. Replaying the same stable media target replaces its previous global-history row, so only the latest occurrence remains.

## Architecture

The module follows the existing layer boundaries:

- `Main.qml` renders the source filters, date groups, progress, availability, replay, deletion, empty state, and pagination controls.
- `AppViewModel` creates playback occurrences, throttles progress updates, resolves source-specific replay requests, and maps safe display data.
- `PlaybackHistoryListModel` exposes presentation roles and stable record ids to QML. It does not expose replay targets.
- `SessionRepository` owns the SQLite schema, migration, privacy filtering, pagination, progress updates, and coordinated deletion.
- Existing source clients resolve Emby, Jellyfin, WebDAV, and IPTV replay targets.
- `PlayerController` remains the only module that calls libmpv.

QML never queries SQLite, constructs service API requests, handles credentials, or calls libmpv.

History rows reserve fixed-width status, duration, and action columns. The
progress track consumes the remaining width, so every visible row has an equal
track length and an equal replay-button size at a given viewport width. Records
without a known duration keep an empty track and display the current position
against `--:--`; completed records render a full track even if their duration was
not reported.

## Persistence Model

The `playback_history` table stores one row per stable media identity. Identity is the combination of source type, service id, and replay target, so equal titles on different services remain independent:

- Stable UUID record id.
- Source type and source service id/name.
- Source-specific replay target.
- Title and optional subtitle.
- Local playback date and UTC start/update timestamps.
- Last known position, duration, and completion state.
- Privacy classification.

Rows are ordered by playback timestamp and id, newest first. Queries are bounded to 60 records per page and use indexes on timestamp and source type. A unique SQLite index enforces the stable-media identity, and a new playback uses `INSERT OR REPLACE` to reset the row to the latest occurrence. Database initialization removes pre-existing duplicates by keeping the newest timestamp before creating the unique index. Existing `link_playback_history` rows are migrated with `INSERT OR IGNORE`; both tables keep the same record id for coordinated deletion and backward compatibility with the dedicated Link Playback page.

Progress writes are throttled in `AppViewModel` instead of writing on every libmpv position signal. Stop and end-of-file events force a final update. A row is complete when playback reaches EOF or at least 97 percent of a known duration. Replaying a completed row starts at zero; an incomplete row resumes from its stored position.

## Replay Targets

Replay targets are stable source identifiers whenever the source provides them:

- Emby and Jellyfin store the media item id. Replay reloads current item details and obtains a fresh playback URL through the existing official client flow, so access tokens and stale signed URLs are not persisted in global history.
- IPTV stores the channel id and reloads the saved playlist before selecting the channel.
- WebDAV stores the remote item URL. Replay accepts it only when its origin and normalized path remain inside the configured WebDAV root, then uses the existing credential/password flow.
- Local Playback stores the canonical local path and verifies that the file still resolves as a playable local video.
- Link Playback stores the normalized HTTP/HTTPS URL and validates it again before replay.

Missing local files and deleted source configurations are marked unavailable before their rows reach QML. Unavailable rows remain visible so users can understand their history, but replay is disabled.

## Privacy And Security

Normal mode excludes private history. Privacy mode includes both normal and private history, matching the existing privacy-card behavior. For configured services, the current service privacy flag takes precedence so moving a card into or out of privacy mode immediately changes history visibility; otherwise the privacy value captured with the occurrence is used.

Deleting a service with local data also deletes its global history. Hiding a service without deleting local data preserves its history rows, but marks them unavailable until the service is enabled again.

Replay targets never enter the QML model. Display addresses remove query and fragment data, media-server tokens are not stored, WebDAV passwords remain in `CredentialStore`, and logs contain only generic lifecycle/error messages.

When clearing `QUrl` components, use explicit `QString {}` arguments. Empty braced arguments are ambiguous between the `QString` and `QUrlQuery` overloads of `QUrl::setQuery` on GCC and break Linux x86_64 and ARM64 builds.

## Media-Server API Basis

Media-server replay reuses the APIs already documented in `EmbyJellyfinApi.md`:

- Emby item details: `GET /Users/{UserId}/Items` with `Ids={ItemId}`.
- Jellyfin item details: `GET /Items/{itemId}` with `userId`.
- Existing direct-play URL resolution and `/Sessions/Playing`, `/Sessions/Playing/Progress`, and `/Sessions/Playing/Stopped` reporting remain unchanged.

Official references:

- Emby Items API: https://dev.emby.media/reference/RestAPI/ItemsService/getUsersByUseridItems.html
- Emby playback reporting: https://dev.emby.media/reference/RestAPI/SessionsService/postSessionsPlaying.html
- Jellyfin stable OpenAPI: https://api.jellyfin.org/openapi/jellyfin-openapi-stable.json

## Testing

`PlaybackHistoryTest` verifies:

- Persistence and deterministic newest-first ordering.
- Normal/private visibility rules.
- Source filtering and pagination.
- Position, duration, and completion updates.
- Stable-id lookup in `PlaybackHistoryListModel`.
- Coordinated deletion.
- Migration of existing Link Playback history without losing encoded replay URLs.
- Replacement of repeated playback for the same stable media target and cleanup of pre-existing duplicates.

`LinkPlaybackHistoryTest` additionally verifies privacy filtering for the legacy page. Manual verification should cover all six sources, completed and resumable rows, unavailable local/source states, private-mode switching, source filters, pagination, deletion, and both mouse and keyboard/remote activation.
