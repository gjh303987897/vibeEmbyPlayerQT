# 代码缺陷分析报告（2026-10-07 复核与修复版）

- **项目**：vibeEmbyPlayerQT。
- **源码基线**：`fb438ef`，`feat(cli): add automation commands and usage documentation`。
- **复核环境**：Windows 11 x64，Qt 6.7.3，clang-cl，SQLite（Qt QSQLITE 驱动）。
- **范围**：原报告全部 **54 项**：C1–C3、H-1–H-11、M-1–M-12，以及按原顺序补编号的 L-01–L-28；另校正历史审计对照表。
- **方法**：逐项阅读相关源码、调用方和 VIBEDOCS；核对官方文档；编译运行隔离复核程序，并运行相关现有测试。
- **复核阶段**：修正文档并验证基线缺陷，没有修改应用源码。
- **修复阶段**：已修改应用源码、补充回归测试及模块文档；桌面与 CLI 构建成功，26/26 测试目标通过。修改未提交 Git，没有修改真实用户服务、凭据或数据库。

本文不是对全部源文件安全性的证明。原文的“6 路审查”“全部模块无缺陷”“逐行核对即确认崩溃”等表述不能作为本次验证证据，已撤下。行号针对上述源码基线；后续改动后应重新定位。

## 结论与证据标准

以下数量记录 `fb438ef` 基线的复核结论，**不是修复后仍存在的缺陷数量**。当前状态见下一节；原编号、原始证据及未确认风险仍保留，避免把防护调整等同于复现了崩溃。

| 结论 | 数量 | 含义 |
| --- | ---: | --- |
| 成立 | 15 | 源码能确认缺陷或安全缺口；注明哪些经过运行验证。成立不等于已复现全部用户影响。 |
| 部分成立 | 13 | 原文机制或部分条件真实，但范围、因果关系或严重度需修正。 |
| 待验证 | 6 | 有线索，尚无完整触发链或运行证据，不能按确认缺陷排期。 |
| 不成立 | 20 | 被代码、现有设计、官方契约或测试反驳；包含设计边界和维护建议。 |
| **合计** | **54** | 保留原编号，历史重复项不再计数。 |

原文“3 个 Critical、11 个 High”的统计不再成立。本次没有确认可直接定为 Critical 的崩溃或远程漏洞。优先处理 **C2 隐私历史分类/删除** 和 **H-1 提交失败后的事务状态**。未复现的窗口生命周期、socket 存活期问题保留为风险，不能写成必然崩溃。

## 当前修复状态（2026-10-07）

**15 项“成立”问题均已修改实现**，H-9 的加密交付限定 Windows。其余部分成立项按有效触发条件处理；设计边界及没有证据的推断没有作为 bug 重写。

