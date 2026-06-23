# IPTV 模块

## 模块目标

IPTV 模块为桌面播放器提供本地 M3U/M3U8 播放列表导入、频道浏览、分组过滤、搜索和播放能力。它作为独立服务类型接入服务页，不复用 Emby/Jellyfin 的登录或媒体服务器 API 流程。

## 主要文件

- `src/models/IptvPlaylist.h`：IPTV 播放列表元数据。
- `src/models/IptvChannel.h`：IPTV 频道模型。
- `src/services/iptv/IptvParser.h`
- `src/services/iptv/IptvParser.cpp`：M3U/M3U8 解析器。
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
- `sort_order`：播放列表中的顺序。

## 解析规则

- 标准频道列表通过 `#EXTINF` 读取频道元信息，下一条非注释行作为播放地址。
- 支持 `tvg-name`、`tvg-logo`、`group-title`。
- 没有分组时使用 `Default`。
- 如果文件是 HLS 清单，生成一个单频道，播放地址指向导入后的本地副本。
- 字符编码优先 UTF-8，失败后回退 GB18030、GBK 和系统编码。

## 架构边界

- IPTV 不创建 `sessions` 记录。
- IPTV 不调用 `MediaServiceClient`。
- IPTV 播放不向 Emby/Jellyfin 上报播放进度。
- QML 只负责显示、文件路径输入和用户交互，导入解析、过滤和播放状态设置都在 C++ 层。

## 后续扩展

- 支持远程 M3U URL 时，可以复用 `sourceType` 字段并新增下载/刷新逻辑。
- 收藏频道可新增独立表保存频道 ID。
- EPG 可新增节目单模型与频道 ID 映射。
