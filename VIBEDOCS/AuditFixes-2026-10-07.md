# 2026-10-07 缺陷修复记录

本记录对应根目录 [复核报告](../BUG-ANALYSIS-2026-10-07.md) 的原编号。
原报告针对 `fb438ef` 的验证结论保留；当前状态以报告新增的修复表为准。

## 数据与隐私

- 历史列表、日期、按日查询和删除统一使用“记录私有或服务私有”的规则。
  普通服务的全局隐私播放记录不会被普通模式显示或按日删除；稳定媒体身份
  仍只保留最近一次历史。详见 [GlobalPlaybackHistory](GlobalPlaybackHistory.md)。
- 八处既有事务改用 [SqlTransaction](SqlTransaction.md)，服务的多步数据库
  删除也使用它。提交失败回滚，回滚失败关闭连接，原错误保留。
- 普通卡片重排在完整的启用卡片列表中替换普通位置，私有卡片保留位置；
  列表发生并发增删时返回重试错误，避免越界或更新错误的卡片。
- IPTV 重导入保留既有父列表 ID，更新子频道；引号属性中的逗号不会被当作
  `EXTINF` 标题的分隔符。
- 旧会话按时间、ID 去重后建唯一索引。Windows Token 使用
  [SessionTokenCipher](SessionTokenCipher.md)，包含旧明文迁移和只读兼容。
- 删除定时任务立即移除其持久 missed ID；调度器重载时使用完整的数据库 ID
  集合清理已删除任务，普通模式不会清掉仍存在的私有任务 ID。

## 传输、网络与备份

- `retainPartial=true` 的暂停恢复及自动重试保留残片。只有强 ETag 和有效
  残片才发 Range/If-Range；有强 ETag 时每次请求另加 If-Match，固定检查点
  对应的文件版本。响应 200 时从头写入，异常 Content-Range 或不同 ETag 的
  206 被拒绝；版本变化不会追加到旧残片。默认桌面下载仍从头重试。
- WebDAV DELETE/MOVE/COPY 补齐认证挑战凭据，并保留既有 Basic 头。取消与
  超时分别返回 `Canceled`、`Timeout`，CLI 主动取消返回 130。
- 媒体列表的 JSON 解析错误、缺失/错误类型的数组及非对象元素明确失败。
  合法空列表仍成功。解析 watcher 随客户端存活，网络回调检查客户端存活期。
- 登录及播放报告 POST 不自动跟随重定向；需配置最终 API 地址，避免变成
  GET 或转发凭据。该兼容性变化已在 API 文档中说明。
- TSSL 网络备份采用空闲超时、总时限和最多两次瞬时故障重试。S3 XML 在
  worker 解析，损坏 XML、缺失或重复分页令牌明确失败；页数、时间和响应体
  有界。详见 [TsslBackup](TsslBackup.md)。
- 普通 WebDAV 播放代理限制请求头为 64 KiB、完整头等待为 15 秒，并使用
  `QPointer` 防止断连后的 socket 访问。加密代理逐个检查 Content-Range
  数值转换是否成功。

## 生命周期、进度与日志

- 首页缓存 timer 单次接线，推荐刷新或首页请求未完成时延后后台刷新，
  避免旧推荐回调因代次变化而遗留刷新状态。
- 播放器先关闭 libmpv 再拆宿主窗口，阻止同步重入创建窗口；事件头和
  end-file 数据先复制，句柄代次变化后停止排空旧事件。
  详见 [PlayerRuntime](PlayerRuntime.md)。
- CLI 等待结果使用共享状态和受保护的事件循环指针，提前退出返回取消；
  晚完成回调不访问已经释放的栈变量。等待 future 时先等后台工作结束，
  再释放命令输入，避免退出时后台工作仍引用这些数据。
- 打包时限制实际读入长度，并检查完整读取、文件大小和 QFile 错误；异常
  数据在返回错误前清零。打包/还原共用 [FfmpegProgress](FfmpegProgress.md)。
- 仅在配置 `VIBEPLAYER_LOG_FILE` 时写文件。文件写入及轮转串行执行，当前
  日志约 10 MiB 轮转，最多保留 `.1`、`.2`、`.3` 三个历史文件；单条记录
  限制 64 KiB。日志写入失败不会影响任务执行，已有 Qt 日志通道继续可用。

## 本地验证

Windows 11 x64、Qt 6.7.3、clang-cl/Ninja 的桌面、CLI、播放宿主及全部测试
目标编译成功。原有 25 个目标和新增 `audit_regression` 均通过：
**26/26，48.32 秒**。

新增目标的 QtTest 日志为 **34 passed、0 failed、0 skipped**（含初始化和
清理）。加密代理测试另外增加三个 Content-Range 溢出案例，以及真实 libmpv
从状态、结束、错误回调同步关闭后重新初始化的三个案例。

构建/测试记录在被忽略的构建目录：

- `build-clang/audit-fix-build.txt`
- `build-clang/audit-fix-tests.txt`
- `build-clang/audit-regression-results.txt`

测试使用独立临时数据库、INI、文件、假凭据和本地 HTTP 服务。现有
加密代理测试还覆盖本地/HTTP/WebDAV 播放、音轨切换、字幕加载和 seek。
未对真实 SMB 服务、GUI 原生窗口反复销毁、真实 Emby/Jellyfin 登录、真实 S3、
macOS/Wayland 或 ASan 进行验收，不能据此宣称所有平台上的崩溃风险已消除。
H-8 的真实首页竞态、M-8 大库性能、L-05 日志队列压力、L-10 平台窗口、
L-14 CBOR 收敛仍需独立验证。C3 保持既有隐藏停止及显式播放重建规则。

修改没有安装新运行依赖，也没有改动真实用户数据；没有创建 Git 提交。