| 编号 | 当前状态 | 修复与验证 |
| --- | --- | --- |
| C2 | 已修复 | 记录标记与当前服务标记取 OR；四种服务来源、日期列表、按日查询/删除和移动私有卡片的回归通过。 |
| H-1 | 已修复 | 八处事务统一 RAII；提交失败回滚，回滚失败关闭连接。锁竞争测试验证下一次事务可用。 |
| H-5 | 已修复 | 暂停/自动重试保留 `retainPartial`；使用强 ETag 的 Range/If-Range，并以 If-Match 固定检查点文件版本。200 从头写、异常 206 拒绝。两种恢复测试最终字节一致、变化 ETag 不追加到旧残片，既有进程重启测试通过。 |
| H-6 | 已修复 | timer 单次连接到后台刷新；首页请求或推荐刷新仍在进行时延后触发。桌面编译通过，真实首页交互验收待补。 |
| H-9 | Windows 已修复 | 用户范围 DPAPI 加密，旧明文事务迁移，安全删除旧单元内容，错误不降级明文；旧库、只读解密、清除测试通过。macOS/Linux 仍为既有存储，见 [边界说明](VIBEDOCS/SessionTokenCipher.md)。 |
| H-11 | 已修复 | expected 错误传播；损坏 JSON、HTML、缺失/错误数组及无效元素失败，合法空列表成功。解析及真实客户端调用回归通过。 |
| M-4 | 已修复 | 重排完整启用卡片列表中的普通位置，保留私有位置；并发增删返回重试错误。C/B/A 和反向移动、唯一序号测试通过。 |
| M-9 | 已修复 | 仅使用引号外逗号分割 EXTINF；名称/分组回归通过。 |
| M-11 | 已修复 | 主动 abort 返回 Canceled，超时计时器单独标记；网络与 WebDAV 取消测试通过，CLI 保持退出 130。 |
| L-02 | 已修复 | 有 id 的旧表先保留最新身份再建唯一索引；无 id 的旧表按时间与 rowid 确定替换顺序。迁移/重复身份测试通过。 |
| L-04 | 已修复 | 64 KiB 请求头上限、15 秒完整头等待期限、单连接单响应；超长请求收到 431 并关闭连接的测试通过。 |
| L-11 | 已修复 | start/end/total 各自检查数值转换成功；三种溢出响应均在容器解析前被拒绝。 |
| L-15 | 已修复 | 打包/还原共用安全进度行解析，缺分隔符或空键忽略，数值校验；异常行及既有打包/还原测试通过。 |
| L-16、L-17 | 已修复 | S3 worker 解析、根节点检查，缺失/重复 token 明确错误，页数和整体列举时间有界；损坏 XML/重复 token 端到端测试通过。 |
| H-3 | 已修复剩余清理问题 | 删除任务移除持久 missed ID；调度器用完整数据库 ID 集合清理已删除项，保留存在的私有项。仓库删除清理回归通过。 |
| H-4 | 已调整并验证 | 60 秒无进度时限 + 15 分钟总时限；最多两次有界瞬时故障重试。缩短时限的慢速进度/总时限及 503 重试回归通过，未作真实 60 秒慢链路验收。 |
| H-10 | 已修复挑战缺口 | DELETE/MOVE/COPY 设置认证属性；三种操作的本地 Digest 挑战回归通过，既有 Basic 路径保留。 |
| M-3 | 数据库部分已修复 | 多步删除与级联在事务内，提交后才清理托管文件；故障触发器验证失败保留会话/卡片。不声称文件系统与 SQLite 跨资源原子。 |
| M-5 | 已加完整读取防护 | 一次捕获有界长度，实际读取有界，检查大小变化及 QFile 错误，失败清零；正常打包回归通过，未注入磁盘短读。 |
| M-7 | 已改善并验证 | 文件写入/轮转串行、约 10 MiB、最多 3 个归档、单条 64 KiB；并发 800 条完整/唯一与轮转测试通过。 |
| L-01 | 已修复低层接口边界 | IPTV 重导入保留已有父 ID，并替换频道；不同输入 ID 的重导入回归通过。 |
| L-18 | 已修复提前退出边界 | 共享结果状态与 QPointer 保护循环，未完成返回取消；future 提前退出先等待后台工作结束，再释放命令输入。提前退出、晚完成及后台清理测试通过，watcher 随应用清理。 |
| L-23 | 已增加生命周期防护 | 网络回调检查 QPointer，解析 watcher 随客户端；完成调用前转交 watcher 以允许回调清理客户端。正常 API 回归通过，未做 ASan 销毁压力验收。 |
| L-24 | 已明确兼容策略 | POST 不自动跟随重定向，提示配置最终地址；302 测试确认只发一次，不改 GET/转发凭据。 |
| C1、L-06 | 生命周期调整已实施 | libmpv 在宿主销毁前停止，阻止 teardown 重入；复制事件/结束数据并检查句柄代次。真实 headless mpv 的状态/结束/错误回调关闭和重新初始化通过；GUI 宿主压力及各平台窗口验收仍待补。没有声称已复现原崩溃。 |
| H-7 | 预防性防护已实施 | socket 使用 QPointer，响应状态共享管理；十次延迟上游断连测试通过。未用 ASan 证明或复现原 UAF，保留原风险结论。 |
| H-8 | 已避免接通 timer 后的已知代次竞争 | 推荐未完成时不启动后台代次；未完成真实首页并发场景验收，继续保留待验证状态。 |
| C3 | 保留现有行为 | 隐藏视频停止，显式 play 重建；没有加入自动续播，原“永久失效”推断不成立。 |
| M-8、L-05、L-10、L-14 | 仍待独立验证 | 大库 GUI 延迟、mpv 日志队列压力、Wayland/平台窗口及 CBOR 收敛没有新增运行证据，不按已确认缺陷改写。 |
| 其余 20 项不成立条目 | 无修复任务 | 保留基线结论及已有设计，不为误判修改实现。 |

当前构建与测试记录：`build-clang/audit-fix-build.txt`、`build-clang/audit-fix-tests.txt`、`build-clang/audit-regression-results.txt`。**26/26 目标通过，48.32 秒**；新增目标 QtTest 为 **34 passed、0 failed、0 skipped**（含初始化/清理）。详见 [修复记录及验收边界](VIBEDOCS/AuditFixes-2026-10-07.md)。以下章节与历史对照表是修复前基线的证据记录。

## C 类原始条目复核

### C1. 宿主窗口先于 mpv 句柄销毁

**结论：部分成立。销毁顺序属实，崩溃及平台细节未验证；暂按生命周期风险处理。**

证据：`src/player/MpvVideoItem.cpp:648–671`，Windows 在 653 行销毁 HWND，非 Windows 在 660 行删除 QWindow，670 行才调用 `m_controller.shutdown()`；`src/player/PlayerController.cpp:641–650` 随后终止并销毁 mpv。

