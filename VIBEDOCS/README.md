# VIBEDOCS

- `EncryptedHlsTarContainer.md` documents the indexed `.m3u8sp` TAR container and its local/WebDAV playback rules.

- `ScheduledPlayback.md`: Emby 手动/重复保号策略、日期计算、自动队列、随机选片和前台播放抢占规则。

本目录记录项目中可复用模块的设计、边界和实现依据。

当前文档：

- [ServiceCardLoader.md](ServiceCardLoader.md)：服务卡片后台只读快照、请求合并、隐私过滤与最近使用时间。
- [SelfSignedTlsPolicy.md](SelfSignedTlsPolicy.md)：自签名错误边界、桌面/CLI 验证播放代理、真实证书测试与系统 CA 临时文件。
- [QmlUtilities.md](QmlUtilities.md)：辅助组件的闭包、请求代次、图片生命周期及运行测试。

- [BoundedReplyWriter.md](BoundedReplyWriter.md)：普通 WebDAV 播放的有界转发与背压。

- [AuditFixes-2026-10-07.md](AuditFixes-2026-10-07.md)：缺陷修复、回归证据与尚未验证的边界。
- [SqlTransaction.md](SqlTransaction.md)：SQLite 提交失败后的统一事务清理。
- [SessionTokenCipher.md](SessionTokenCipher.md)：DPAPI/系统凭据引用、旧会话迁移、事务清理和平台边界。
- [NativeCredentialStore.md](NativeCredentialStore.md)：系统凭据原生后端、依赖与 CI 测试环境。
- [FfmpegProgress.md](FfmpegProgress.md)：打包和还原共用的安全进度行解析。

- [CLI-Tutorial.md](CLI-Tutorial.md)：完整用户教程，覆盖全部 CLI 命令、参数、配置键、视频加密/还原、后台任务，以及 PowerShell/CMD/GitHub Actions 自动化示例。
- `CLI.md`：控制台入口、全部命令、Shell、隐私/凭据、JSON/退出码和自动化示例。
- `CliMedia.md`、`CliBackgroundTasks.md`、`CliShell.md`：媒体宿主、持久任务续传、交互表达式。
- `SmbClient.md`、`S3CredentialProvider.md`：Windows SMB、S3 临时凭据与 STS 角色。
- `CliOutput.md`：终端表格、完整 ID/路径、中文宽度、批量明细、JSON 兼容与重定向。
- `CliVerification.md`：Windows 本地构建、25 项测试、真实终端、安装包与验收范围。
- `SharedWorkflows.md`：服务管理、配置快照、文件夹上传规划、TSSL 导入的共享服务层。
- `ServiceOperations.md`、`WorkflowSettings.md`、`WebDavUploadPlanner.md`、`TsslImportWorkflow.md`：各共享模块的接口与边界。
- [TsslBackup.md](TsslBackup.md)：本地、WebDAV、S3 的备份传输、取消和新旧格式兼容。
- [TsslBackupArchive.md](TsslBackupArchive.md)：TSSL ZIP 格式、清单校验、资源限制和独立压缩/解压模块。

- `FileSelection.md`：统一的异步文件/文件夹选择控制器、原生优先策略、批量目录兼容实现与生命周期测试。
- `EncryptedHlsRestore.md`：视频还原服务卡片、M3U8S/M3U8SP 批量解密重封装、原文件名恢复、完整性校验与取消清理。
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
- `NetworkTrafficCoalescer.md`：逐块网络字节计数合并为周期样本的公共组件，供各 WebDAV 宿主转发 `networkTrafficSample`。
- `FfmpegCapability.md`：启动时对未捆绑的外部 FFmpeg 做一次性能力探测（只卡版本下限，外加横幅身份校验），驱动 M3U8S 页状态徽标、路径选择行与启动警告弹窗。
- `GitHubActionsRelease.md`：跨平台构建、原生安装包、Flatpak 与 GitHub Release 发布流程。
- `DialogBackdropBlur.md`：模态对话框共享的背景虚化遮罩（双源 1:1 抓取）与弹出动效约定。
- `OptionSegmentedControl.md`：分段按钮组选择控件，选中块横向滑移、等宽分段与悬停淡入规则，用于替代短枚举下拉框。
- `ThemeTransition.md`：明暗主题切换的快照溶解动效（themePalette 单一换色入口 + ThemeSnapshotController 场景图抓屏），按钮动画先行、DWM 窗口件同帧翻转，含启动期瞬时应用与十个已踩坑记录。

调试提示（QML 布局自检）：

- 从 git-bash 以 `./vibePlayerQT.exe > log 2>&1 &` 启动时，QML 的 `console.log/warn` 与 Qt 警告都不会
  出现在该文件里（长期 0 行），所以“日志无输出”不能作为“无错误”的证据。
- 需要确认运行时几何时，把结果字符串赋给 `root.title`，再用
  `powershell.exe -NoProfile -Command "(Get-Process vibePlayerQT | Select-Object -First 1).MainWindowTitle"`
  读回，即可拿到 x/y/width/height、`visible`，以及 `anchors` 是否真的生效（例如
  `anchors.centerIn: <Control>` 会被静默拒绝并把 item 落在 (0,0)）。验证完删除探针。
