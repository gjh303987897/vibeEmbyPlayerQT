# CLI 使用说明

用户操作步骤、完整参数表和可复制的脚本见 [CLI 完整使用教程](CLI-Tutorial.md)。
可直接跳转到 [快速开始](CLI-Tutorial.md#quick-start)、
[视频加密/解密](CLI-Tutorial.md#video) 或 [JSON 与自动化](CLI-Tutorial.md#automation)。
本文保留命令概览、模块边界和实现/验证信息。

## 入口与运行

Windows 发行包的 `bin` 中同时安装 `vibePlayerQT.exe`（桌面）和
`vibePlayerQT.com`（控制台 PE 程序）。CMD 和 PowerShell 默认的 PATHEXT
先匹配 `.COM`，因此可以使用同名入口；如果自定义过 PATHEXT，请显式写 `.com`。

```powershell
cd D:\workspace\vibeEmbyPlayerQT\build-clang
.\vibePlayerQT list
.\vibePlayerQT.com list --json
.\vibePlayerQT shell
.\vibePlayerQT.exe
```

`.com` 无参数启动旁边的桌面 `.exe`。查询命令仅创建 `QCoreApplication`，
不初始化 QML、窗口、托盘、播放器、定时播放或更新检查。
macOS 的入口位于 `vibePlayerQT.app/Contents/MacOS/vibePlayerQT-cli`；
Linux 使用 `bin/vibePlayerQT-cli`，便携包的 `vibePlayerQT.sh` 有参数时转到 CLI。
CLI 与桌面任务各自运行。CLI 后台任务由独立任务进程管理；
play 另启动同目录的 vibePlayerPlayback.exe，由 PlayerController 管理 libmpv。

默认使用与桌面相同的应用名称、SQLite、QSettings、TSSL 目录和系统凭据。
`--data-dir DIR` 可隔离 SQLite、TSSL、IPTV 导入目录和 INI 配置，适合测试或
独立自动化配置；系统凭据仍按服务 ID 存储，因此使用隔离配置时也应避免复用
生产服务的 ID。Shell 启动后不能更改数据目录。

## 命令

服务选择支持完整 ID 或唯一名称；同名时必须给出 ID。
`list` 只读取数据库，不执行迁移、去重或历史清理；没有数据库时返回空列表。
写服务的命令复用迁移。SQLite 忙时最多等待 3000 ms，然后报告错误。

Windows 会话 Token 由用户范围 DPAPI 加密；旧会话在写模式初始化时迁移。
只读查询保持不迁移，复制数据库到其他 Windows 账户后可能需要重新登录。
详见 [Token 存储](SessionTokenCipher.md)。登录端点的 POST 重定向明确报错，
需要配置最终服务器地址。正常取消以及等待循环提前退出均返回 130，晚完成
回调不访问已经退出的命令栈。

| 命令 | 含义 |
|---|---|
| `list` / `service list` | 服务卡片；保存会话/凭据状态不表示在线 |
| `service show SERVICE` | 配置与保存状态，不显示 Token、密码 |
| `service add --type TYPE --name NAME --url URL --username USER` | Emby、Jellyfin、WebDAV、SMB |
| `service add --type iptv --name NAME --playlist FILE_OR_URL` | 本地或 HTTP(S) 播放列表 |
| `service update SERVICE [--name NAME] [--url URL] [--username USER] [--playlist FILE]` | 仅更改指定字段 |
| `service login SERVICE` | 登录并保存会话；WebDAV 验证根目录后保存密码 |
| `service check SERVICE` | 用现有会话或凭据检查；不修改配置 |
| `service remove SERVICE [--yes]` | 移除卡片、会话和密码；保留本地媒体、历史及 TSSL |
| `service logout SERVICE` / `service move SERVICE INDEX` | 清除登录或调整顺序，索引从 0 开始 |
| `webdav ls SERVICE [PATH]` | 列出目录，默认 `/` |
| `webdav stat SERVICE PATH` | 文件或目录信息 |
| `webdav mkdir SERVICE PATH [--parents]` | MKCOL；指定 parents 时创建祖先 |
| `webdav upload SERVICE SOURCE TARGET [--overwrite]` | 上传文件/目录 |
| `webdav download SERVICE SOURCE TARGET [--overwrite] [--resume]` | 下载、原子覆盖、保存续传记录 |
| `webdav remove SERVICE PATH --yes` | 删除资源及子项；拒绝服务根目录 |
| `webdav copy/move/rename SERVICE SOURCE TARGET [--overwrite]` | 标准 COPY/MOVE，默认拒绝覆盖 |
| `smb ls/stat/mkdir/upload/download/remove/move/copy` | Windows 原生 SMB 文件操作 |
| `tssl list` / `tssl show DIGEST` | 列表或元数据，不输出密钥 |
| `tssl import FILE...` | 报告导入、重复、失败数量 |
| `tssl export DIGEST FILE` | 导出一个完整密钥包 |
| `tssl export --all --output DIR` | 导出全部密钥包 |
| `tssl export DIGEST... --output DIR` | 导出多个选定包，摘要去重 |
| `tssl remove DIGEST... [--yes]` | 批量删除，一次确认，返回成功/失败数量 |
| `tssl backup` / `tssl restore` | 备份/恢复，默认保存的目标 |
| `video pack SOURCE...` | 文件/目录批量生成 M3U8S/M3U8SP |
| `video restore PACKAGE_OR_FOLDER... [--service WEBDAV]` | 递归发现本地或远程包并还原 |
| `media libraries/ls/search/show SERVICE ...` | Emby/Jellyfin 查询；列表支持 offset/limit |
| `media channels SERVICE` | IPTV 频道 |
| `play FILE_OR_URL` / `play SERVICE ITEM_OR_PATH_OR_CHANNEL` | 文件、HTTP、服务媒体播放 |
| `player status/pause/resume/stop/seek/volume/speed/subtitle/audio` | CLI 播放宿主状态、控制和轨道 |
| `task list/show/pause/resume/retry/cancel/remove` | CLI 后台任务 |
| `daemon run/stop` | 显式启动/停止任务宿主；提交时自动启动 |
| `privacy set/change/remove` | PIN 创建、更改、移除 |
| `config list` / `config get KEY` / `config set KEY VALUE` | 下载、视频、备份、外观和播放默认值 |
| `help [GROUP [COMMAND]]` / `--version` | 帮助与版本 |

服务添加/修改还支持 `--auto-login true|false`、`--trust-self-signed true|false`；
修改支持 `--private-mode true|false`（移动到隐私区需解锁）。
登录失败退出 3，不把旧会话当成新登录成功。
其他平台没有可用的现有安全凭据后端时，WebDAV 仍可通过环境变量完成任务，
结果中的 `credentialsSaved` 为 false。

### WebDAV 路径和冲突

远程 `/` 对应卡片保存的根目录。路径逐段编码，中文、空格、`#`、`%` 不会
变成 URL 的查询或片段；`..` 不允许逃出根目录。下载规划拒绝其他域名或
请求目录之外的资源，不会把认证信息发送给列表中嵌入的其他主机。

目标表示最终路径，而不是目标的父目录。例如：

```powershell
.\vibePlayerQT webdav upload "家庭 DAV" "D:\视频\合集" "/电影/合集"
.\vibePlayerQT webdav download "家庭 DAV" "/电影/合集" "D:\下载\合集"
```

文件夹保留层级，上传不跟随符号链接。父目录 `/电影` 必须已存在。
默认同名报错，上传的每个 PUT 都带 `If-None-Match: *`，防止检查后出现的
竞争覆盖。`--overwrite` 允许上传替换文件和合并已有目录。
下载默认通过不覆盖重命名发布，--overwrite 使用 QSaveFile 原子替换。
普通前台下载取消清理当前临时文件；--resume 或后台下载保留独立暂存。
强 ETag 一致时使用 Range/If-Range，200 响应安全重下当前文件。
无强 ETag 时不拼接旧文件。详见 [后台任务](CliBackgroundTasks.md)。

### 视频参数

默认继承 `config list` 中的保存设置。以下参数仅影响当前命令：

| 参数 | 值 |
|---|---|
| `--output DIR` | 本地输出目录 |
| `--webdav SERVICE --path PATH` | 打包后上传 |
| `--container` | `m3u8s` / `m3u8sp` |
| `--video` | `copy` / `h264` / `h265` / `auto` |
| `--audio` | `copy` / `aac` |
| `--quality` | `high` / `balanced` / `compact` |
| `--jobs N` | 1 至当前允许的并行数（最大 4） |
| `--ffmpeg FILE` / `--temp DIR` | FFmpeg 和临时目录 |
| `--auto-codecs h264,h265 --auto-target h264` | 自动编码规则 |
| `--keep-local` | WebDAV 上传成功后也保留本地包 |
| `video restore --format` | `original` / `mkv` / `mp4` |

只有 `config set` 持久修改设置。还原保留现有模块的同名自动编号行为。
上传失败或取消会保留本地打包结果，并在结果中返回保留目录。
视频 WebDAV 输出可准备祖先；普通 mkdir 使用 --parents 时同样支持。
video restore 使用 --service 从 WebDAV 下载包；远程 .m3u8s 下载其父目录的分段。
本地和远程目录递归发现包，临时下载在还原完成/取消后清理。
FFmpeg 未捆绑，由已有定位与能力探测模块处理。

### TSSL 备份目标

```powershell
.\vibePlayerQT tssl backup --target local --local-path "D:\备份\TSSL"
.\vibePlayerQT tssl restore --target webdav --service "家庭 DAV" --path "/备份/TSSL"
.\vibePlayerQT tssl backup --target s3 --s3-endpoint "https://s3.example.com" --s3-bucket media --s3-region us-east-1 --s3-prefix tssl --s3-access-key ACCESS_KEY --s3-secret-env S3_SECRET
```

不提供目标覆盖时使用 `tsslBackup/*` 保存设置。WebDAV 备份目录需要已存在。
S3 沿用 SigV4 和现有 HTTPS 要求，仅 `localhost` 开发端点允许 HTTP。
备份使用摘要命名，沿用已有备份模块的覆盖行为；恢复按摘要去重。
导出命令产生完整密钥文件，`list/show` 仅显示元数据。

## 隐私和凭据

可在桌面配置 PIN，也可用 privacy set --new-pin-env NEW_PIN --confirm-pin-env NEW_PIN。
privacy change 另需 --pin-env OLD_PIN，privacy remove --yes 需当前 PIN。
PIN 为 4 至 12 位数字。移除 PIN 后隐私服务仍保留，设置新 PIN 后才能访问。
独立命令必须明确请求 `--private` 并验证 PIN，
即使知道完整私密服务 ID，也无法跳过校验。

```powershell
.\vibePlayerQT list --private --pin-env PLAYER_PIN
.\vibePlayerQT service login "家庭 DAV" --password-env DAV_PASSWORD
.\vibePlayerQT shell --password-env DAV_PASSWORD --pin-env PLAYER_PIN
```

环境参数的值是**变量名称**。CLI 优先使用已有安全存储；缺失时，真正的
交互终端会隐藏输入。重定向、管道或自动化执行缺少凭据直接退出 3，
不会读取脚本下一行当作密码。没有明文密码、PIN 或 S3 secret 参数。
密码和会话 Token 不出现在输出、进度或 CLI 日志文件中。

删除操作在终端确认；脚本必须传 `--yes`，否则退出 2。
Shell 启动参数中的 `--yes` 不会自动传给后续删除命令。

## 交互终端

```text
use "家庭 DAV"
pwd
cd "/电影"
ls
mkdir "新 文件夹"
upload "D:\视频\影片.mp4" "新 文件夹/影片.mp4"
download "新 文件夹/影片.mp4" "D:\下载\影片.mp4"
privacy unlock
privacy lock
exit
```

简写使用当前 WebDAV 卡片及目录；完整分组命令仍可指定其他服务。
解锁只在当前 Shell 会话有效，锁定会清空选中的服务。单条 `--private`
不会永久解锁 Shell。支持单/双引号；反斜线保留原样，适用于 Windows/UNC
路径；引号内部连续两个相同引号表示一个字面引号。
Windows 支持 Tab 补全和方向键历史。history/history --clear 管理当前会话历史，
不写磁盘。支持 $NAME、环境变量大括号形式、$(CLI命令)、
| select/where/get/count 结果管道及末尾 & 后台提交。
表达式执行 CLI 命令及过滤，不调用外部系统 Shell。见 [CliShell.md](CliShell.md)。
重定向脚本保留最后一次失败退出码，shell --fail-fast 在首个错误退出。

## 自动化输出与取消

普通输出使用 UTF-8 对齐表格和字段列表，显示结果数量。服务的完整 ID/地址、
TSSL 的完整摘要/路径位于对应条目下方；长值按终端宽度换行，保持完整。
目录显示 `Folder` / `File`，大小显示 B / KiB / MiB 等单位，批量任务先显示
计数再显示明细。没有 ANSI 颜色码，输出重定向后仍可阅读。
`Saved` / `None` 只表示保存的会话或凭据，不代表在线。
详见 [CLI 输出模块](CliOutput.md)。

`--json` 每条命令输出一个对象，原始字段、字节数、布尔值与退出码保持不变：

```json
{"schemaVersion":1,"command":"service list","ok":true,"data":[],"error":null}
```

错误对象为 `{"code":数字,"message":"说明"}`，进度及日志写 stderr。
Shell 的 JSON 模式每条输入命令各输出一行对象。

| 退出码 | 意义 |
|---|---|
| 0 | 成功 |
| 1 | 执行失败 |
| 2 | 参数错误/脚本删除缺少 `--yes` |
| 3 | 认证或解锁失败 |
| 4 | 目标不存在 |
| 5 | 同名冲突 |
| 6 | 批量部分成功、部分失败（查看统计及明细）；全部失败返回 1 |
| 130 | 取消 |

```powershell
.\vibePlayerQT list --json > services.json
if ($LASTEXITCODE -ne 0) { throw "查询失败" }
$services = Get-Content services.json -Raw | ConvertFrom-Json
.\vibePlayerQT webdav upload $services.data[0].id "D:\视频\影片.mp4" "/影片.mp4" --password-env DAV_PASSWORD --json 2> progress.log
exit $LASTEXITCODE
```

Ctrl+C 调用现有取消接口并等待清理：独立命令退出 130；Shell 回到提示符。
已经完成的文件保留，不自动删除远程文件。
扩展版支持媒体查询、独立播放、远程文件操作和后台任务恢复。
播放命令控制 CLI 播放宿主；任务命令管理 CLI 宿主的任务。用法和范围见下方模块文档。

## 模块与验证

`CliCommand` 定义共用语法/分组帮助，`CliOutput` 呈现结果，
`CliEngine` 执行命令及 Shell 上下文，
`CliRuntime` 用 Qt 事件循环等待已有异步模块，`CliConsole` 封装平台终端输入/
取消及无参数桌面启动。Windows 启动桌面时明确使用正常窗口显示参数；
其他命令不调用此入口。`vibeCore` 是无 GUI 的共享库，`vibeCli` 和桌面链接同一基础设施。

相关流程见 [SharedWorkflows.md](SharedWorkflows.md)。
Windows 本地验收记录见 [CliVerification.md](CliVerification.md)。
播放、后台、SMB 和 AWS 凭据分别见 [CliMedia.md](CliMedia.md)、
[CliBackgroundTasks.md](CliBackgroundTasks.md)、[SmbClient.md](SmbClient.md)、
[S3CredentialProvider.md](S3CredentialProvider.md)。
测试入口：`ctest --test-dir build-clang --output-on-failure`；
`cli` 覆盖语法、引号、只读旧数据库、隐私、忙超时，以及中文表格、长值、
批量明细、窄窗口和 JSON 输出兼容；
`cli_integration` 使用本地测试端点覆盖服务/递归传输/条件 PUT/取消、
三种 TSSL 备份目标和实际 FFmpeg 打包还原/部分失败。
GitHub Actions 构建、安装、便携与原生包均包含控制台入口，安装后执行版本和
JSON 冒烟检查。macOS/Linux 由现有矩阵验证，不宣称已在 Windows 上运行它们。

实现依据：[Qt SQLite 驱动](https://doc.qt.io/qt-6/sql-driver.html)、
[HTTP If-None-Match](https://www.rfc-editor.org/rfc/rfc9110.html#section-13.1.2)、
[WebDAV MKCOL](https://www.rfc-editor.org/rfc/rfc4918.html#section-9.3)。
