# 代码审计报告

- 项目：vibeEmbyPlayerQT（Qt Quick + libmpv 桌面媒体播放器）
- 日期：2026-08-11
- 类型：静态审计 + 逐项复核 + 修复验证
- 状态：**Critical / Medium 已复核；误报已删除，确认问题已修复**
- 范围：播放器、WebDAV 加密 HLS 管线、数据库/调度、网络层、Emby/Jellyfin 客户端、ViewModel/QML 接线

本报告问题按严重程度排序。所有文件:行号均基于复核时的工作区内容。复核列标注了该项是否已在第二轮逐行核对确认。

---

## 严重（Critical）

### C1. 自签名证书确认使用嵌套事件循环和单一回调（已修复；复核建议降为 High）

| 项 | 内容 |
| --- | --- |
| 文件 | `src/network/NetworkClient.cpp`；`src/viewmodels/AppViewModel.cpp/.h`；`qml/Main.qml` |
| 复核 | 真实问题，但原报告对“整个 GUI 永久冻结”的表述过重 |
| 状态 | 2026-08-15 已修复 |

原实现从 `sslErrors` 处理器进入嵌套 `QEventLoop`，同时由 `AppViewModel` 仅保存一个证书回复回调。并发握手可覆盖先前回调，使对应请求栈无法退出。嵌套事件循环仍会处理 UI 事件，因此不能笼统认定整个 GUI 必然冻结，但请求悬挂和重入风险真实存在。

修复：
- 删除嵌套事件循环、单一待处理回调、信号转发和 QML 请求级确认对话框。
- 服务器未显式允许自签名证书时维持 Qt 默认验证失败行为。
- 显式允许时仅忽略该 reply 当前报告的 SSL 错误，并记录不含凭据的警告日志。
- Emby、Jellyfin 与 WebDAV 统一使用持久化的服务器级证书策略。

---

## 高（High）

### H1. `WebDavPlaybackProxy` 中 `QTcpSocket` 存活期竞态（use-after-free 风险）

| 项 | 内容 |
| --- | --- |
| 文件 | `src/services/webdav/WebDavPlaybackProxy.cpp:191-218` |
| 复核 | 已确认 |

`readyRead`/`finished` lambda 直接捕获裸指针 `socket`，而 socket 由 `disconnected -> deleteLater`（118 行）管理。客户端断开/seek/stop 时，socket 可能先被销毁，而 reply 仍在投递 `readyRead`，届时 `socket->state()` / `socket->write()` 访问已释放内存。

同目录 `EncryptedHlsPlaybackProxy` 使用 `QPointer<QTcpSocket>` 防护，此处缺失，属真实可触发缺陷（seek/stop 在代理场景很常见）。

### H2. libmpv 与宿主 HWND 的销毁顺序错误

| 项 | 内容 |
| --- | --- |
| 文件 | `src/player/MpvVideoItem.cpp:635-659`；`PlayerController::shutdown` at `PlayerController.cpp:586-599` |
| 复核 | 已确认 |

`destroyNativeWindow()` 先 `DestroyWindow(hwndFromId(...))`（640 行），之后才调用 `m_controller.shutdown()`（657 行 → `mpv_terminate_destroy`，594 行）。两步之间 libmpv 的 VO/渲染线程仍引用已销毁的 HWND，可能导致 teardown 崩溃或异常。安全顺序应为：先 `mpv_terminate_destroy`，再销毁窗口。

触发：正常停止、item 移出场景（ItemSceneChange 窗口为空）、清空源、item 销毁。

### H3. 定时播放：任务在真正播放成功前就被标记为已运行

| 项 | 内容 |
| --- | --- |
| 文件 | `src/services/scheduler/ScheduledPlaybackManager.cpp:290-306` |
| 复核 | 已确认 |

`evaluateScheduledTasks()` 对每个到期任务**无条件**先持久化 `lastRunDate` 并推进 checkpoint（293、312 行），之后才 `enqueueScheduledTask` / 开始播放。若随后播放失败（会话失效、认证失败、网络错误 → `failTask`），该次运行仍被记为已完成，任务被静默跳过直到下一调度窗口，而非重试或标记错过。

### H4. 数据库事务 COMMIT 失败时未回滚，遗留打开事务

