# Emby / Jellyfin API Notes

## Scope

第一阶段只覆盖：

- 用户名 + 密码登录。
- 获取当前用户可访问的媒体库。
- 进入媒体库后获取电影 / 剧集列表。
- 搜索当前 Emby / Jellyfin 用户可访问的全部影片。
- 服务首页继续播放列表。
- 媒体详情页基础信息。

## Official References

- Emby REST API: `https://dev.emby.media/reference/RestAPI/`
- Emby login: `POST /Users/AuthenticateByName`
- Emby user views: `GET /Users/{UserId}/Views`
- Emby user items: `GET /Users/{UserId}/Items`
- Emby server search: `GET /Users/{UserId}/Items` with `SearchTerm` and `Recursive=true`
- Emby continue watching: `GET /Users/{UserId}/Items` with `Filters=IsResumable`
- Emby suggested series: `GET /Users/{UserId}/Suggestions` with `IncludeItemTypes=Series`
- Emby item details: `GET /Users/{UserId}/Items` with `Ids={ItemId}`
- Jellyfin OpenAPI stable: `https://api.jellyfin.org/openapi/jellyfin-openapi-stable.json`
- Jellyfin login operation: `AuthenticateUserByName`
- Jellyfin user views operation: `GetUserViews`
- Jellyfin items operation: `GetItems`
- Jellyfin server search: `GET /Items` with `userId`, `searchTerm` and `recursive=true`
- Jellyfin resume operation: `GetResumeItems`, path `GET /UserItems/Resume`
- Jellyfin suggestions operation: `GetSuggestions`, path `GET /Items/Suggestions`
- Jellyfin item details operation: `GetItem`, path `GET /Items/{itemId}`

## Authentication

