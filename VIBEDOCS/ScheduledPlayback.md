# Emby Keep-Alive Playback Strategies

## Purpose

保号任务模块为已保存登录会话的 Emby 播放源执行无窗口、无声音的后台播放。用户可以手动启动，也可以保存按本机时间重复运行的策略。

支持的策略：

- `manual`：仅手动运行。
- `daily`：每天在指定时间运行。
- `weekly`：每周指定星期和时间运行。
- `monthly`：每月指定日期和时间运行。
- `custom_monthly`：每月多个指定日期运行，例如每月 14、21、28 日 12:00 运行 2 小时。

每个策略独立保存播放源、运行时间、重复日期、播放时长和是否启用。即使自动策略被暂停，仍然可以通过“立即开始”手动运行。

## Architecture

调用链为：

`ScheduledTasksPage (QML)` -> `AppViewModel` -> `ScheduledPlaybackManager` -> `EmbyClient` / `SessionRepository` / `PlayerController`

重复时间计算由独立模块 `ScheduledPlaybackSchedule` 提供。它只负责：

- 规范化星期或每月日期列表。
- 判断策略是否为有效的自动策略。
- 按给定时区计算下一次运行时间。
- 跳过不存在的月日期，例如 4 月 31 日，并寻找下一个有效月份。
- 使用 `last_run_date` 防止同一策略在同一天重复自动触发。

`ScheduledPlaybackManager` 负责 30 秒检查定时器、到期任务排队、后台播放执行、随机选片、跨视频累计时长、失败处理和前台播放抢占。QML 不计算下一次运行时间，也不直接读写数据库。

## Scheduling Rules

1. 自动策略使用电脑当前本地时区，时间格式为 24 小时制 `HH:mm`。
2. 应用启动时会比较上次调度检查时间与当前时间，查找应用关闭期间已经到点但没有执行的策略。
3. 漏跑策略不会未经确认自动开始；应用右下角会显示持久通知，由用户选择“立即补跑”或“本次忽略”。
4. 如果系统睡眠跨过了运行时间，应用仍处于运行状态，因此已有的待触发时间会在恢复后直接进入自动队列，不显示启动漏跑通知。
5. 多条策略同时到期时进入 FIFO 队列，同一时刻仍只运行一个后台播放器。
6. 前台播放优先。自动任务到点时如果存在前台播放，会进入 `waiting` 状态并在前台播放结束后继续。
7. 自动任务入队时立即写入本地运行日期，避免定时器或页面刷新造成重复触发。
8. 月度策略选择 29、30 或 31 日时，不存在该日期的月份会被跳过。
9. 用户点击停止会终止当前后台播放并清空本轮等待队列；已经触发的策略不会在当天再次自动运行。
10. 修改系统时区后，调度器会重新计算所有可见策略的下一次运行时间。

普通模式只调度普通服务的策略；进入隐私模式后才加载并调度隐私服务策略。进入或退出隐私模式、修改服务隐私属性时会停止当前后台任务并刷新调度集合，防止隐私服务名称和状态跨模式残留。

## Startup Missed-Run Recovery

调度器通过 QSettings 保存两套 UTC 检查点：

- 普通检查点：只覆盖普通服务策略。
- 隐私检查点：只在进入隐私模式后读取和更新。

启动恢复流程：

1. 加载上次尚未回应的漏跑策略 ID。
2. 使用 `ScheduledPlaybackSchedule::firstOccurrenceBetween()` 检查上次检查点到当前时间之间是否存在应运行时间；没有历史检查点时从策略 `created_at` 开始检查。
3. 同一策略即使离线期间错过多次，也只合并为一次待补跑项，避免每日策略离线多天后一次排队运行多份完整时长。
4. 将待确认 ID 立即持久化，再更新当前检查点。
5. 右下角通知显示漏跑策略数和最多三个播放源名称；通知使用窗口 Overlay 的有效尺寸定位，高度由实际内容计算并受可用高度限制，正文最多展示四行，同时对横纵坐标应用安全边界，确保底部操作按钮不会越出窗口。
6. 用户选择补跑后，当前可见的漏跑策略各进入队列一次；选择忽略则只清除本轮待确认记录。