| 项 | 内容 |
| --- | --- |
| 文件 | `src/database/SessionRepository.cpp:510-513, 867-870, 1303-1306, 1354-1357, 1668-1670, 1746-1748` |
| 复核 | 已确认 |

所有事务函数中 BEGIN 与各 DML 失败均有 `ROLLBACK`，唯独最终 `COMMIT` 失败直接 `return std::unexpected(...)` 而未 `ROLLBACK`。COMMIT 失败（磁盘满、I/O 错误）会遗留持锁打开事务，后续写入报 "database is locked"。

### H5. `SessionRepository` 未移除数据库连接，且连接非线程安全

| 项 | 内容 |
| --- | --- |
| 文件 | `src/database/SessionRepository.cpp:99-104, 1543-1563` |
| 复核 | 已确认 |

析构只 `m_database.close()` 不调用 `QSqlDatabase::removeDatabase()`，连接残留在 Qt 全局注册表（退出告警）。且 `QSqlDatabase` 连接为线程局部，任一方法从 worker 线程调用会与主线程共享/竞争 `m_database` 成员（定时播放评估是潜在触发点）。

### H6. `parseLibraries` / `parseItems` 忽略 JSON 解析失败

| 项 | 内容 |
| --- | --- |
| 文件 | `src/services/media/MediaServerClientBase.cpp:361-391` |
| 复核 | 已确认 |

这两处未像 `login`/`parseItemDetails`（395 行起检查 `QJsonParseError`）那样校验解析错误。2xx 但响应体非合法 JSON（HTML 错误页、截断响应）被静默当作空列表成功返回，UI 显示空媒体库而非报错。

---

## 中（Medium）

### M2. 非有限播放参数与缓存暂停状态（已修复；复核建议降为 Low）

| 项 | 内容 |
| --- | --- |
| 文件 | `src/player/PlayerController.cpp` |
| 复核 | 部分真实；`seekAbsolute(NaN)` 会被 `std::max(0.0, NaN)` 转为 `0.0`，原报告对此描述错误 |
| 状态 | 2026-08-15 已修复 |

确认存在的问题是 `seekRelative`、倍速输入和部分观察属性缺少有限数校验，以及 `togglePause` 依赖可能滞后的 `m_paused`。修复后所有相关输入/观察值先通过 `std::isfinite`，非法值被拒绝并记录日志；暂停切换改用 libmpv 官方 `cycle pause` 命令，不再读取缓存状态决定方向。

### M3. HTTP Basic Auth 使用不存在的 libmpv 选项且忽略错误（已修复）

| 项 | 内容 |
| --- | --- |
| 文件 | `src/player/PlayerController.cpp` |
| 复核 | 已确认；捆绑 libmpv 不包含 `http-user` / `http-password` 选项 |
| 状态 | 2026-08-15 已修复 |

修复后通过官方支持的运行期 `options/http-header-fields` 属性设置或清空 `Authorization: Basic ...`，不记录凭据并在使用后清零临时字节缓冲。认证头或 TLS 策略设置失败会记录诊断、向 UI 报错并中止加载，避免旧服务器的认证头或不安全 TLS 状态被带到下一次播放。

### M5. 本地加密 HLS 准备任务捕获裸 `this`（已修复）

| 项 | 内容 |
| --- | --- |
| 文件 | `src/services/webdav/EncryptedHlsPlaybackProxy.cpp` |
| 复核 | 部分真实；确认代理销毁时的 UAF 风险，未发现 `TsslStore` 内存数据竞争 |
| 状态 | 2026-08-15 已修复 |

原 `QtConcurrent::run` worker 捕获代理裸指针，并可能在代理销毁后调用成员函数，构成真实 UAF 风险。`TsslStore` 仅保存构造后不变的目录字符串，各方法使用局部文件对象，原报告不能据“没有内部锁”推导出 C++ 内存数据竞争。

修复后 worker 仅按值捕获规范化路径和 TSSL 存储目录，在 worker 内创建独立的值语义 `TsslStore` 视图；不再解引用代理对象。完成通知仍绑定代理拥有的 `QFutureWatcher`，代理销毁后自动丢弃。

### M7. 自签名证书策略在媒体服务和 WebDAV 间不一致（已修复）

| 项 | 内容 |
| --- | --- |
| 文件 | `src/network/NetworkClient.cpp`；`src/services/webdav/*`；`src/viewmodels/AppViewModel.cpp` |
| 复核 | 部分真实；回调覆盖与 C1 重复，原文案本身明确写的是“允许自签名证书确认” |
| 状态 | 2026-08-15 已修复 |

