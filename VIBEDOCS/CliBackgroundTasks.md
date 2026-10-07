# CLI 后台任务与检查点

CliTasks 准备任务和凭据，CliDaemon 管理 Core 进程，CliIpc 使用用户范围的
QLocalServer/QLocalSocket 传递单行 JSON。socket 名基于 profile 摘要，
同一 profile 一个宿主。桌面 TransferManager 的任务不属于此宿主。

~~~powershell
.\vibePlayerQT webdav download "家庭 DAV" /电影 "D:\下载\电影" --background --password-env DAV_PASSWORD
.\vibePlayerQT task list
.\vibePlayerQT task show TASK_ID
.\vibePlayerQT task pause TASK_ID
.\vibePlayerQT task resume TASK_ID
.\vibePlayerQT task retry TASK_ID
.\vibePlayerQT task cancel TASK_ID
.\vibePlayerQT task remove TASK_ID --yes
.\vibePlayerQT daemon stop
~~~

支持后台 WebDAV/SMB upload/download、video pack/restore、tssl backup/restore。
宿主一次执行一个顶层任务，底层视频并行数沿用设置。pause/resume 控制
正在传输的 WebDAV 文件和 SMB 文件块；规划、FFmpeg 和 TSSL 阶段不接受暂停，明确报冲突。
排队任务可暂停；cancel 使用现有取消接口。

数据目录保存 cli_tasks.sqlite3（命令、参数、状态、时间、结果），不保存密码/PIN。
检查点在 cli-task-checkpoints/ID，前台 --resume 在目标父目录创建
.vibe-resume-摘要。QSaveFile 发布元数据，保存 ETag、完成文件 SHA-256 和目标。
临时密码/环境凭据经 CredentialStore 的 CliTask/ID 保存；成功后删除 secret，
失败/取消保留便于重试，task remove 清理记录、凭据、暂存，保留最终输出。
没有安全后端时，含临时密码的持久任务拒绝提交。

隐私 PIN 仅在宿主内存，隐私任务查询/控制重新校验；重启后必须再次提供
--private --pin-env NAME。退出把未完成任务保留，重启标记 interrupted，
由 task resume 显式排队。下载强 ETag 一致时发送 Range/If-Range；
206 的起点、终点、总大小和 Content-Length 必须一致，最终文件大小也需匹配；
错误响应不发布文件，可在服务恢复后 retry。200 截断暂存重接收。
弱/未知 ETag 重新下当前文件。
已完成文件只有本地摘要和远程 ETag 一致才跳过。上传恢复同样验证；
缺少修订信息时可能需显式 --overwrite。取消不自动删除远程资源。
video/TSSL retry 重新执行业务命令，保留模块的同名/去重规则，不保存 FFmpeg 内部进度。
SMB 恢复按文件进行：源和目标 SHA-256 与检查点都相同时跳过已完成文件，
取消清理当前未完成的暂存文件；当前文件重新复制，不拼接 SMB 字节。
已完成任务拒绝 cancel，保留完成状态，仍可用 task remove 清理记录。

## CliCheckpoint 与 IPC 接口

`src/cli/CliCheckpoint.h` 提供 `fileHash`、`checkpointFile`、`checkpoint`、
`saveCheckpoint`。key 的 SHA-256 作为文件名，内容为单个 JSON 对象。
空目录参数关闭持久记录；root 保存最终目标（SMB 另含源），文件记录
etag/hash，partial:URL 保存暂存修订。摘要计算逐块检查暂停/取消；
WebDAV 调用放入工作线程，通过事件循环继续响应任务控制。
元数据使用 QSaveFile 发布，完成输出不会被 task remove 删除。

CliIpc 请求含 action/id 或 invocation，结果为 code/data/message，
使用换行作为帧边界，最多 1 MiB 请求、4 MiB 响应；启动参数只有 profile。
凭据仅通过用户范围 socket 传输或 CredentialStore 持久保存，不进命令行。
Windows 宿主启动关闭句柄继承并禁用 STARTF_USESTDHANDLES，
避免后台进程占着父命令的重定向管道，导致 PowerShell 输出捕获一直等待。
隐私校验前刷新 QSettings，已经运行的宿主也会立即拒绝旧 PIN。
serveRequests 为任务和播放宿主共用的接收器：接入后同时检查已有数据，
单次请求只处理一次，结果写完关闭连接。宿主先取得 QLockFile、
加载状态并安装处理器，随后才监听，避免首次请求早于准备完成。
锁文件为 cli-tasks.lock / cli-player.lock，长期宿主禁用时间过期判断；
正常退出释放锁，进程意外退出后由 Qt 检测失效锁。

测试验证暂停、退出、重启、Range、最终内容和 SQLite 无明文密码。
依据：[用户 socket 权限](https://doc.qt.io/qt-6/qlocalserver.html#SocketOption-enum)、
[HTTP Range](https://www.rfc-editor.org/rfc/rfc9110.html#section-14)、
[QSaveFile](https://doc.qt.io/qt-6/qsavefile.html)。
长期单实例锁依据 [QLockFile](https://doc.qt.io/qt-6/qlockfile.html)，
Windows 启动参数依据 [QProcess](https://doc.qt.io/qt-6/qprocess.html#setCreateProcessArgumentsModifier)。
