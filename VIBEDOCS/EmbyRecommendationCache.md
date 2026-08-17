# Emby 推荐缓存

## 目标

`EmbyRecommendationCache` 为 Emby 首页推荐提供可复用的缓存编解码、每日刷新判断和类型过滤能力。推荐网络请求仍由 `EmbyClient` 执行，页面只通过 `AppViewModel` 读取状态和触发操作。

## 数据流

1. `AppViewModel` 进入 Emby 首页时，以服务器 ID 和用户 ID 查询 `emby_recommendation_cache`。
2. 如果缓存更新时间与本地当前日期相同，直接展示缓存，不发起推荐请求。
3. 如果缓存已过期，先展示旧缓存，再异步请求 Emby `GET /Users/{UserId}/Suggestions`。
4. 手动刷新会跳过同日缓存判断。没有活动 Emby 会话时，清除缓存，使下一次进入 Emby 时强制刷新。
5. 请求成功后保存未过滤的推荐，界面展示时再应用用户设置的排除类型。

缓存按 `(server_id, user_id)` 隔离。删除服务本地数据时，同时删除该服务的推荐缓存。

## 安全边界

Emby 图片 URL 通常包含 `api_key`。序列化时会移除大小写不敏感的 `api_key` 和 `X-Emby-Token` 查询参数；反序列化时使用当前会话令牌重新生成图片 URL。因此 SQLite 缓存不保存会话令牌，也不会在用户重新登录后继续使用旧令牌。

## 类型过滤

`EmbyClient::fetchSeriesGenres` 优先使用官方 `GET /Items/Filters` 接口，并传入当前 `UserId` 与 `IncludeItemTypes=Series` 获取可用剧集流派。若服务器未返回该字段，则兼容回退到官方 `GET /Genres` 接口；缓存推荐条目自带的 `Genres` 也会立即合并到可选项中。流派请求即使在用户离开 Emby 页面后才完成，结果仍会写入设置缓存，避免异步会话切换丢失数据。设置页以多选框展示流派，勾选代表从推荐中排除。

已获取的可选流派保存在 QSettings，并与已排除项合并，因此设置页没有活动 Emby 会话时仍可编辑现有选项。匹配忽略大小写，但要求完整流派名称一致，避免把 `Action` 错误匹配到 `Live Action`。

过滤发生在本地未过滤缓存上，不会触发网络请求。为保证排除后仍有足够内容，每日请求最多缓存 32 项，首页最多展示前 8 项未被排除的推荐。

## 官方接口依据

- Emby Suggestions Service: `GET /Users/{UserId}/Suggestions`，通过 `Fields=Genres` 获取推荐条目的流派。
- Emby Filtering: `GET /Items/Filters`，通过 `UserId` 与 `IncludeItemTypes=Series` 获取可选流派。
- Emby Genres Service: `GET /Genres`，作为部分服务器未从过滤接口返回流派时的兼容路径。
- Emby Item Information: `Genres` 是条目元数据字段。

Suggestions API 的 `Genres` 查询参数用于包含筛选，没有排除语义，因此排除过滤由客户端完成。