真实问题是同一持久化字段在 Emby/Jellyfin 路径表示“允许弹出请求级确认”，在 WebDAV 路径却表示“直接允许”，行为不一致。修复后所有来源统一为服务器级显式允许策略，UI 文案同步为“允许自签名证书”；重复的回调覆盖问题归入 C1，不再作为独立漏洞计数。

---

## 低（Low）

| 编号 | 文件:行 | 问题 | 复核 |
| --- | --- | --- | --- |
| L1 | `WebDavPlaybackProxy.cpp:108-117` | 请求头缓冲区无上限、连接无超时，本地客户端持续发送无 `\r\n\r\n` 数据可无界增长（加密 HLS proxy 已有 64KiB 上限 + 431，此处未同步） | 已确认 |
| L2 | `EncryptedHlsPackager.cpp:26-37, 260` | `readBoundedFile` 未校验完整读取，短读会用截断数据加密；明文 manifest/文件名缓冲区未用后清零 | 已确认 |
| L3 | `TransferManager.cpp:920-928` | 上传 `put(request,file)` 后又 `file->setParent(reply)`，manager 与 reply 对 device 双重所有权（下载路径无此问题） | 已确认 |
| L4 | `SessionRepository.cpp:462-486` | `saveIptvPlaylist` 重导入改写父主键（`ON CONFLICT(service_id) DO UPDATE SET id=...`），子表 FK 仅 `ON DELETE CASCADE`；当前调用方用确定性 id 故不触发，属潜在隐患 | 已确认 |
| L5 | `SessionRepository.cpp:1261-1308` | `moveServer` 仅在当前隐私子集内重排，跨隐私模式可能产生重复 sort_order | 已确认 |
| L6 | `SessionRepository.cpp:1205-1258` | `deleteServer` 多表删除无事务，中途失败遗留部分状态 | 已确认 |
| L7 | `NetworkClient.cpp:132-141`；`WebDavClient.cpp:361-366` | `OperationCanceledError` 一律归类为 Timeout，外部主动 abort 被误报超时 | 已确认 |
| L8 | `ScheduledPlaybackSchedule.cpp:99-101` | DST 春令时"缺失小时"内构造的本地时间无效，该日任务被跳过至下周期 | 已确认 |
| L9 | `ScheduledPlaybackManager.cpp:234-240, 248-255` | 普通模式下错过任务 id 永不清理并随隐私模式迁移；隐私模式运行时仍写普通 checkpoint | 已确认 |

---

## 未发现问题的模块

以下模块经重点审查未发现明确缺陷：

- `AesGcmDecryptor`：IV 长度（`EVP_CTRL_GCM_SET_IVLEN` = 0x9）、GCM tag 设置/读取顺序、`[IV][ciphertext][tag]` 布局、分块/计数器处理一致正确。
- `HlsManifestValidator`、`TsslStore`、`TlsCertificateStore`：格式/签名/原子写正确；无手动 OpenSSL 分配，无泄漏/双释放。
- `EncryptedHlsBatchPackager`、`CredentialStore`、`AppSettings`：无逻辑或内存缺陷（`CredentialStore` 已复制 blob 后再 `CredFree`，无悬垂读）。
- 数据库层：无 SQL 注入（全部预编译语句/常量 DDL），列索引均匹配，无除零；`QSqlQuery` 均函数局部，无作用域外使用。

---

## 优先级建议

Critical / Medium 复核范围内的确认问题已全部修复。剩余优先级：
1. H1：WebDAV 播放代理 socket use-after-free（seek/stop 易触发）。
2. H2：mpv 与宿主 HWND 销毁顺序。
3. H3 / H4：定时播放提前记账、事务 COMMIT 不回滚。

---

## 复核说明

本报告第一轮由静态分析产出。2026-08-15 对 Critical 与 Medium 逐项结合调用链、模块文档、Qt 行为和 libmpv 官方接口重新核对：删除 M1、M4、M6 三项误报；修正 C1、M2、M5、M7 的范围和严重度描述；修复 C1、M2、M3、M5、M7 中确认存在的问题。High 与既有 Low 项不在本轮修复范围，其原始结论仍待后续独立处理。
