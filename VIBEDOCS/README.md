# VIBEDOCS

- `EncryptedHlsTarContainer.md` documents the indexed `.m3u8sp` TAR container and its local/WebDAV playback rules.

- `ScheduledPlayback.md`: Emby 手动/重复保号策略、日期计算、自动队列、随机选片和前台播放抢占规则。

本目录记录项目中可复用模块的设计、边界和实现依据。

当前文档：

- `EmbyJellyfinApi.md`：Emby / Jellyfin 第一阶段登录、媒体库与库内列表 API 调研。
- `MediaServices.md`：媒体服务层、网络层、ViewModel 和 QML 的分层边界。
- `MediaHomeUi.md`: Emby / Jellyfin shared cinematic home, media rails, and
  interaction boundaries.
- `DesktopLifecycle.md`：桌面端窗口生命周期、系统托盘和最小化到托盘行为。
- `SettingsAppearance.md`：设置页、i18n、明暗主题和服务卡片拖拽排序设计。
- `PlayerRuntime.md`：libmpv runtime、Window Embedding、QML 播放页和播放 URL 流程。
- `LocalPlayback.md`：轻量本地目录浏览、后台枚举、路径边界与播放来源隔离。
- `LinkPlayback.md`：HTTP/HTTPS 直接媒体与 HLS 链接校验、按日期保存的播放历史、单条删除、流量统计及安全边界。
- `GlobalPlaybackHistory.md`：六类播放来源的统一历史、SQLite 数据模型、进度更新、隐私隔离与重播路由。
- `WebDAV.md`：WebDAV 协议边界、下载规划、总任务/文件明细模型与传输统计口径。
- `GitHubActionsRelease.md`：跨平台构建、原生安装包、Flatpak 与 GitHub Release 发布流程。
- `DialogBackdropBlur.md`：模态对话框共享的背景虚化遮罩（双源 1:1 抓取）与弹出动效约定。
- `OptionSegmentedControl.md`：分段按钮组选择控件，选中块横向滑移、等宽分段与悬停淡入规则，用于替代短枚举下拉框。

调试提示（QML 布局自检）：

- 从 git-bash 以 `./vibePlayerQT.exe > log 2>&1 &` 启动时，QML 的 `console.log/warn` 与 Qt 警告都不会
  出现在该文件里（长期 0 行），所以“日志无输出”不能作为“无错误”的证据。
- 需要确认运行时几何时，把结果字符串赋给 `root.title`，再用
  `powershell.exe -NoProfile -Command "(Get-Process vibePlayerQT | Select-Object -First 1).MainWindowTitle"`
  读回，即可拿到 x/y/width/height、`visible`，以及 `anchors` 是否真的生效（例如
  `anchors.centerIn: <Control>` 会被静默拒绝并把 item 落在 (0,0)）。验证完删除探针。
