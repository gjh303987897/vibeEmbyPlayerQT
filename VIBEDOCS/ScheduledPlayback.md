# Manual Emby Keep-Alive Playback

## Purpose

保号任务模块由用户手动立即启动，为指定 Emby 播放源执行无窗口、无声音的后台播放。主要用途是维持服务器账号活跃状态，同时避免干扰用户正常观看。

当前边界：

- 仅支持已保存登录会话的 Emby 服务卡片。
- 当前版本不提供定时触发，也不会在应用启动时自动播放。
- 用户只需选择播放源和目标时长，然后点击“立即开始”。
- 播放配置可保存，之后可再次手动启动。
- 后台播放器流量计入所选 Emby 服务的历史统计，但不把保号时长计入用户观看时长。

## Architecture

调用链为：

`ScheduledTasksPage (QML)` -> `AppViewModel` -> `ScheduledPlaybackManager` -> `EmbyClient` / `SessionRepository` / `PlayerController`

职责边界：

- QML 只负责展示任务、编辑表单、状态和进度。
- `AppViewModel` 负责输入校验、模型刷新和前台播放状态转发。
- `ScheduledPlaybackManager` 负责手动任务执行、随机选片、跨视频累计时长、失败重试和前台播放抢占。
- `SessionRepository` 负责 SQLite 持久化。
- `EmbyClient` 负责官方 REST API 请求和播放状态上报。
- `PlayerController` 仍然是唯一调用 libmpv C API 的模块。

后台 `PlayerController::playbackNetworkBytes` 由 `ScheduledPlaybackManager` 连同对应的 `ServerConfig` 转发给 `AppViewModel`，复用前台统计的批量聚合和 `SessionRepository::addDailyUsage()` 写入路径。流量按 1 MB 阈值或 15 秒定时器落库；任务完成、失败或手动停止时会强制写入剩余流量，历史页面打开时会立即刷新。

历史统计会区分正常流量和保号流量：`network_bytes_in/out` 仅保存正常请求及前台播放流量，`keep_alive_network_bytes_in/out` 仅保存后台保号播放器流量。每日记录和 30 天汇总会分别显示两类流量，并额外显示两者合计。数据库升级只为新字段补充零值；升级前已经混合写入正常字段的历史保号流量无法可靠反向识别，因此仍按正常流量保留。

保号任务列表和播放源选择器必须遵守隐私模式边界。普通模式只加载 `servers.private_mode = 0` 的任务与 Emby 会话；用户通过 PIN 进入隐私模式后，只加载 `servers.private_mode = 1` 的任务与播放源。进入或退出隐私模式，以及修改服务的隐私属性时，都会停止当前后台任务并刷新模型，防止服务名称、任务状态或编辑上下文跨模式残留。

## Persistence

SQLite 表 `scheduled_playback_tasks` 保存：

- `id`
- `server_id`
- `start_time`，手动任务固定保存为 `manual`
- `duration_minutes`
- `enabled`
- `last_run_date`，手动任务保持为空
- `created_at`
- `updated_at`

`server_id` 通过外键关联服务卡片。删除服务及其本地数据时，对应任务会级联删除。加载任务时只返回仍然启用且类型为 Emby 的服务。

`start_time`、`enabled` 和 `last_run_date` 暂时保留是为了兼容已经创建的 SQLite 数据库，不参与当前版本的自动调度。后续重新设计定时功能时应通过正式迁移调整表结构。

`last_run_date` 是 `NOT NULL` 列。手动模式必须保存非 null 的空字符串；`SessionRepository` 会在持久化边界把 null `QString` 规范化为 `""`，避免 SQLite 约束错误。

## Manual Start Rules

1. 用户在编辑器中点击“立即开始”时，配置先保存，再启动后台播放。
2. 用户也可以从已保存配置卡片中点击“立即开始”。
3. 如果有前台播放，任务进入 `waiting` 状态，前台播放结束后自动开始或恢复。
4. 同一时刻只运行一个后台任务，运行期间其他“立即开始”按钮不可用。
5. 应用退出时停止后台播放器并上报当前视频的停止位置。

前台播放包括 Emby / Jellyfin、WebDAV、IPTV 和本地验证播放。前台播放开始时，后台任务立即停止当前媒体并保存已经实际播放的秒数；前台播放结束后重新随机选择媒体继续累计。

## Random Media And Duration

随机候选使用 Emby 官方 `GET /Users/{UserId}/Items` 接口：

- `Recursive=true`
- `Filters=IsNotFolder`
- `MediaTypes=Video`
- `IncludeItemTypes=Movie,Episode`
- `SortBy=Random`
- `Limit=12`

官方参考：

- `https://dev.emby.media/reference/RestAPI/ItemsService/getUsersByUseridItems.html`

调度器会避免立即重复最近播放的媒体。一个视频结束后，如果累计播放时间仍小于目标时长，会继续选择下一个随机视频。累计时间以 libmpv 实际播放位置为准，不以媒体元数据时长推算。

连续三次无法获取播放地址或播放失败时，任务进入 `error` 状态，避免无限快速重试。

调用异步播放地址请求时，候选 `MediaItem` 必须按值复制到回调。禁止在同一次函数调用的其他实参中移动该对象，否则 C++ 未指定的实参求值顺序可能先清空条目 ID，导致会话完整却无法创建播放 URL。

## Headless Playback

后台播放器使用独立的 `PlayerController` 实例，并通过 `initializeHeadless()` 初始化 libmpv：

- `force-window=no`
- `vo=null`
- `ao=null`
- `keep-open=no`
- 不设置 `wid`

因此不会创建视频窗口，也不会输出声音。任务页面只显示服务名、当前媒体、状态、累计时间和目标时间。

普通窗口播放器使用 `keep-open=yes` 保留最后一帧；后台播放器必须使用 `keep-open=no`，并在每次加载 URL 前清除暂停状态。否则前一个媒体结束时由 `keep-open` 产生的暂停状态可能被下一条媒体继承，界面会停在上一条媒体的累计秒数。

播放开始、进度和停止仍使用 Emby 会话接口上报，使服务器能看到真实播放活动：

- `POST /Sessions/Playing`
- `POST /Sessions/Playing/Progress`
- `POST /Sessions/Playing/Stopped`

任何日志都不得包含 Token、Cookie 或带 `api_key` 的完整播放 URL。

## Status Values

- `idle`: 当前无后台任务，可以手动开始。
- `waiting`: 等待前台播放结束。
- `starting`: 正在选择媒体或初始化播放。
- `playing`: 后台媒体正在按正常速度播放。
- `completed`: 已达到目标时长。
- `error`: 登录会话、网络、播放地址或 libmpv 出错。

## Maintenance Notes

- 增加 Jellyfin 支持前，必须先核对 Jellyfin 官方 OpenAPI 的随机排序和会话上报行为，不能直接假设与 Emby 完全一致。
- 调度器不直接调用 mpv，也不把复杂调度逻辑放入 QML。
- 修改前台播放入口时，应同步维护 `AppViewModel::setForegroundPlaybackActive()` 的开始和结束调用，确保后台任务始终让位。
