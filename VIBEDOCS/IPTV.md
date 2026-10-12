# IPTV 模块

## 模块目标

IPTV 模块为桌面播放器提供本地 M3U/M3U8 播放列表导入、频道浏览、分组过滤、搜索和播放能力。它作为独立服务类型接入服务页，不复用 Emby/Jellyfin 的登录或媒体服务器 API 流程。

## 主要文件

- `src/models/IptvPlaylist.h`：IPTV 播放列表元数据。
- `src/models/IptvChannel.h`：IPTV 频道模型。
- `src/services/iptv/IptvParser.h`
- `src/services/iptv/IptvParser.cpp`：M3U/M3U8 解析器。
- `src/services/iptv/IptvProbe.h`
- `src/services/iptv/IptvProbe.cpp`：异步线路探测与 HLS 主清单媒体信息解析。
- `src/services/iptv/IptvPlaylistStore.*`：应用私有目录中的播放列表受管副本。
- `src/viewmodels/IptvChannelListModel.h`
- `src/viewmodels/IptvChannelListModel.cpp`：暴露给 QML 的频道列表模型。
- `src/database/SessionRepository.*`：持久化 IPTV 播放列表与频道。
- `src/viewmodels/AppViewModel.*`：服务添加、导入、加载、过滤与播放入口。
- `qml/Main.qml`：IPTV 添加表单和频道页。
- `src/app/main.cpp`：记录 QML 启动 warning，便于定位启动期 UI 加载问题。

## 数据表

`iptv_playlists`

- `id`：播放列表 ID，当前由服务 ID 派生。
- `service_id`：对应 `servers.id`。
- `name`：播放列表名称。
- `source_type`：来源类型，当前为 `LocalFile`，预留 `RemoteUrl`。
- `source_path`：用户选择的原始文件路径。
- `imported_path`：复制到应用数据目录后的文件路径。
- `imported_at`：导入时间。

`iptv_channels`

- `id`：频道稳定 ID。
- `playlist_id`：对应 `iptv_playlists.id`。
- `name`：频道名称。
- `group_name`：频道分组。
- `logo_url`：台标 URL。
- `stream_url`：播放地址。
- `tvg_id`：频道的外部稳定标识；存在时优先用于合并同名线路。
- `stream_urls`：同一频道合并后的全部播放地址，以紧凑 JSON 数组保存。
- `sort_order`：播放列表中的顺序。
- `width`、`height`、`bitrate`、`codec`：最近一次探测或真实播放得到的媒体信息。
- `probe_state`：线路探测状态，例如 `available`、`unavailable`、`playback`。
- `stream_info`：每条线路的可用性、媒体参数、检查时间和 HLS 子清单信息，以 JSON 保存。

## 解析规则

- 标准频道列表通过 `#EXTINF` 读取频道元信息，下一条非注释行作为播放地址。
- 支持 `tvg-name`、`tvg-logo`、`group-title`。
- 没有分组时使用 `Default`。
- 如果文件是 HLS 清单，生成一个单频道，播放地址指向导入后的本地副本。
- 字符编码优先 UTF-8，失败后回退 GB18030、GBK 和系统编码。

## 本地文件生命周期

- 用户选择 `.m3u` 或 `.m3u8` 后，文件通过 `QSaveFile` 原子复制到
  `QStandardPaths::AppDataLocation/iptv`，服务卡和 HLS 单频道播放地址均使用该受管副本。
- 导入完成后不再依赖用户选择的外部目录；外部原文件可以移动或删除。
- 编辑既有 IPTV 服务时保持服务 ID，不会因路径切换到应用目录而创建重复服务。
- 重新选择播放列表会安全覆盖受管副本；直接编辑后保存同一个受管文件也不会先删除源文件。
- 旧版本创建的服务在加载时优先采用 `imported_path`，无需重新导入即可脱离外部路径。
- 用户选择删除本地数据时，数据库记录和对应受管副本一并删除；仅隐藏服务时保留副本。
- IPTV 服务卡不显示文件副标题；隐私服务列表仅显示服务类型，不展示“本地文件”或应用私有目录路径。

## 架构边界

- IPTV 不创建 `sessions` 记录。
- IPTV 不调用 `MediaServiceClient`。
- IPTV 播放不向 Emby/Jellyfin 上报播放进度。
- QML 只负责显示、文件路径输入和用户交互，导入解析、过滤和播放状态设置都在 C++ 层。

## 多线路合并与探测

- 导入只做静态解析和合并，不发起线路探测。优先按 `tvg-id` 合并；缺失时按
  去除首尾空白、折叠连续空白并忽略大小写后的名称和分组合并。不同 `tvg-id`、
  不同分组且没有标识的同名频道保留独立显示；不猜测 HD/4K 后缀是否表示同一频道。