待确认 ID 独立于检查点保存，因此通知出现后即使应用再次异常退出，下次启动仍会继续询问。补跑不会修改 `last_run_date`，所以补跑昨天的任务不会阻止今天稍后的正常自动运行。

普通模式不会显示或清除隐私任务的待确认记录。进入隐私模式后才检查隐私策略的离线时间段并显示相关通知。

## Persistence

SQLite 表 `scheduled_playback_tasks` 保存：

- `id`
- `server_id`
- `schedule_type`
- `start_time`
- `schedule_days`
- `duration_minutes`
- `enabled`
- `last_run_date`
- `created_at`
- `updated_at`

`schedule_days` 使用排序且去重的逗号分隔整数：

- 每周策略保存 ISO 星期值 `1..7`，其中 1 为星期一。
- 每月策略保存日期 `1..31`。
- 每日和手动策略保持为空。

Repository 在写入 `schedule_days` 等允许为空的文本字段前，会将 Qt 的 null 字符串规范化为非 null 空字符串。手动和每日策略因此向 `schedule_days` 写入 `''`，不会触发数据库的 `NOT NULL` 约束。

旧数据库通过 `ALTER TABLE` 增加 `schedule_type` 和 `schedule_days`，默认值为 `manual` 和空字符串，因此旧的手动配置保持兼容。手动策略继续把 `start_time` 保存为 `manual`。

`last_run_date` 是本地 ISO 日期 `yyyy-MM-dd`。只有自动触发会更新它；用户点击“立即开始”不会占用当天的自动运行机会。

QSettings 额外保存：

- `scheduledPlayback/normalCheckpoint`
- `scheduledPlayback/privateCheckpoint`
- `scheduledPlayback/pendingMissedTaskIds`

`server_id` 通过外键关联服务卡片。删除服务及其本地数据时，对应策略会级联删除。

## Manual Start And Queue Rules

- 编辑器提供“保存策略”和“保存并立即开始”两个动作。
- 已保存卡片始终提供“立即开始”，包括暂停的自动策略。
- 用户手动启动时如果已有后台任务正在运行，会提示任务繁忙，不会插入自动队列。
- 自动到期任务可以在另一个后台任务运行时排队，并在前一个任务完成或失败后继续。
- 应用退出时停止后台播放器并上报当前视频的停止位置。

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

连续三次无法获取播放地址或播放失败时，当前任务进入 `error` 状态，自动队列随后继续处理下一项。

## Headless Playback And Reporting

后台播放器使用独立的 `PlayerController` 实例，并通过 `initializeHeadless()` 初始化 libmpv：

- `force-window=no`
- `vo=null`
- `ao=null`
- `keep-open=no`
- 不设置 `wid`

播放开始、进度和停止继续使用 Emby 会话接口上报：

- `POST /Sessions/Playing`
- `POST /Sessions/Playing/Progress`
- `POST /Sessions/Playing/Stopped`

后台播放器流量计入所选 Emby 服务的保号流量统计，但播放时长不计入用户正常观看时长。任何日志都不得包含 Token、Cookie 或带 `api_key` 的完整播放 URL。

## Status Values

- `idle`：当前无后台任务。
- `waiting`：等待前台播放结束。
- `starting`：正在选择媒体或初始化播放。
- `playing`：后台媒体正在播放。
- `completed`：当前策略达到目标时长。
- `error`：当前策略发生登录会话、网络、播放地址或 libmpv 错误。

## Tests And Maintenance

`ScheduledPlaybackScheduleTest` 覆盖：

- 日期列表排序、去重和范围过滤。
- 每日与每周下一次运行时间。
- 每月 31 日跨越无效月份。
- 每月 14、21、28 日等多日期策略。
- `last_run_date` 防重复。
- 应用关闭时间窗内的漏跑判断。
- 手动和暂停策略不参与自动计算。

增加 Jellyfin 支持前，必须核对 Jellyfin 官方 OpenAPI 的随机排序和会话上报行为，不能假设与 Emby 完全一致。除 `PlayerController` 外，任何调度模块仍不得直接调用 libmpv API。