Playback discovery uses `GET /Items/{id}/PlaybackInfo` with `UserId` and the saved
authorization headers. Both [Emby's GET reference](https://dev.emby.media/reference/RestAPI/MediaInfoService/getItemsByIdPlaybackinfo.html)
and [Jellyfin 10.10.7's controller](https://github.com/jellyfin/jellyfin/blob/v10.10.7/Jellyfin.Api/Controllers/MediaInfoController.cs)
define the GET without `AutoOpenLiveStream` or `IsPlayback`; these unsupported
query parameters have been removed. Automatic live-source opening belongs to the
POST playback-info lifecycle. This native direct-play path uses the returned
media-source/session identifiers and `static=true`; adaptive transcoding and an
explicit live-source open/close lifecycle are outside its current implementation.
The presence of a query string alone did not prove that GET had opened a live source.

### GUI playback request ownership

`AppViewModel` assigns a separate generation to each media-server playback
request. A new request, playback cleanup, session replacement, or navigation
invalidates the previous generation. Starting local encrypted playback or an
encrypted WebDAV preparation also invalidates the old request immediately,
before preparation finishes. Cancelling a history replay uses the same guard.

PlaybackInfo callbacks validate their generation, server/user identity, and
selected item before changing loading, foreground scheduling, errors, proxy
streams, playback URL, MediaSourceId, PlaySessionId, or page navigation. Stale
successes and failures return silently. Only the pending request's owner clears
its loading/foreground reservation; an old response cannot clear a newer
request's loading state or interrupt its playback. The successful callback
releases ownership before entering the player, so that navigation does not
cancel the playback it has just accepted.

This is a UI lifetime rule, not a server API change. It does not call the shared
client's `cancelRequests()`, which could abort unrelated browsing requests.
`AppViewModelStateTest` uses a local HTTP endpoint with explicitly released
responses to cover both Emby and Jellyfin: return/settings navigation, explicit
cancellation, local and link playback replacement, held encrypted-local
preparation, repeated requests for the same item, another media item, either
completion order, stale success/error, current-request errors, and matching
ItemId / MediaSourceId / PlaySessionId in playback-start reports.

Windows validation on 2026-10-22 reproduced the navigation resurrection and URL
overwrite against the pre-fix binary. All 46 added race cases passed with the
fix, and all 10 selected CTest targets passed (desktop state, QML utilities,
native player host, audit regression, CLI integration, local/link services,
history, encrypted playback proxy, and media-server search). Offscreen runs
skip their native-only UI cases; the native player host was checked separately.
Server race coverage uses a local HTTP fixture, not a live Emby/Jellyfin account
or an SMB share.

Emby:

- Endpoint: `POST /Users/AuthenticateByName`
- Body: `Username`, `Pw`
- Header: `Authorization` or `X-Emby-Authorization`
- Current implementation uses `Authorization: Emby Client=..., Device=..., DeviceId=..., Version=...`
- Authenticated requests include `X-Emby-Token` and `Token` inside the authorization value.

Jellyfin:

- Endpoint: `POST /Users/AuthenticateByName`
- Body: `Username`, `Pw`
- Header scheme used by Jellyfin clients is `MediaBrowser`.
- Authenticated requests include `X-Emby-Token` and `Token` inside the authorization value for compatibility.

Sensitive values:

- Password is never persisted.
- Token is persisted only through `SessionRepository`.
- Windows session persistence uses [user-scoped DPAPI](SessionTokenCipher.md).
- Token, Cookie and password must never be logged.
- Library/item list responses must be JSON objects with an `Items` array (lowercase
  `items` remains compatible). Malformed JSON, absent/wrong arrays and non-object
  entries return `NetworkErrorKind::Parse`; a valid empty array succeeds. Parser
  watchers belong to the client, and pending network callbacks check client lifetime.
- Login/report POST redirects are returned as explicit errors instead of being
  converted into GET requests or forwarding credentials. Configure the final
  server URL when a reverse proxy redirects the API endpoint.

## Libraries

Emby:

- Endpoint: `GET /Users/{UserId}/Views`
- Response shape contains `Items`.

Jellyfin:

- Endpoint: `GET /UserViews`
- Query: `userId`, `includeHidden=false`, `includeExternalContent=false`
- Response shape contains `Items`.

The shared parser maps common fields:

- `Id`
- `Name`
- `CollectionType`
- `Type`
- `PrimaryImageTag`
- `ChildCount`

## Library Items

Emby:

- Endpoint: `GET /Users/{UserId}/Items`
- Query: `ParentId`, `Recursive`, `StartIndex`, `Limit`, `IncludeItemTypes`, `Fields`, `EnableImages`, `EnableUserData`

Jellyfin:

- Endpoint: `GET /Items`
- Query: `userId`, `parentId`, `recursive`, `startIndex`, `limit`, `includeItemTypes`, `fields`, `enableImages`, `enableUserData`

Current item type mapping:

- `movies` collection -> `Movie`
- `tvshows` collection -> `Series`
- other collection types are requested without an item-type filter.

## Media Server Search

Emby 搜索复用官方用户条目接口：

- Endpoint: `GET /Users/{UserId}/Items`
- Query: `SearchTerm`, `Recursive=true`, `StartIndex`, `Limit`, `IncludeItemTypes`, `Fields`, `EnableImages=true`, `EnableUserData=true`

Emby official reference: `https://dev.emby.media/reference/RestAPI/ItemsService/getUsersByUseridItems.html`

Jellyfin 搜索使用官方 `GetItems` 操作：

- Endpoint: `GET /Items`
- Query: `userId`, `searchTerm`, `recursive=true`, `startIndex`, `limit`, `includeItemTypes`, `fields`, `enableImages=true`, `enableUserData=true`
- 请求使用 Jellyfin `MediaBrowser` 鉴权方案，并保留服务现有的 `X-Emby-Token` 兼容头。

Jellyfin official reference: `https://api.jellyfin.org/openapi/jellyfin-openapi-stable.json`, operation `GetItems`

Service behavior:

- Emby uses `IncludeItemTypes=Movie,Series,Video`, so search results contain
  movies, series, and generic videos without individual episodes.
- Emby requests only `PrimaryImageAspectRatio` as an additional field, limits
  image metadata to one primary image, and loads 36 rows per page.
- Jellyfin currently retains `includeItemTypes=Movie,Series,Episode,Video`.
- 两种请求都不传 `ParentId` / `parentId`，因此搜索范围是当前用户可访问的服务器根目录，而不是当前打开的媒体库。
- 搜索结果使用独立的 `MediaItemListModel`，不会覆盖媒体库分页和目录导航状态。
- 新关键词提交时使用请求代数使旧响应失效；详情页返回时恢复原搜索结果和滚动上下文。
- QML 在输入期间保留本地草稿，只在提交时同步到 ViewModel，避免中文输入法组合文本触发跨层状态更新；服务类型对应的路径、参数命名和鉴权方案由 C++ 客户端负责。

## Continue Watching

Emby:

- Endpoint: `GET /Users/{UserId}/Items`
- Query: `Recursive=true`, `Filters=IsResumable`, `IncludeItemTypes=Movie,Episode`, `Limit`, `Fields`, `EnableImages=true`, `EnableUserData=true`

Jellyfin:

- Endpoint: `GET /UserItems/Resume`
- Query: `userId`, `limit`, `includeItemTypes=Movie,Episode`, `fields`, `enableImages=true`, `enableUserData=true`

Continue-watching items open the media details page. Direct play from this section is not part of the current implementation.

`openContinueItem()` shares `m_episodeDetailRequestGeneration` with ordinary
media details and episode switching. It captures a newly incremented generation
when requesting details and checks it before changing loading, errors, the
selected item, people, or season/episode models. Returning or opening another
item invalidates the old generation through the existing `clearSeriesDetails()`
path; both stale successes and stale failures are ignored.

For an accepted response, a missing/non-positive playback position or played
percentage still falls back to the original continue-watching card's progress.
Positive progress from the detail response takes precedence. The shared local
HTTP test fixture can hold item-detail responses as well as PlaybackInfo:
`staleContinueDetails` covers late success/error after returning and switching to
recommendation, search, library, or another continue-watching item, with the new
request pending or completed. `continueDetailsPreservesResumeProgress` covers
missing, zero, and positive server progress, the resulting playback start
position, and errors from the current request, for both Emby and Jellyfin.

Windows validation on 2026-10-22 reproduced the wrong selected item and premature
loading completion before the fix. All 28 added continue-detail cases and the
previous 46 PlaybackInfo race cases passed after the fix. The complete desktop
state target reported 91 passes, no failures, and one native-only offscreen skip;
playback-history and media-server-search targets also passed. These use local
fixtures rather than a live media-server account.

Emby results are requested with `SortBy=DatePlayed` and descending order. The
client keeps only the first resumable episode for each series, so repeated
episodes from one series collapse to its most recent record.

## Suggested Series

Emby:

- Endpoint: `GET /Users/{UserId}/Suggestions`
- Query: `Recursive=true`, `IncludeItemTypes=Series`, `Limit=8`, `Fields`,
  `EnableImages=true`, `ImageTypeLimit=2`,
  `EnableImageTypes=Primary,Backdrop`, `EnableUserData=true`
- The response uses the normal item query-result shape and is parsed into the
  existing `MediaItemListModel`.
- Some Emby-compatible servers return `Studio` or `Genre` entries even when
  `IncludeItemTypes=Series` is present. The client discards every non-Series
  entry. If no valid Series remains, it requests the same user root through
  `GET /Users/{UserId}/Items` with `Recursive=true`,
  `IncludeItemTypes=Series`, and `SortBy=Random`, retaining the same metadata
  and image fields. The fallback response is type-checked again before it is
  exposed to QML.
- This model is used only for the home featured carousel. Empty or failed
  responses do not surface a blocking page error; the ViewModel falls back to
  continue watching.

Official reference: `https://dev.emby.media/reference/RestAPI/SuggestionsService/getUsersByUseridSuggestions.html`

Jellyfin:

- Endpoint: `GET /Items/Suggestions`
- Official operation: `GetSuggestions`
- Query: `userId`, `type=Series`, `startIndex=0`, `limit=8`,
  `enableTotalRecordCount=false`
- The official response schema is `BaseItemDtoQueryResult`, so it reuses the
  normal item parser and the existing `MediaItemListModel`.
- The endpoint does not expose the normal `fields` or image-selection query
  parameters; the client sends only parameters declared by the official
  OpenAPI operation.

Official reference: `https://api.jellyfin.org/openapi/jellyfin-openapi-stable.json`, operation `GetSuggestions`

## Item Details

Emby:

- Endpoint: `GET /Users/{UserId}/Items`
- Query: `Ids={ItemId}`, `Fields=PrimaryImageAspectRatio,Overview,Genres,People,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,BackdropImageTags`, `EnableImages=true`, `EnableUserData=true`

Jellyfin:

- Endpoint: `GET /Items/{itemId}`
- Query: `userId`, `fields=PrimaryImageAspectRatio,Overview,Genres,People,DateCreated,RunTimeTicks,CommunityRating,OfficialRating,BackdropImageTags`

The shared parser maps title, type, year, overview, poster, backdrop, runtime, rating, genres, people and user progress.

## Series Seasons And Episodes

Emby:

- Seasons endpoint: `GET /Shows/{Id}/Seasons`
- Episodes endpoint: `GET /Shows/{Id}/Episodes`
- Episodes are loaded with `SeasonId={SeasonId}` and `UserId={UserId}`.
- Current query fields include overview, genres, people, runtime, ratings, backdrop tags, series primary image tag and user data.

Jellyfin:

- Seasons endpoint: `GET /Shows/{seriesId}/Seasons`
- Episodes endpoint: `GET /Shows/{seriesId}/Episodes`
- Episodes are loaded with `seasonId={seasonId}` and `userId={userId}`.
- The official Jellyfin OpenAPI operation ids are `GetSeasons` and `GetEpisodes`.

ViewModel behavior:

- Series details automatically load seasons after the item detail response succeeds.
- Episode details opened from continue-watching also load seasons when the item detail response includes `SeriesId`.
- The first returned season is selected by default.
- For episode details, `ParentId` is used to select the current season first, with `ParentIndexNumber` as a fallback.
- Selecting a season loads episodes for that season.
- Clicking an episode opens the episode details page and reuses the existing playback URL / progress reporting flow.

## Images

Images use:

- `GET /Items/{ItemId}/Images/Primary`
- `GET /Items/{ItemId}/Images/Logo` for optional title artwork
- `GET /Items/{ItemId}/Images/Backdrop/{Index}` for each reported series backdrop

The current implementation adds:

- `maxWidth`
- `quality`
- `tag`
- `api_key`

Primary image availability is determined from `ImageTags.Primary`, with `PrimaryImageTag` retained as a compatibility fallback. The client does not create a primary-image URL when neither field contains a tag. Episodes without their own primary image use `SeriesPrimaryImageTag` and the series item id as a display fallback.

Emby detail and episode-list requests explicitly include `Logo` in `EnableImageTypes`. The parser prefers `ImageTags.Logo` on the current item, then resolves `ParentLogoItemId` with `ParentLogoImageTag` for inherited series artwork. Missing logo data does not trigger a speculative image request, because the official API returns 404 for image types that the item does not report.

Backdrop parsing retains every `BackdropImageTags` entry. Episode details prefer `ParentBackdropItemId` and `ParentBackdropImageTags` over episode-owned artwork, while series and movie details use their own backdrop list. Each URL includes its required backdrop index and cache tag.

This keeps QML image loading simple while still allowing the server to authorize image access.

Note: because QML `Image` does not attach custom authorization headers, the current first version uses `api_key` in image URLs. A later image proxy/cache layer should remove token-bearing URLs from the QML surface.

## Manual Keep-Alive Random Playback

The Emby manual keep-alive module requests random playable candidates through the official user-items endpoint:

- Endpoint: `GET /Users/{UserId}/Items`
- Query: `Recursive=true`, `Filters=IsNotFolder`, `MediaTypes=Video`, `IncludeItemTypes=Movie,Episode`, `SortBy=Random`, `Limit`
- Fields: `RunTimeTicks`, `SeriesPrimaryImageTag`, `ParentId`

Official reference: `https://dev.emby.media/reference/RestAPI/ItemsService/getUsersByUseridItems.html`

The selected item reuses the existing playback URL flow and the existing `/Sessions/Playing`, `/Sessions/Playing/Progress`, and `/Sessions/Playing/Stopped` reports. Keep-alive playback is intentionally Emby-only until Jellyfin's official OpenAPI behavior is reviewed separately. Manual starts and local-time recurring strategies share the same playback and reporting flow; recurrence calculation remains local and does not add any server API assumptions.