- URL 去重并保留原始顺序；兼容加载旧数据库时保留第一条旧频道 ID，播放历史仍可定位。
  卡片和播放器侧栏显示线路数以及已知分辨率。
- 后台串行处理服务页首批 12 个频道、最多 36 条 HTTP(S) 线路。点击频道会取消旧探测，
  优先探测该频道最多 36 条线路。播放立即使用已有信息启动，不等待全部探测完成，
  后台结果不会切断当前播放；实际失败或 20 秒未开始时自动尝试下一候选。搜索和分组变化会再次调度当前列表首批频道。
- 每条线路最多读取 128 KiB，网络超时 6 秒；可选 FFprobe 只读取本地样本，超时 2 秒。
  HLS 主清单解析分辨率、码率、编码和子清单 URL；含独立音轨/字幕时保留主清单。
  HLS 媒体清单或样本不足的流暂不展示未知参数，等真实播放校正。
- 设置 → 常规 → IPTV 提供“高画质优先”和“省流优先”。高画质优先按已知分辨率、
  码率排序；省流优先在两条线路都有码率时先比较码率，再比较分辨率。
  可用线路优先于未知线路，失败线路最后保留。未知参数不视为最低码率或最低分辨率。
- 信息按 URL 保存：成功缓存有效期 6 小时，失败 1 分钟，过期后再次按需探测。
  播放器回报实际分辨率、编码和码率后校正当前线路与子清单；真实播放优先于晚到的
  探测结果。元数据合并为快照并在后台串行写库，避免在界面线程持续执行 SQL。
- 桌面和 CLI 共用线路选择器。CLI IPTV 播放使用保存的信息和偏好，不自行启动后台探测。

播放器工具栏增加“播放线路”菜单，允许手动选择当前频道的原始线路及已知 HLS
清晰度；显示当前项与已知媒体信息。手动选择开启新一轮回退，每个实际播放 URL
一轮只尝试一次，全部失败后停止并提示。正常 EOF、停止和暂停不触发自动回退。

模块接口、边界和测试见 [IPTV 线路选择与探测](IptvStreamSelection.md)。

## 导入与播放性能

- 桌面保存 IPTV 服务时，`AppViewModel` 用 `QtConcurrent` 调用共享的
  `ServiceOperations::savePlaylist`。解析、受管副本复制和批量 SQL 写入均在后台执行；
  `SessionRepository` 连接在同一后台线程创建和销毁，不跨线程使用界面连接。
- 导入期间 `loading` 保持为真，重复保存不会启动第二个导入。完成后异步刷新服务卡；
  如果用户已经离开或编辑了下一张卡，旧任务不会清空新表单或改变当前页面。
- 自定义数据库路径的调用者使用数据库父目录保存受管副本；普通桌面运行仍使用应用数据目录。
- `MpvVideoItem.liveStream` 绑定 `iptvPlaybackActive`，由 `PlayerController` 为 IPTV
  设置 `video-sync=audio`、`interpolation=no`，避免为直播额外生成插值帧。
  硬件解码仍使用 `hwdec=auto`；切换回普通视频会恢复现有平滑播放设置。
- 频道卡片直接作为 `GridView` 的 delegate，保留缓存与复用；由视图分帧创建缓存区
  delegate，不再为每个频道增加第二层异步 `Loader` 和淡入动画。台标继续异步加载。

参考：[mpv 视频同步与插帧](https://mpv.io/manual/stable/#video-synchronization)。

`AppViewModelStateTest` 使用 100 个临时频道验证后台导入、数据库忙时的界面计时器响应、
重复保存以及导入过程中开始编辑下一张服务卡的状态保护。

Windows 的 `native_iptv_import_playback` 测试装载完整 `Main.qml`，导入 100 个临时频道、
实际播放本地 720p/25fps 测试视频、返回服务页、打开添加 IPTV 表单和原生文件窗口，
检查弹窗背景捕获对象的窗口归属，并正常销毁整个界面。使用 D3D11 WARP 渲染界面以避开
测试机器的旧 Intel 驱动启动异常；视频仍由 libmpv 单独渲染。缺少 FFmpeg 时跳过该测试。
该用例覆盖本地 IPTV 流程，不能替代用户实际直播源和显卡负载的验收。

## 后续扩展

- 支持远程 M3U URL 时，可以复用 `sourceType` 字段并新增下载/刷新逻辑。
- 收藏频道可新增独立表保存频道 ID。
- EPG 可新增节目单模型与频道 ID 映射。