官方说明确认 Win32 的 `wid` 表示父 HWND，mpv 创建自己的子窗口。这说明先拆宿主值得检查，但不足以证明每次停止都会崩溃。原文把 QWindow 删除直接等同于释放 macOS NSView，以及声称所有平台都有相同线程行为，证据不足。[mpv 窗口嵌入说明](https://mpv.io/manual/stable/#options-wid)

需补充本地/HTTP/WebDAV 播放中反复停止、隐藏、销毁组件的压力测试，记录实际 DLL 版本、VO、崩溃栈。调整顺序还要验证关闭耗时及 shutdown 信号重入，不能仅凭“交换两行”宣称修复完成。

### C2. 普通服务的隐私播放历史被当作普通记录

**结论：成立，已运行复现；建议 High，优先处理。**

证据：`src/database/SessionRepository.cpp:802、827、830、888、924、928、1442`；`src/viewmodels/AppViewModel.cpp:9623、9641、9651`。

写入使用 `m_privacyMode || server.privateMode`，读取和按日删除却使用：

```sql
COALESCE(servers.private_mode, history.privacy_mode)
```

匹配到普通服务时，服务器标记 `0` 优先于记录标记 `1`。隔离数据库中分别写入 Emby、Jellyfin、IPTV、WebDAV 的私有历史后，普通查询均返回一条，返回的 `privacyMode` 均变为 `false`；日期列表和按日查询也可见；普通模式按日删除把四条私有记录全部删除。Local/Link 未匹配服务器时使用记录标记，不属于该触发条件。

**设计冲突也必须说明**：[GlobalPlaybackHistory.md](VIBEDOCS/GlobalPlaybackHistory.md) 同时写了“普通模式排除私有历史”和“当前卡片隐私属性优先”。后者描述当前实现，但会吞掉全局隐私播放时写入的标记。应明确全局隐私记录的隔离规则，再统一列表、日期和删除谓词；不能只修改显示列或简单删掉服务器隐私判断，否则可能暴露已移入隐私模式的卡片历史。

### C3. 视频项隐藏后“永久失效”

**结论：部分成立。不会自动续播属实，“永久失效、必须重新设置 source”不成立。**

证据：`src/player/MpvVideoItem.cpp:427–452、483–488、533–557`。

非音频视频项变为不可见时调用 `stop()`，清除 `m_pendingPlay` 并销毁播放器。仅把同一个组件重新显示，且没有新播放请求，不会自动开始播放。但显式 `play()` 会调用 `ensureNativeWindow()`，无需更换 source；场景切换也可能重建窗口。因此不能直接判为 Critical。

是否恢复隐藏前的播放及位置取决于页面生命周期约定。若要求隐藏后续播，需保留播放意图和位置，并验证主动停止不会被错误恢复。本次没有验证桌面导航中存在无法通过播放操作恢复的场景。

## H 类原始条目复核

### H-1. COMMIT 失败未收尾事务

**结论：成立，SQLite 机制已运行复现；建议 High。**

证据：`src/database/SessionRepository.cpp:535、684、1022、1464、1591、1642、2318、2396`，八处最终提交失败直接返回，未回滚。

隔离测试让另一连接持有读事务：写事务提交返回 `database is locked`，随后同一连接执行 `BEGIN IMMEDIATE` 返回 `cannot start a transaction within a transaction`；显式回滚后恢复。机制与官方说明一致。[SQLite 事务错误与提交语义](https://www.sqlite.org/lang_transaction.html)

需修正原文：`SQLITE_FULL`、`SQLITE_IOERR` 等错误**可能自动回滚**，不能一概声称事务必然悬挂；影响主要是遗留事务的连接，并非数据库文件永久损坏。关闭连接或正确收尾可以恢复。建议统一事务清理，保留原始提交错误，同时处理回滚结果。测试复现的是 SQLite 提交条件，不是对每个仓库方法都注入了提交失败。

### H-2. 定时播放在成功前写 lastRunDate

**结论：不成立为现有设计缺陷；属于明确记录的自动触发策略。**

证据：`src/services/scheduler/ScheduledPlaybackManager.cpp:290–301`；[ScheduledPlayback.md](VIBEDOCS/ScheduledPlayback.md) 的执行规则第 7 条及 `last_run_date` 定义。

字段记录当天自动触发日期，防止同一天重复入队，不代表播放成功或任务完成。失败后不再次自动触发属实，用户仍可手动运行；原文将它解释成“已完成标志”错误。需要成功后记账或自动重试时应作为调度策略变更，不能直接回滚日期导致定时器反复入队。

### H-3. 跨隐私模式检查点和 missed ID

**结论：部分成立；两项主要推断不成立，仅剩已删除任务 ID 清理不及时的 Low 风险。**

证据：`src/database/SessionRepository.cpp:1188–1225`；`src/services/scheduler/ScheduledPlaybackManager.cpp:63–91、171–183、186–254、390–403`。

- `loadScheduledPlaybackTasks(true)` 加载**普通和私有**任务，不是只加载私有任务。实测普通模式返回一条，隐私模式返回两条；`rebuildNextRuns()` 遍历整个集合。推进普通检查点不能据此认定漏评普通任务。
- 隐私模式的 `existingTaskIds` 同样包含普通任务，因此交集不会仅因切入隐私模式抹掉普通 missed ID。启动检查每种模式只执行一次，不是每次切换都交集。
- 普通模式下删除任务后，原 ID 可能继续留在持久化 missed 集合；后续隐私模式首次完整检查可能清理。`missedTaskCount()` 只统计当前任务集合，不能推导为幽灵通知必然出现。

可改善删除 ID 的清理，但应在掌握完整任务集合时进行，避免普通模式清掉暂不可见的私有任务。

### H-4. TSSL 备份每请求 60 秒总时限及失败即停止

**结论：部分成立；慢速大包的时限风险成立，原吞吐量和包大小推断错误。**

证据：`src/services/backup/TsslBackupService.cpp:27–28、756–805`。

每个网络请求使用一次性 60 秒计时器，即使持续有进度也会到时终止；S3 PUT 上传整个 payload。任一文件失败结束当前流程，没有自动重试。恢复失败清理本次临时下载文件，不等于删除原有 TSSL 或已上传的远程对象。

`maxPackageBytes = 256 MiB` 主要用于恢复读取防护，不是正常 TSSL 的大小承诺；`src/services/webdav/TsslStore.cpp:21、224` 将正常包限制为 64 MiB。纯传输计算下，256 MiB / 60 秒约 **4.27 MiB/s**，64 MiB / 60 秒约 **1.07 MiB/s**，未计连接和服务端耗时。不能声称“任何低于 3.5 MB/s 的链路必然失败”，小包可完成。

Qt 传输超时针对无数据交换的间隔，与请求总时限不同。可设计可配置的总时限、空闲超时和有界重试；不能简单取消全部总时限，让异常请求无限等待。[Qt 传输超时](https://doc.qt.io/qt-6/qnetworkrequest.html#setTransferTimeout)

### H-5. retainPartial 在恢复及自动重试时失效

**结论：成立，已运行复现；建议 Medium，限定于同一 TransferManager 的对应路径。**

证据：`src/services/webdav/TransferManager.cpp:868–877、1717–1718、1800–1840`，尤其 1826 行无条件移除下载文件；暂停完成分支在 1645 行保留 `retainPartial` 分片。

本地慢速 HTTP 测试中，`retainPartial=true` 暂停留下 **20480 字节**；`resumeTask()` 后变为零字节，第二次 GET 无 Range。自动重试也有无条件删除。默认 `retainPartial=false` 的桌面行为本来就从头下载，不属于此回归。

**原文“续传恒定失效”错误**：CLI 进程重启后重建任务的路径不经过同一个 `requeueTask()`；现有两组重启续传测试通过，包括合法 Range 和非法 Range 后重试。修复需检查主动恢复、自动重试、取消、ETag 变化，不能无条件保留所有残片。

### H-6. 首页缓存延迟刷新定时器未接线

**结论：成立，源码确认；建议 Medium。**

证据：`src/viewmodels/AppViewModel.h:1081`；`src/viewmodels/AppViewModel.cpp:123、9330、11254–11283、11311`。全源码检索未发现该 timer 的 timeout 连接；`backgroundRefreshHome()` 除定义外无调用。

缓存新鲜期是 120 秒，**延迟刷新是 650 ms**。命中缓存后的后台校验不执行；显式刷新、缓存失效后的正常加载仍可更新，不能写成全部首页刷新永久失效。守卫仍有用途，也不能仅凭刷新标志常为 false 推导重复请求。

补接线时需明确单次触发，并验证请求代次及推荐刷新状态；“一行 connect 即完成全部验证”的建议不充分。

### H-7. WebDAV 播放代理 socket UAF

**结论：待验证；裸指针风险存在，尚未证明 seek/stop 崩溃。**

证据：`src/services/webdav/WebDavPlaybackProxy.cpp:174–223`。

reply 回调捕获裸 socket，socket 断开后安排 `deleteLater()`，缺少显式 QPointer 防护。但同一断开信号会终止正在运行的 reply；常规路径可能在 socket 真正删除前完成回调。还要分析信号顺序、重入和 proxy 析构顺序，不能从“没有 QPointer”直接确认 UAF。

建议用延迟响应、连续断连及生命周期压力测试配合内存诊断取得栈和时序。历史“已确认”标签不能替代证据。

### H-8. Emby 推荐刷新标志卡死

**结论：待验证，原文未给出可到达的完整触发链。**

证据：`src/viewmodels/AppViewModel.cpp:8742–8812、8961–8970、11254–11265、11304–11328`。

过期结果确实提前返回，但 `invalidateHomeLoading()` 同时增加代次并清刷新标志；推荐状态重置也清标志。另一处增加代次的 `backgroundRefreshHome()` 当前无入口（H-6）。仅看 return 不能证明标志永久为 true。

需验证同一服务内并发刷新或接通 H-6 后的代次竞争。旧回调不能无条件清标志，否则可能清掉新请求的状态。

### H-9. access_token 明文 SQLite 存储

**结论：成立为安全缺口，源码确认；建议 Medium，是否升 High 取决于本地威胁模型。**

证据：`src/database/SessionRepository.cpp:384、2258`。读取数据库文件可得到访问 Token；密码走 CredentialStore 不保护这些 Token。

有效期和权限取决于服务器策略，不能一概等同“永久凭据”；AGENTS.md 明文禁止的是**明文存密码**和在日志中记录 Token/Cookie，不是原文引用的“禁止明文存凭据”。README 也披露 Token 保存在 SQLite。

安全存储迁移仍值得做，需包含旧库迁移、会话清除、CLI/桌面共用及存储不可用策略。本次没有确认远程泄漏或读取受保护数据库的额外攻击途径。

### H-10. WebDAV mutate 缺少认证挑战凭据

**结论：部分成立，已运行验证认证条件；建议 Medium。**

证据：`src/services/webdav/WebDavClient.cpp:222–227、401–432`。

DELETE/MOVE/COPY 的 reply 未设置 `webdavUsername` / `webdavPassword`。但 `configureRequest()` 已预置 Basic Authorization，接受此头的服务可成功，原文“必然 401”错误。

本地测试：相同假凭据下 Basic DELETE 成功；要求 Digest 挑战的 DELETE 失败。挑战回调从缺失属性取空凭据，应统一认证属性设置。COPY/MOVE 有相同发送缺口；不能以 Basic 成功认定全部认证方案正确。

### H-11. 列表 JSON 错误被当作空结果

**结论：成立，已运行验证解析函数；建议 Medium。**

证据：`src/services/media/MediaServerClientBase.cpp:359–389`；`src/services/emby/EmbyClient.cpp:499–525`。

列表解析未检查 JSON 错误和文档形状。调用受保护的生产解析函数，非法 HTML 和损坏 JSON 均返回空 vector；2xx 响应通过网络层后可能显示空列表。Emby 的 `fetchItemDetails()` 使用 `parseItems()`，空结果被报告为 `Media item was not found`。

独立 `parseItemDetails()` 已有错误检查，不能把全部详情解析都算入。原“gzip 未解压”无证据，已删去。应使用可表达错误的返回类型，检查 JSON、根节点、必需字段并传播错误；合法空集合应继续成功。

## M 类原始条目复核

| 编号 | 结论 / 建议级别 | 修正后的事实与证据 |
| --- | --- | --- |
| M-1 | **不成立；已实测** | QSQLITE 可重复使用同名占位符。`SELECT :x,:x` 绑定一次得到 `7,7`；`SessionRepository.cpp:831` 的四种来源实际筛选也返回正确记录。无需拆占位符。 |
| M-2 | **不成立为现有设计缺陷；已实测替换** | `SessionRepository.cpp:239–243、295–297、702、784` 按稳定媒体身份只保留最近一次，符合 [GlobalPlaybackHistory.md](VIBEDOCS/GlobalPlaybackHistory.md)。普通/隐私重播互相替换属实，但不能假设承诺两套独立历史。改变规则需同步索引、迁移和 Link 历史；不与 C2 混为一项。 |
| M-3 | **部分成立；Medium；已注入失败** | `SessionRepository.cpp:1509–1544` 多步删除无事务。最终服务器 DELETE 注入失败后，session 已删，服务器/任务仍在，确认部分状态。成功删除时外键 ON DELETE CASCADE 会移除 IPTV/调度子表，不能声称必有孤儿；文件原子性也非数据库事务可独自保证。 |
| M-4 | **成立；Low；已实测** | `SessionRepository.cpp:1044、1548–1645` 中 true 加载全部卡片，false 仅普通卡片。A普通=0、B私有=1、C普通=2，普通模式把 C 移到首位后得到 C=0、A=1、B=1。并列属实，但“两种模式各编一套”及必然随机排序错误。 |
| M-5 | **部分成立；Low；静态** | `src/services/encryptedhls/EncryptedHlsPackager.cpp:30–41` 未检查完整读取。I/O 错误或文件在 size 检查后变化可能短读；未复现普通打包必然截断。应检查实际读取及 QFile 错误。 |
| M-6 | **不成立；官方契约 + 已实测** | QNAM 不取得上传 QIODevice 所有权，reply 父子关系维持存活期。setParent 转移单一 QObject 父对象，不产生双重所有权。实际 PUT 成功后销毁 reply，payload 析构一次；不应删除此管理方式。[Qt PUT 设备契约](https://doc.qt.io/qt-6/qnetworkaccessmanager.html#put) |
| M-7 | **部分成立；Low；静态** | `src/utils/AppLogger.cpp:14–28` 文件日志无轮转/显式串行化；仅设置 VIBEPLAYER_LOG_FILE 时追加。缺少 mutex 不能证明 Windows 覆盖丢行，Append 也非 Truncate。交错/丢失需压力测试，可改善轮转与错误诊断。 |
| M-8 | **待验证；性能线索** | `AppViewModel.cpp:5405、5494、9716、10160、10887、10916` 有 GUI 同步数据库访问。但进度在 9709 行按位置差 10 秒或间隔 15 秒节流，页查询有界，不能称每个位置事件都写库、全部都是全表扫描。大库及 3 秒 busy 等待可能阻塞，需耗时采样。 |
| M-9 | **成立；Low；已实测** | `src/services/iptv/IptvParser.cpp:74–78、160–162` 取首逗号；缺少 tvg-name 时引号属性内逗号污染名称。实测名称为 `b.png" group-title="Movies",Proper Name`，但 group=Movies、sortOrder=0 正确，“分组/排序错乱”无依据。 |
| M-10 | **不成立为缺陷；平台边界** | `src/services/smb/SmbClient.cpp:65–81` 用 Windows 系统 SMB 重定向器。libsmb2 是推荐，不是强制；无 WNetCancelConnection2 不能推出 C++ 内存泄漏，系统会话非进程内对象。非 Windows 明确不支持，是 [SmbClient.md](VIBEDOCS/SmbClient.md) 记录的范围；跨平台或显式断连另立需求。 |
| M-11 | **成立；Low；静态** | `src/network/NetworkClient.cpp:148–155`、`src/services/webdav/WebDavClient.cpp:466–469` 把 OperationCanceledError 归为 Timeout，未区分主动 abort/计时器 abort。CLI networkFailure 检查取消返回 130，部分 GUI 过期回调也忽略，所以并非所有用户取消显示超时。 |
| M-12 | **不成立；官方契约 + 已实测** | `src/cli/CliEngine.cpp:64–75`、`src/cli/CliSmb.cpp:63–79` 用 QSaveFile，未开 directWriteFallback。写临时文件，commit 才发布；中途 return 不截断旧目标。实测未 commit 保留 original，commit 后变 new；现有 CLI 覆盖失败保护测试也通过。[QSaveFile 语义](https://doc.qt.io/qt-6/qsavefile.html#details) |

## 原 Low 条目逐项复核

原文没有编号，下面严格按原来的 28 行顺序编号。级别仅对确认或部分成立的剩余问题给建议。

| 编号 | 原议题 / 源码位置 | 结论与修正 |
| --- | --- | --- |
| L-01 | IPTV 父主键变更；`SessionRepository.cpp:488–494` | **部分成立；Low**。不同 playlist ID 可触发外键更新失败；当前共享流程固定 `iptv-playlist-{serviceId}`，不触发。外键启用时失败会回滚，不能假设留悬空子记录。 |
| L-02 | 旧 sessions 重复；`SessionRepository.cpp:2267–2274` | **成立；Medium；已实测**。限定旧表含 id、无唯一约束且存在重复身份时，建索引失败导致初始化返回错误。两条假 session 旧库复现 UNIQUE constraint failed；新库正常，不是所有旧库必坏。 |
| L-03 | ALTER 失败中断、未设 WAL；`SessionRepository.cpp:2202–2228` | **不成立为独立缺陷**。迁移失败返回错误正确；已有 QSQLITE_BUSY_TIMEOUT=3000。WAL 是并发方案选择，仍只有一个写者。不能把无 WAL 本身定为 bug，或忽略 ALTER 错误当作修复。 |
| L-04 | 代理请求头无界；`WebDavPlaybackProxy.cpp:112–123` | **成立；Low；静态**。等待完整请求头前无长度上限和空闲时限，上游超时尚未建立。监听 loopback，威胁来自本机客户端，不能扩大为直接互联网暴露。 |
| L-05 | mpv info 日志 + 40 ms 轮询；`PlayerController.cpp:245、319、795` | **待验证**。有界队列及过慢消费可丢事件属官方行为，但当前每次排空队列；无压力数据证明 info 必然使字幕失败或同步命令挂死。[事件队列契约](https://raw.githubusercontent.com/mpv-player/mpv/master/include/mpv/client.h) |
| L-06 | mpv 回调重入；`PlayerController.cpp:795–912` | **部分成立；潜在 High**。直接处理器若 shutdown 或经 MpvVideoItem::stop 销毁句柄，后续可读旧 event 或以空 m_mpv 调用 wait_event。PlayerController::stop 本身不销毁句柄，原文错误。API 说明 event 在句柄销毁后失效；需验证实际触发链及崩溃。[事件存活期](https://raw.githubusercontent.com/mpv-player/mpv/master/include/mpv/client.h) |
| L-07 | setlocale 覆盖 QLocale；`PlayerController.cpp:278` | **不成立**。设置 C 运行库 LC_NUMERIC，没有调用 QLocale::setDefault；libmpv 明确要求 LC_NUMERIC 为 C。不能因重复调用认定界面地区被覆盖。[locale 要求](https://raw.githubusercontent.com/mpv-player/mpv/master/include/mpv/client.h) |
| L-08 | 截图负 stride 越界；`PlayerController.cpp:1295–1319` | **不成立为原推导**。mpv 允许负 stride，规定 `data + y*stride` 访问；data 可指逻辑首行而非最低地址，负偏移不自动越过实际分配。当前符合访问公式。极端数值/布局可另查，本次未确认越界。[screenshot-raw 契约](https://mpv.io/manual/stable/#command-interface)、[img->planes[0] 实现](https://github.com/mpv-player/mpv/blob/master/player/screenshot.c) |
| L-09 | initialize 忽略新 wid；`PlayerController.cpp:270–272` | **不成立于已查调用链**。幂等 initialize 属实，但 MpvVideoItem 重建前 shutdown，CLI 固定窗口，调度无窗口。未找到活句柄直接换窗口路径，不能称窗口重建必用旧 wid。 |
| L-10 | Wayland winId=0 / 非 Windows 指针；`MpvVideoItem.cpp:622–633、689、806–823` | **待验证**。未在 Wayland 运行，不能假设 winId 必为零或必弹窗口。geometry 入口有 native ID 守卫，destroy 清 ID；仅看 apply 内无判空不能确认现有路径解引用空指针。 |
| L-11 | Content-Range 数字转换；`EncryptedHlsPlaybackProxy.cpp:1121–1127` | **成立；Low；静态**。三次转换不读取 ok。多数异常被范围/长度拒绝，但 start/end 溢出变零可能通过部分检查；不能称每种溢出都接受或直接越界。应逐字段检查转换成功。 |
| L-12 | 第二 HTTP 请求挂连接；`EncryptedHlsPlaybackProxy.cpp:1235–1239` | **不成立于正常完成路径**。一连接一响应，使用 Connection: close，写完后 disconnect；忽略第二管线请求不意味首请求完成后永远挂着。上游停滞需另查其超时。 |
| L-13 | packager worker 裸 this；`EncryptedHlsPackager.cpp:514–528、764–776、851` | **不成立为当前 UAF**。析构先取消，再等待 encryption/codec/tar watcher，成员尚未销毁。等待可能影响关闭耗时，属维护建议，不能同时说“等待兜底”和“确认 UAF”。 |
| L-14 | CBOR 四轮 offset 不收敛；`EncryptedHlsTarContainer.cpp:283–300` | **待验证**。四轮后还有第五次 encode，“落盘第四次、offset 第三轮”错误。无显式收敛检查属实，但无允许大小范围内不收敛案例；现有测试通过不证明所有边界，也不证明全部偏移错误。 |
| L-15 | FFmpeg first(-1)；`EncryptedHlsRestorer.cpp:225–226` | **成立；Medium；静态契约确认**。不含 `=` 的完整行使 separator=-1，first 负长度是未定义行为。正常进度通常不触发，异常输出需校验；本次未让真实 FFmpeg 输出异常行。[Qt first 前置条件](https://doc.qt.io/qt-6/qbytearray.html#first) |
| L-16 | S3 XML 错误为空；`TsslBackupService.cpp:209–214、572–578` | **成立；Medium；静态**。setContent 失败返回空列表，调用方无解析失败信息；2xx 非法 XML 可报没有备份，应区别合法空列表与解析失败。 |
| L-17 | S3 重复续页 Token；`TsslBackupService.cpp:575–576` | **成立；Medium；静态**。持续 truncated、相同非空 Token 会不断请求，未校验前进/去重；每页 60 秒不限制整个分页。仍可取消，不是 UI 必然冻结；需补异常服务集成测试。 |
| L-18 | Cli await 空 optional；`CliRuntime.h:27–40` | **部分成立；Low**。若循环在完成回调前被应用退出终止，未检查 optional，并有晚回调捕获栈变量风险。正常 Ctrl+C 走取消回调，不直接 quit；不能称通常取消必崩。 |
| L-19 | 重启丢 PIN 自动恢复；`CliDaemon.cpp:51–52、96–99、194–203` | **不成立**。重启标为 interrupted，不自动执行；显式 resume/retry 验证 PIN 后再填内存 map。不持久化 PIN 符合安全设计。[后台任务文档](VIBEDOCS/CliBackgroundTasks.md) |
| L-20 | IPC 超长请求等 60 秒；`CliIpc.cpp:78–87、102` | **不成立为原结论**。abort 后检测 UnconnectedState 跳出等待，再报缺响应；非固定等 60 秒。left(-1) 返回全部已有字节，后续有 JSON 错误检查，不意味垃圾被成功执行。可改善缺换行的协议诊断。 |
| L-21 | 锁一秒失效；`CliDaemon.cpp:41`、`PlaybackMain.cpp:41` | **不成立；已实测**。零关闭按文件年龄失效判断，不是设一秒；1250 ms 后第二把锁获取失败，长期宿主适合此设置。[QLockFile 说明](https://doc.qt.io/qt-6/qlockfile.html#details)、[Qt 6.7.3 实现](https://github.com/qt/qtbase/blob/v6.7.3/src/corelib/io/qlockfile.cpp) |
| L-22 | shell 吞脚本错误；`src/cli/main.cpp:69–81` | **不成立；现有测试验证**。TTY 交互返回零；重定向脚本累积失败并返回，--fail-fast 可立即停止。缺失服务脚本实际返回 4；不能把 interactive 分支套非交互执行。 |
| L-23 | watcher 泄漏及裸 this；`MediaServerClientBase.cpp:396–417`、`EmbyClient.cpp:280–302、319` | **部分成立；生命周期风险**。正常完成 watcher deleteLater，未证明通常泄漏。部分回调捕获裸 this；若提前销毁客户端且事件循环继续运行，需存活期保护。长期持有客户端不等于每次 UAF。 |
| L-24 | 登录 POST 301/302；`NetworkClient.cpp:98–102` | **部分成立；兼容性边界**。Qt 跟随 301/302 可转 GET，有官方依据；不意味所有登录失败，307/308 或最终地址不同。已有错误传播，“静默”需运行证据。明确策略和诊断，勿盲目重发凭据。[Qt redirect 规则](https://doc.qt.io/qt-6/qnetworkrequest.html#RedirectPolicy-enum) |
| L-25 | 远程 IPTV 相对路径；`IptvParser.cpp:181–195` | **不成立于当前 CLI 共享流程；已测试**。远程列表已支持，`src/services/management/ServiceOperations.cpp:69–72` 以 origin.resolved 解析频道/台标。低层 parser 保留 URL 不说明高层未处理；不宣称桌面所有本地相对路径均覆盖。 |
| L-26 | AppSettings 无引用；`src/settings/AppSettings.h` | **不成立为运行缺陷**。struct 未被生产流程引用，是维护清理项；不使用就不能导致运行时默认值不一致。 |
| L-27 | logout 卡片状态过期；`AppViewModel.cpp:8072–8101` | **不成立为所述原因**。logout 重置内存会话，未调用 repository.clearSession；会话仍保存，“已保存会话”非“当前登录”。lastUsedAt 是历史时间，不应仅因 logout 清零。 |
| L-28 | 分页计数无限加载；`AppViewModel.cpp:5376–5384、5424–5487` | **不成立**。offset 按数据库已消费行数前进，即使无有效行，下页也不重复，最终到尾；按过滤后数量反而可能重复。文件缺失条目一般 unavailable 后仍显示。可优化空页体验，非无限循环。 |

## 历史审计状态对照（不重复计入 54 项）

历史编号来自 [AUDIT-REPORT.md](AUDIT-REPORT.md)，与本文编号不同。历史报告说明既有 High/Low 未全部独立复核，其标签不能代替证据。

| 历史编号 | 议题 | 当前修订结论 |
| --- | --- | --- |
| C1 | 自签名证书嵌套循环 | 当前服务器级显式策略，原弹窗机制已移除；不沿用“整个 GUI 永久冻结”。 |
| H1 | WebDAV socket UAF | 见 H-7，待验证风险，不标成已复现。 |
| H2 | mpv/窗口顺序 | 见 C1，顺序仍在，崩溃/平台细节待验证。 |
| H3 | 定时提前记账 | 见 H-2，自动触发去重，不是完成标志。 |
| H4 | COMMIT 不收尾 | 见 H-1，成立，限定提交错误与连接影响。 |
| H5 | SQLite 跨线程/清理 | 当前析构 removeDatabase，后台独立连接；历史实现已调整，不保证全部未来访问正确。 |
| H6 | 列表 JSON | 见 H-11，仍成立。 |
| M2 | 非有限参数/pause | 当前 isfinite 和 cycle pause，已有对应处理。 |
| M3 | http-user 选项 | 当前 options/http-header-fields，原实现已替换。 |
| M5 | 加密准备裸 this | 当前按值捕获路径/目录，完成绑定 watcher；不与 packager 等待析构混淆。 |
| M7 | 自签名策略不一致 | 当前共享服务器级允许；不重复计作嵌套确认缺陷。 |
| L1 | 请求头无上限 | 见 L-04，仍成立。 |
| L2 | 短读/明文清理 | 本文核对短读剩余问题，见 M-5；已有清零处理不证明全部暂存内容清零。 |
| L3 | 设备双重所有权 | **误判，见 M-6**；撤销“已扩散”，没有应修的双重所有权。 |
| L4 | IPTV 父主键 | 见 L-01，不同 ID 防护问题，当前固定 ID 不触发。 |
| L5 | 跨隐私排序 | 见 M-4，普通子集可并列；隐私模式非另一独立子集。 |
| L6 | 删除无事务 | 见 M-3，失败部分状态，非成功后的外键孤儿。 |
| L7 | 取消 Timeout | 见 M-11，模块分类仍在，非全部 UI/CLI 取消误报。 |
| L8 | DST 春季缺失小时 | **“跳过当天”在当前 Qt 6.7.3 不成立**。Europe/Berlin 的 2026-03-29 02:30 实际为当天 03:30+02:00。沿用默认 transition resolution；是否跳过/拒绝/顺延应明确，其他 Qt/时区另测。[Qt 时间转换](https://doc.qt.io/qt-6/qdatetime.html#TransitionResolution-enum) |
| L9 | missed ID/checkpoint | 见 H-3，主要前提错误，保留删除 ID 清理不及时。 |

## 基线复核的验证记录与限制

临时程序在被忽略的 `build-clang/audit-20261007/`，直接编译相关生产 .cpp，不改实现。数据库、INI、列表和文件均在独立临时目录；HTTP 服务仅用假凭据。异常退出的首轮测试未计为成功；补齐测试日期后完整运行通过。

运行记录：`build-clang/audit-20261007/probe-results.txt`。真实输出要点：

```text
Qt=6.7.3
C2: 四种私有记录普通查询可见；按日查询 4 条；普通按日删除全部 4 条
M-1: SELECT :x,:x = 7,7；生产历史来源筛选正常
M-2: 同媒体身份以新私有记录替换旧普通记录
H-3: 普通模式 1 任务；隐私模式普通+私有，共 2 任务
M-3: 成功删除级联清理；最终 DELETE 失败则卡片/任务留存、session 已删
M-4: 普通子集重排后 A=1,B=1,C=0
L-02: 带 id 且重复身份的旧 sessions 初始化失败
M-12: 未 commit 保留 original；commit 后发布 new
L-21: 1250 ms 后仍不能获取现存活锁
M-9: 引号属性内逗号污染名称，分组/排序未变
H-1: COMMIT=database is locked；BEGIN=transaction within a transaction
历史 L8: nextOccurrence=2026-03-29T03:30:00+02:00
M-6: PUT payload 析构一次
H-11: 非法 HTML/JSON 得到空列表
H-10: Basic DELETE 成功；Digest 挑战失败
H-5: 暂停保留 20480 字节；恢复清零，第二次 GET 无 Range
ALL AUDIT PROBES PASSED
```

**现有相关测试**：8 个 CTest 目标 **8/8 通过，34.13 秒**：cli、link_playback_history、playback_history、webdav_client、encrypted_hls_tar_container、transfer_manager、media_server_search、scheduled_playback_schedule。记录：`build-clang/audit-20261007/regression-tests.txt`。

另运行 CliIntegrationTest 的 extendedFileCommandsAndScriptExitCodes、mediaAndRemotePlaylist、backgroundTransferSurvivesWorkerRestart（两组数据）：**4 个功能用例通过**，含初始化/清理共 6 passed，8.638 秒。记录：`build-clang/audit-20261007/cli-tests.txt`。

证明既有脚本退出码、在线 IPTV、覆盖失败保护、进程重启 Range 续传等路径有效，**不证明全部异常条件被覆盖**。本次未重跑全部 25 个目标，未做真实服务器登录、真实慢速 60 秒上传、GUI 播放压力测试、ASan、macOS/Wayland 验收，也未对 S3 异常 XML/重复 Token 作端到端复现。静态结论已标注，未验证风险没有写成已复现。

## 基线修复建议（保留复核记录）

以下是复核阶段给出的修复优先级，不是当前剩余任务清单；当前进展与限制见顶部修复状态表。

1. **先处理 C2、H-1**：隐私规则与全部查询/删除保持一致；提交失败统一收尾。补全局隐私播放和锁竞争回归案例。
2. **处理 H-5、H-6、H-10、H-11**：残片策略、延迟刷新接线、挑战认证属性、列表错误传播。H-6 接通后同时验证 H-8。
3. **异常输入与部分状态**：L-15 进度行、L-16/L-17 S3 解析/分页、M-3 删除原子性、L-02 旧库迁移、L-04/L-11 请求边界。
4. **独立验证播放器存活期**：C1、H-7、L-06，取得触发链和诊断后定修复方式；不写成“一行即可消除的必然崩溃”。
5. **按策略排期**：H-4 超时/重试、H-9 Token 安全存储、M-4 排序、M-5 短读、M-11 取消分类；调度重试和独立隐私历史需明确行为。

不为 M-1、M-6、M-12、L-21 等误判改代码；不建议去掉设备父对象、取消 QSaveFile 原子发布、让旧回调无条件改新状态，或持久化私有任务 PIN。原“无任何缺陷/无 nonce 复用/无路径穿越”等绝对断言已删除，本次复核没有提供证明这些全局结论所需的证据。
