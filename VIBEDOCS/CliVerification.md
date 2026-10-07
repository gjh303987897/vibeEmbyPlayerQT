# CLI 本地验收记录

## 扩展版：2026-10-06

Windows 11 x64、Qt 6.7.3、clang-cl / MSVC、Ninja、Debug，FFmpeg 9.0.2。
全部目标编译成功，最终 `ctest --test-dir build-clang --output-on-failure --parallel 4`
为 **25/25 通过**（31.41 秒），包含原有 23 项。

| 新增范围 | 实际验证 |
|---|---|
| 媒体与 IPTV | Emby/Jellyfin 库、目录、搜索、详情；登录/退出；HTTP 播放列表及重命名后相对地址/来源保留 |
| 播放 | 实际 libmpv 本地/Emby/Jellyfin/WebDAV 播放；headless --wait、播放失败、控制命令、停止报告；隐私访问与宿主运行中 PIN 变更 |
| WebDAV 操作 | 递归建目录、COPY/MOVE/重命名/DELETE、根目录保护、删除确认、覆盖失败保留旧文件 |
| 后台与恢复 | 提交、暂停、退出、重启、interrupted、恢复、Range/If-Range；错误 Content-Range 拒绝发布、修复后 retry；完成任务拒绝取消 |
| PIN/配置/Shell | PIN 创建/更改/移除、范围检查、字幕路径检查；变量/替换/结果管道、历史、补全候选、脚本累计失败和 fail-fast |
| TSSL/视频 | 选定摘要导出去重、批量删除/部分失败、全部导出与本地备份取消；目录递归及远程两种容器还原 |
| AWS | profile 临时凭据；SessionToken 参与 S3 签名；STS 源 Token、表单编码、响应解析及取消 |
| SMB 核心 | Windows 虚拟根、越界/设备名/非法片段拒绝、摘要取消；已编译原生认证/文件操作及后台接口，尚未连接真实 NAS |
| Windows 自动化 | PowerShell 捕获 JSON 时独立宿主仍存活，命令正常结束；原生 Ctrl+C 包含加密播放准备取消 |

安装目录 `build-clang/cli-install-check` 已更新，包含 .com、桌面 .exe 和
vibePlayerPlayback.exe。移除 Qt 开发目录 PATH 后，CMD/PowerShell 同名入口、
UTF-8 JSON 重定向、中文带空格路径、实际 libmpv 播放和后台宿主均通过；
参数错误返回 2。安装冒烟记录为
`build-clang/cli-expansion-installed-smoke.json`，全量结果为
`build-clang/cli-expansion-final-tests.txt`。
PE 检查确认 .com 的 Qt 直接依赖仍只有 Core/Network/Sql/Xml，
播放宿主单独加载 Qt Gui 和 libmpv。

验收中修复了 PIN 缓存、在线 IPTV 重命名后丢失来源、重定向句柄继承、
宿主首包/准备时序、错误续传响应等问题。测试配置、PIN、文件及凭据均隔离，
临时诊断已移除。网络测试仍使用本地模拟端点；真实 NAS、云账户、
EC2 IAM 和 STS 角色权限未实机验收。此次没有运行远程 GitHub Actions，
也没有执行 macOS/Linux 构建。

后台 SMB 按完整文件检查点恢复，WebDAV 上传当前文件从头 PUT；
FFmpeg/TSSL retry 重新执行业务命令，暂停仅适用于传输阶段。
CLI 播放和任务宿主独立于桌面会话。详细边界见
[CLI.md](CLI.md)、[CliBackgroundTasks.md](CliBackgroundTasks.md)、
[SmbClient.md](SmbClient.md)、[S3CredentialProvider.md](S3CredentialProvider.md)。

## 首版：2026-10-05

日期：2026-10-05。环境：Windows 11 x64、Qt 6.7.3、clang-cl / MSVC、
Ninja、Debug 构建；视频测试使用 FFmpeg 9.0.2。

## 构建与回归

- 全部目标编译成功；`ctest --test-dir build-clang --output-on-failure --parallel 4`
  最终结果 **25/25 通过**，包括原有 23 项及新增 `cli`、`cli_integration`。
- 新增测试使用临时 SQLite / INI / TSSL / IPTV 目录；显式指定 QSettings 格式，
  禁用 INI 的系统配置回退，并检查原生配置在测试前后保持一致。
- 新增 Windows 凭据测试只使用随机本地测试端点生成的服务 ID，结束后清理
  该测试凭据，不使用真实账户。

输出格式改进后再次运行全部测试，仍为 **25/25 通过**。`cli` 增加 4 个输出
测试方法；另使用真实控制台和 CMD/PowerShell 重定向检查实际中文列表、
分组帮助与退出码，并在不含 Qt 开发路径的安装目录中验证 JSON。

