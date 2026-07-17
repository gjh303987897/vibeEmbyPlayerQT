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
- Emby item details: `GET /Users/{UserId}/Items` with `Ids={ItemId}`
- Jellyfin OpenAPI stable: `https://api.jellyfin.org/openapi/jellyfin-openapi-stable.json`
- Jellyfin login operation: `AuthenticateUserByName`
- Jellyfin user views operation: `GetUserViews`
- Jellyfin items operation: `GetItems`
- Jellyfin server search: `GET /Items` with `userId`, `searchTerm` and `recursive=true`
- Jellyfin resume operation: `GetResumeItems`, path `GET /UserItems/Resume`
- Jellyfin item details operation: `GetItem`, path `GET /Items/{itemId}`

## Authentication

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
- Token, Cookie and password must never be logged.

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

Shared behavior:

- `IncludeItemTypes` / `includeItemTypes` 当前为 `Movie,Series,Episode,Video`，用于覆盖电影、剧集、单集和普通视频。
- 两种请求都不传 `ParentId` / `parentId`，因此搜索范围是当前用户可访问的服务器根目录，而不是当前打开的媒体库。
- 搜索结果使用独立的 `MediaItemListModel`，不会覆盖媒体库分页和目录导航状态。
- 新关键词提交时使用请求代数使旧响应失效；详情页返回时恢复原搜索结果和滚动上下文。
- QML 只展示共用搜索栏和结果状态；服务类型对应的路径、参数命名和鉴权方案由 C++ 客户端负责。

## Continue Watching

Emby:

- Endpoint: `GET /Users/{UserId}/Items`
- Query: `Recursive=true`, `Filters=IsResumable`, `IncludeItemTypes=Movie,Episode`, `Limit`, `Fields`, `EnableImages=true`, `EnableUserData=true`

Jellyfin:

- Endpoint: `GET /UserItems/Resume`
- Query: `userId`, `limit`, `includeItemTypes=Movie,Episode`, `fields`, `enableImages=true`, `enableUserData=true`

Continue-watching items open the media details page. Direct play from this section is not part of the current implementation.

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

The current implementation adds:

- `maxWidth`
- `quality`
- `tag`
- `api_key`

Primary image availability is determined from `ImageTags.Primary`, with `PrimaryImageTag` retained as a compatibility fallback. The client does not create a primary-image URL when neither field contains a tag. Episodes without their own primary image use `SeriesPrimaryImageTag` and the series item id as a display fallback.

This keeps QML image loading simple while still allowing the server to authorize image access.

Note: because QML `Image` does not attach custom authorization headers, the current first version uses `api_key` in image URLs. A later image proxy/cache layer should remove token-bearing URLs from the QML surface.

## Manual Keep-Alive Random Playback

The Emby manual keep-alive module requests random playable candidates through the official user-items endpoint:

- Endpoint: `GET /Users/{UserId}/Items`
- Query: `Recursive=true`, `Filters=IsNotFolder`, `MediaTypes=Video`, `IncludeItemTypes=Movie,Episode`, `SortBy=Random`, `Limit`
- Fields: `RunTimeTicks`, `SeriesPrimaryImageTag`, `ParentId`

Official reference: `https://dev.emby.media/reference/RestAPI/ItemsService/getUsersByUseridItems.html`

The selected item reuses the existing playback URL flow and the existing `/Sessions/Playing`, `/Sessions/Playing/Progress`, and `/Sessions/Playing/Stopped` reports. Keep-alive playback is intentionally Emby-only until Jellyfin's official OpenAPI behavior is reviewed separately. Manual starts and local-time recurring strategies share the same playback and reporting flow; recurrence calculation remains local and does not add any server API assumptions.