## 已验证场景

| 范围 | 验收结果 |
|---|---|
| 入口 | CMD、PowerShell 的 `.\vibePlayerQT` 优先运行 .com；.exe 保留桌面入口 |
| 输出 | UTF-8、中文列对齐、完整 ID/长路径续行、批量失败明细、窄窗口、控制字符处理、JSON 重定向与原始类型、标准错误分离 |
| 交互 | 真实终端输入、隐藏 PIN 输入无回显、会话解锁/锁定、带引号的配置路径、退出 |
| 数据库 | 无数据库的 list 返回空列表、不创建数据库；旧库只读不迁移；锁竞争有界等待 |
| 服务 | ID/唯一名称、重名拒绝、字段更新、普通/隐私卡片、错误 PIN、凭据缺失、会话清理 |
| 登录 | Emby / Jellyfin 登录及已有会话检查；WebDAV 验证与 Windows 安全凭据保存/移除 |
| IPTV | 中文本地播放列表导入、无台标频道保存、托管副本、检查与重命名 |
| WebDAV | ls/stat/MKCOL、缺失父目录、中文递归上传/下载、层级与 URL 编码 |
| 冲突 | 默认拒绝同名、显式覆盖上传、检查后的条件 PUT 竞争返回 412、下载不覆盖 |
| 网络错误 | 503、GET 阶段认证失败 401、stat 后资源消失 404、越界资源拒绝 |
| 取消 | Windows 原生 Ctrl+C 发给独立测试控制台；单命令返回 130、Shell 继续下一条命令 |
| 下载清理 | 未完成的文件不发布到最终路径；独立临时目录清理；保留已完成项 |
| TSSL | 元数据查询不输出密钥；有效/重复/损坏混合导入、导出一个/全部、删除确认 |
| 备份 | 本地、WebDAV、S3 备份/恢复；共用去重导入流程、网络恢复取消 |
| 视频 | 实际 FFmpeg：M3U8S/M3U8SP、两种容器上传 WebDAV、original/mkv/mp4 还原 |
| 视频失败 | FFmpeg 缺失、损坏输入、密钥缺失、部分成功、全部失败、临时覆盖不写持久配置 |
| 桌面共存 | 实际桌面进程运行时 CLI 成功读取相同服务数据库；另有同时打开数据库的测试连接 |

WebDAV、Emby、Jellyfin、S3 网络验收使用本地模拟端点。S3 检查现有 SigV4
请求与对象列表/上传/下载流程，未连接真实云账户。桌面启动检查通过临时应用内
日志确认 Qt 根窗口加载且 `visible=1`；诊断代码已移除。没有修改播放器行为。

新增测试还发现并修复了两处原有问题：无台标 IPTV 的空 QString 被绑定成 SQL
NULL，以及短 FFmpeg 错误诊断触发 Debug 断言，导致失败批次无法结束。

## 安装与运行依赖

工作区安装验收目录：`build-clang/cli-install-check`。
使用绝对安装路径完成 `cmake --install`；Qt 6.7.3 部署脚本不接受相对 prefix。

移除 Qt 开发目录的 PATH 后，安装目录中的 .com 仍能执行版本、空服务列表、
JSON 输出和参数错误检查。控制台二进制的直接 Qt 导入只有 Core / Network /
Sql / Xml，没有 Qt GUI、QML 或 libmpv。无参数运行启动旁边的桌面 .exe。

这是本地 Debug 验收目录；发布构建继续由 GitHub Actions 生成 Release 包。

## 持续集成与平台范围

GitHub Actions 已加入 CLI 构建、测试、安装及版本/JSON 启动检查。
Windows .com、macOS 应用包内 CLI、Linux 便携包与 AppImage 入口均已更新；
Flatpak 可通过 `flatpak run --command=vibePlayerQT-cli io.github.gjh303987897.vibeEmbyPlayerQT list`
指定控制台入口。

此次未触发远程 Actions，也未在本机执行 macOS/Linux 构建。它们由现有矩阵
验证；核心代码使用 Qt，Windows 控制台输入、信号、桌面显示参数单独封装。

命令说明和自动化示例见 [CLI.md](CLI.md)，共享接口见 [SharedWorkflows.md](SharedWorkflows.md)。

中断依据：[Microsoft 控制台事件](https://learn.microsoft.com/en-us/windows/console/generateconsolectrlevent)；
入口显示参数使用 [QProcess 的 Windows 启动参数修改接口](https://doc.qt.io/qt-6/qprocess.html#setCreateProcessArgumentsModifier)。
