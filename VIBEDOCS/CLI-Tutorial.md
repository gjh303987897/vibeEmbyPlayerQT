# vibePlayerQT CLI 完整使用教程

本教程面向 Windows 的 PowerShell、CMD 和自动化脚本，覆盖当前已实现的 CLI 命令。命令定义以 `src/cli/CliCommand.cpp` 为准；安装后可用 `help` 查看当前版本的帮助。

[返回中文 README](../README.zh-CN.md) · [English README](../README.md) · [CLI 实现与接口说明](CLI.md)

目录与 README 直达链接使用 [GitHub 支持的相对链接和自定义锚点](https://docs.github.com/en/get-started/writing-on-github/getting-started-with-writing-and-formatting-on-github/basic-writing-and-formatting-syntax#custom-anchors)。

## 目录

- [1. 快速开始](#quick-start)
- [2. 命令、路径和凭据约定](#conventions)
- [3. 服务卡片管理](#services)
- [4. 隐私服务与 PIN](#privacy)
- [5. WebDAV 文件操作](#webdav)
- [6. SMB 文件操作](#smb)
- [7. TSSL 密钥包管理](#tssl)
- [8. TSSL 备份与恢复：本地、WebDAV、S3](#backup)
- [9. 视频加密打包与解密还原](#video)
- [10. 媒体库与 IPTV 查询](#media)
- [11. 播放和播放器控制](#player)
- [12. 后台任务与重启恢复](#tasks)
- [13. 配置查询与全部配置键](#config)
- [14. 持续交互终端 Shell](#shell)
- [15. JSON、退出码与自动化脚本](#automation)
- [16. 常见问题与当前边界](#troubleshooting)

<a name="quick-start"></a>

## 1. 快速开始

### 1.1 找到程序目录

使用解压或安装后的发行包，进入同时包含 `vibePlayerQT.com` 和 `vibePlayerQT.exe` 的目录；当前安装布局通常是 `bin`。运行发行包不需要安装 VS、LLVM、CMake 或 Ninja。

Windows 的 `.com` 是控制台入口，`.exe` 是桌面入口。CMD 和 PowerShell 在默认 `PATHEXT` 下优先匹配 `.COM`，因此可以省略扩展名。如果系统修改过扩展名顺序，请显式使用 `.\vibePlayerQT.com`。

以下路径仅为示例，请换成实际安装位置：

```powershell
Set-Location "D:\Apps\vibePlayerQT\bin"
.\vibePlayerQT --version
.\vibePlayerQT help
.\vibePlayerQT list
.\vibePlayerQT shell
```

CMD 的切换目录方式：

```bat
cd /d "D:\Apps\vibePlayerQT\bin"
vibePlayerQT.com --version
vibePlayerQT.com list
```

无参数运行 `.\vibePlayerQT` 会打开桌面应用；直接运行 `.\vibePlayerQT.exe` 也会打开桌面应用。查询和文件任务不会打开桌面窗口；`play` 会启动独立播放窗口。

本仓库当前已部署的本地验收版本位于：

```powershell
Set-Location "D:\workspace\vibeEmbyPlayerQT\build-clang\cli-install-check\bin"
.\vibePlayerQT.com list
```

### 1.2 第一次添加并使用 WebDAV

```powershell
# 添加服务卡片；URL 包含要作为根目录使用的路径
.\vibePlayerQT service add --type webdav --name "家庭 DAV" --url "https://dav.example.com/dav/" --username "alice"

# 交互终端会提示输入密码，输入内容不回显
.\vibePlayerQT service login "家庭 DAV"

# 查看目录，并创建上传目标的父目录
.\vibePlayerQT webdav ls "家庭 DAV" /
.\vibePlayerQT webdav mkdir "家庭 DAV" "/电影" --parents

# 上传一个文件
.\vibePlayerQT webdav upload "家庭 DAV" "D:\视频\影片.mp4" "/电影/影片.mp4"
```

将地址、用户名和本地文件替换为自己的值。如果桌面端已经保存服务，可以直接从 `list` 开始。

### 1.3 查看分组或单个命令帮助

```powershell
.\vibePlayerQT help service
.\vibePlayerQT help webdav download
.\vibePlayerQT help video pack
.\vibePlayerQT help tssl backup
.\vibePlayerQT --help
```

<a name="conventions"></a>

## 2. 命令、路径和凭据约定

### 2.1 命令写法

本教程的命令表省略 `.\vibePlayerQT` 前缀：

```text
.\vibePlayerQT <命令分组> <操作> <位置参数> [--选项 值] [--开关]
```

- `SERVICE`：服务的完整 ID 或唯一名称。重复名称会报参数错误，必须使用 `list` 返回的完整 ID。
- `DIGEST`：`tssl list` 返回的完整 64 位十六进制根摘要，不支持截断摘要。
- `TASK_ID`、`ITEM_ID`、`LIBRARY_ID`：对应查询命令返回的 ID，请替换示例中的占位值。
- `FILE...`、`SOURCE...`：允许传多个路径，每个路径独立加引号。
- `[参数]` 表示可选，示例中的尖括号或占位名称不需要原样输入。
- 分组、操作和选项使用教程列出的英文写法；布尔值为 `true` 或 `false`。

### 2.2 路径规则

本地路径相对于启动命令的工作目录解析。包含中文、空格或特殊字符时使用双引号：

```powershell
.\vibePlayerQT video pack "D:\我的视频\影片 01.mp4" --output "D:\加密输出"
```

WebDAV/SMB 远程路径使用 `/`。卡片配置的根目录就是 CLI 的 `/`。例如卡片地址为 `https://dav.example.com/dav/media/`，CLI 的 `/电影/a.mp4` 对应 `/dav/media/电影/a.mp4`。

远程路径中的中文、空格、`#` 和 `%` 直接写原始字符，不要手动做 URL 编码。`..` 不能越过服务根目录。

上传、下载、复制、移动的目标均表示**最终文件或文件夹路径**：

```powershell
# 合集文件夹的内容保存到 /电影/合集，不再自动追加一层“合集”
.\vibePlayerQT webdav upload "家庭 DAV" "D:\视频\合集" "/电影/合集"

# 本地父目录 D:\下载 必须先存在
.\vibePlayerQT webdav download "家庭 DAV" "/电影/合集" "D:\下载\合集"
```

### 2.3 密码与环境变量

不提供明文 `--password`、`--pin` 或 S3 secret 参数。真实交互终端在缺少凭据时会隐藏输入；脚本、重定向和管道执行必须已有保存凭据，或使用环境变量。

`--password-env` 等参数接收的是**环境变量名称**，而不是密码：

```powershell
# DAV_PASSWORD 应由运行环境或秘密管理工具预先注入
.\vibePlayerQT service login "家庭 DAV" --password-env DAV_PASSWORD
.\vibePlayerQT webdav ls "家庭 DAV" / --password-env DAV_PASSWORD
```

在本地 PowerShell 中，也可以隐藏输入后设置当前进程的环境变量：

```powershell
$enteredSecret = Read-Host "WebDAV 密码" -AsSecureString
$env:DAV_PASSWORD = [System.Net.NetworkCredential]::new("", $enteredSecret).Password
try {
    .\vibePlayerQT service login "家庭 DAV" --password-env DAV_PASSWORD
} finally {
    Remove-Item Env:DAV_PASSWORD
    $enteredSecret.Dispose()
}
```

这段示例不将密码字面值写进命令历史。CI 中使用运行平台提供的 secret 注入。

显式 `--password-env` 覆盖保存密码；不指定时，WebDAV/SMB 优先读取已有安全存储。Emby/Jellyfin 登录需要密码，后续查询复用登录会话。

| 参数 | 用途 |
| --- | --- |
| `--password-env NAME` | 读取服务密码 |
| `--pin-env NAME` | 读取当前隐私 PIN |
| `--new-pin-env NAME` | 创建/更改 PIN 时读取新 PIN |
| `--confirm-pin-env NAME` | 读取新 PIN 的确认值 |
| `--s3-secret-env NAME` | 读取 S3 Secret Access Key |
| `--s3-session-token-env NAME` | 读取 AWS 临时 Session Token |

不要将真实密码写进教程、脚本文件或仓库。CLI 的结果和进度不输出密码、Token 或密钥内容。

### 2.4 共享配置与独立配置

默认与桌面端共用服务、配置、凭据和 TSSL。CLI 的播放与后台任务各自独立执行。

```powershell
.\vibePlayerQT list --data-dir "D:\vibe-cli-profile"
.\vibePlayerQT config list --data-dir "D:\vibe-cli-profile"
.\vibePlayerQT shell --data-dir "D:\vibe-cli-profile"
```

`--data-dir` 隔离 SQLite、TSSL、IPTV 托管文件和 INI 配置。每次独立命令都要传相同目录；后台任务查询和播放控制也要使用相同目录。系统凭据仍按服务 ID 存储，独立目录不隔离系统凭据。

Shell 启动后不能切换数据目录。纯 `list` 查询不会初始化或迁移数据库；首次运行没有数据库时返回空列表。

CLI 可与桌面同时运行。数据库繁忙时最多等待约 3 秒，之后报告错误；此时稍后重试。

### 2.5 覆盖、删除和取消

远程文件操作默认拒绝覆盖。只有支持 `--overwrite` 的命令才能显式覆盖；视频还原另有同名自动编号规则。

删除在真实终端提示确认，输入 `y` 才执行。脚本中必须明确加 `--yes`，否则返回退出码 `2`。`--yes` 不代表允许文件覆盖。

前台任务按 Ctrl+C 会等待取消清理后返回 `130`；交互 Shell 回到提示符。已完成项目保留，取消不会自动删除远程文件。

<a name="services"></a>

## 3. 服务卡片管理

| 命令 | 用途 |
| --- | --- |
| `list` / `service list` | 列出可访问的服务卡片 |
| `service show SERVICE` | 查看配置、保存会话及凭据状态 |
| `service add --type TYPE --name NAME ...` | 添加服务 |
| `service update SERVICE ...` | 只修改指定字段 |
| `service login SERVICE` | 登录，成功后保存会话或凭据 |
| `service check SERVICE` | 检查连接，不修改服务配置 |
| `service logout SERVICE` | 清除保存的认证信息 |
| `service move SERVICE INDEX` | 调整顺序，索引从 `0` 开始 |
| `service remove SERVICE [--yes]` | 删除卡片及关联登录信息 |

`Saved`、`hasSavedSession` 和 `hasSavedCredentials` 表示本地保存状态，不代表服务当前在线。需要实际检查时运行 `service check`。

### 3.1 添加各类服务

```powershell
.\vibePlayerQT service add --type emby --name "家庭 Emby" --url "https://emby.example.com" --username "alice"
.\vibePlayerQT service add --type jellyfin --name "家庭 Jellyfin" --url "https://jellyfin.example.com" --username "alice"
.\vibePlayerQT service add --type webdav --name "家庭 DAV" --url "https://dav.example.com/dav/" --username "alice"
.\vibePlayerQT service add --type smb --name "NAS" --url "smb://nas/videos" --username "DOMAIN\alice"
.\vibePlayerQT service add --type iptv --name "本地电视" --playlist "D:\播放列表\channels.m3u"
.\vibePlayerQT service add --type iptv --name "在线电视" --playlist "https://example.com/channels.m3u8"
```

Emby/Jellyfin/WebDAV 使用 `--url` 和 `--username`；SMB 地址格式为 `smb://主机/共享名/可选根目录`。IPTV 使用 `--playlist`，支持本地 M3U/M3U8 或 HTTP(S) 播放列表，并保存托管副本。

添加和修改还支持：

| 参数 | 说明 |
| --- | --- |
| `--auto-login true\|false` | 保存桌面端自动登录设置 |
| `--trust-self-signed true\|false` | 保存信任自签名证书设置 |
| `--private-mode true\|false` | 仅 `service update` 支持，切换隐私区属性 |

`auto-login` 不会让 CLI 查询自动登录；Emby/Jellyfin 先运行 `service login`。

### 3.2 登录、检查、更新和删除

```powershell
.\vibePlayerQT service login "家庭 Emby" --password-env EMBY_PASSWORD
.\vibePlayerQT service check "家庭 Emby"
.\vibePlayerQT service show "家庭 Emby"

.\vibePlayerQT service update "家庭 Emby" --name "客厅 Emby" --auto-login true
.\vibePlayerQT service move "客厅 Emby" 0

# 更新 IPTV 内容时重新导入播放列表
.\vibePlayerQT service update "在线电视" --playlist "https://example.com/new-channels.m3u8"

.\vibePlayerQT service logout "客厅 Emby"
.\vibePlayerQT service remove "客厅 Emby" --yes
```

修改服务地址或用户名后，重新登录。移除服务卡片保留本地媒体、历史和 TSSL；不会删除服务器上的文件。

<a name="privacy"></a>

## 4. 隐私服务与 PIN

PIN 为 4 至 12 位数字。可在桌面设置，也可由 CLI 创建、更改或移除。

```powershell
# NEW_PIN 和 OLD_PIN 由环境预先提供
.\vibePlayerQT privacy set --new-pin-env NEW_PIN --confirm-pin-env NEW_PIN
.\vibePlayerQT privacy change --pin-env OLD_PIN --new-pin-env NEW_PIN --confirm-pin-env NEW_PIN

# 独立命令访问隐私区
.\vibePlayerQT list --private --pin-env PLAYER_PIN
.\vibePlayerQT service show "私密 DAV" --private --pin-env PLAYER_PIN

# 将现有普通卡片移入隐私区
.\vibePlayerQT service update "家庭 DAV" --private-mode true --private --pin-env PLAYER_PIN

# 在隐私区创建新服务
.\vibePlayerQT service add --type webdav --name "私密 DAV" --url "https://dav.example.com/private/" --username "alice" --private --pin-env PLAYER_PIN
```

将普通卡片移入隐私区，以及修改已经位于隐私区的卡片，都需要 `--private` 和 PIN 验证。默认列表隐藏隐私卡片；解锁后可同时访问普通与隐私卡片。

```powershell
# 将隐私卡片移回普通区
.\vibePlayerQT service update "私密 DAV" --private-mode false --private --pin-env PLAYER_PIN

# 移除 PIN；隐私卡片仍保留，设置新 PIN 后才能访问
.\vibePlayerQT privacy remove --pin-env PLAYER_PIN --yes
```

独立命令需要每次明确请求 `--private` 并验证 PIN，知道服务 ID 也不能绕过。独立运行 `privacy unlock` 不会解锁下一次进程；持续解锁应使用 Shell 中的 `privacy unlock`。

隐私播放的 `player` 控制，以及隐私后台任务的查询/控制，同样需要 `--private --pin-env PLAYER_PIN`。

<a name="webdav"></a>

## 5. WebDAV 文件操作

### 5.1 命令一览

| 命令 | 说明 |
| --- | --- |
| `webdav ls SERVICE [PATH]` | 列出目录，默认 `/` |
| `webdav stat SERVICE PATH` | 查询文件/文件夹信息 |
| `webdav mkdir SERVICE PATH [--parents]` | 新建目录；`--parents` 创建缺失祖先 |
| `webdav upload SERVICE LOCAL REMOTE [--overwrite]` | 上传文件或整个目录 |
| `webdav download SERVICE REMOTE LOCAL [--overwrite] [--resume]` | 下载文件或整个目录 |
| `webdav copy SERVICE SOURCE TARGET [--overwrite]` | 在同一服务内复制 |
| `webdav move SERVICE SOURCE TARGET [--overwrite]` | 在同一服务内移动 |
| `webdav rename SERVICE SOURCE TARGET [--overwrite]` | MOVE 的别名，目标仍是完整路径 |
| `webdav remove SERVICE PATH [--yes]` | 删除文件或目录及子项 |

### 5.2 浏览与新建文件夹

```powershell
.\vibePlayerQT webdav ls "家庭 DAV"
.\vibePlayerQT webdav ls "家庭 DAV" "/电影"
.\vibePlayerQT webdav stat "家庭 DAV" "/电影/影片.mp4"
.\vibePlayerQT webdav mkdir "家庭 DAV" "/电影/新 文件夹"
.\vibePlayerQT webdav mkdir "家庭 DAV" "/归档/2026/电影" --parents
```

不加 `--parents` 时，父目录必须已经存在。`--parents` 可以复用已有目录，祖先是文件时仍报错。

### 5.3 上传、下载、覆盖与续传

```powershell
.\vibePlayerQT webdav upload "家庭 DAV" "D:\视频\影片.mp4" "/电影/影片.mp4"
.\vibePlayerQT webdav upload "家庭 DAV" "D:\视频\合集" "/电影/合集"

# 显式替换同名文件；文件夹上传会合并已有目录
.\vibePlayerQT webdav upload "家庭 DAV" "D:\视频\影片.mp4" "/电影/影片.mp4" --overwrite

.\vibePlayerQT webdav download "家庭 DAV" "/电影/影片.mp4" "D:\下载\影片.mp4"
.\vibePlayerQT webdav download "家庭 DAV" "/电影/合集" "D:\下载\合集"
.\vibePlayerQT webdav download "家庭 DAV" "/电影/影片.mp4" "D:\下载\影片.mp4" --overwrite

# 首次执行时就加 --resume；中断后执行相同命令继续
.\vibePlayerQT webdav download "家庭 DAV" "/电影/长片.mp4" "D:\下载\长片.mp4" --resume
```

普通上传不会自动创建目标祖先，先使用 `mkdir --parents`。下载目标的本地父目录必须存在，目录传输保留内部层级。

默认上传使用条件 PUT，检查之后出现同名文件也不会直接覆盖。下载先写独立临时文件，完成后发布；`--overwrite` 原子替换文件，不会将未完成内容直接写入最终文件。

`--resume` 在目标父目录保存检查点和暂存。续传需要服务器返回一致的强 ETag 并支持 Range；无强 ETag 或服务器返回完整内容时，安全地重新下载当前文件。已存在的文件不一定能续传，应使用同一组服务、源和目标路径，以及首次任务产生的检查点。

### 5.4 复制、移动、重命名与删除

```powershell
.\vibePlayerQT webdav copy "家庭 DAV" "/电影/影片.mp4" "/归档/影片.mp4"
.\vibePlayerQT webdav move "家庭 DAV" "/归档/影片.mp4" "/电影/移动后的影片.mp4"
.\vibePlayerQT webdav rename "家庭 DAV" "/电影/移动后的影片.mp4" "/电影/新名称.mp4"
.\vibePlayerQT webdav remove "家庭 DAV" "/电影/新名称.mp4" --yes
```

COPY/MOVE 需要服务器实现对应 WebDAV 方法。源、目标属于同一卡片；目标父目录应先创建。删除拒绝服务根目录 `/`，目录删除包含其子项。

<a name="smb"></a>

## 6. SMB 文件操作

Windows CLI 使用系统 SMB 后端。先添加 `smb://host/share` 服务并登录：

```powershell
.\vibePlayerQT service add --type smb --name "NAS" --url "smb://nas/videos" --username "DOMAIN\alice"
.\vibePlayerQT service login "NAS" --password-env NAS_PASSWORD
.\vibePlayerQT service check "NAS"
```

| 命令 | 说明 |
| --- | --- |
| `smb ls SERVICE [PATH]` | 列出目录，默认 `/` |
| `smb stat SERVICE PATH` | 查询资源信息 |
| `smb mkdir SERVICE PATH [--parents]` | 新建目录 |
| `smb upload SERVICE LOCAL REMOTE [--overwrite]` | 上传文件/目录 |
| `smb download SERVICE REMOTE LOCAL [--overwrite]` | 下载文件/目录 |
| `smb copy SERVICE SOURCE TARGET [--overwrite]` | 同一服务内复制 |
| `smb move SERVICE SOURCE TARGET [--overwrite]` | 同一服务内移动 |
| `smb remove SERVICE PATH [--yes]` | 删除资源 |

```powershell
.\vibePlayerQT smb ls "NAS" /
.\vibePlayerQT smb stat "NAS" "/电影/影片.mp4"
.\vibePlayerQT smb mkdir "NAS" "/电影/合集" --parents
.\vibePlayerQT smb upload "NAS" "D:\视频\影片.mp4" "/电影/影片.mp4"
.\vibePlayerQT smb download "NAS" "/电影/影片.mp4" "D:\下载\影片.mp4"
.\vibePlayerQT smb copy "NAS" "/电影/影片.mp4" "/电影/副本.mp4"
.\vibePlayerQT smb move "NAS" "/电影/副本.mp4" "/电影/新名称.mp4"
.\vibePlayerQT smb remove "NAS" "/电影/新名称.mp4" --yes
```

`--overwrite` 允许文件替换；`smb move` 不合并或替换目录。拒绝越界路径、设备名、符号链接/junction 和服务根目录删除。

SMB 没有前台 `--resume` 参数；使用 `--background` 可按文件恢复任务，当前未完成文件会重新复制。系统报 `1219` 通常表示同一服务器已有不同凭据的连接，CLI 不会自动断开用户已有连接。

<a name="tssl"></a>

## 7. TSSL 密钥包管理

TSSL 保存解密 M3U8S/M3U8SP 所需密钥和校验元数据。它不是加密后的视频文件。`list` 和 `show` 只显示元数据；`export` 产生包含密钥的完整文件。

| 命令 | 说明 |
| --- | --- |
| `tssl list` | 列出本地密钥包 |
| `tssl show DIGEST` | 查看版本、容器、资源数量和原文件名等元数据 |
| `tssl import FILE...` | 批量导入 `.tssl` 或 ZIP 备份，报告成功、重复和失败数量 |
| `tssl export DIGEST FILE` | 导出一个包到指定文件，已有目标报冲突 |
| `tssl export DIGEST... --output DIR` | 导出选定多个包 |
| `tssl export --all --output DIR` | 导出全部包 |
| `tssl remove DIGEST... [--yes]` | 批量删除指定密钥包 |

```powershell
.\vibePlayerQT tssl list
.\vibePlayerQT tssl show DIGEST
.\vibePlayerQT tssl import "D:\密钥\a.tssl" "D:\密钥\b.tssl"
.\vibePlayerQT tssl import "D:\密钥备份\tssl-backup-快照.zip"
.\vibePlayerQT tssl export DIGEST "D:\密钥导出\影片.tssl"
.\vibePlayerQT tssl export DIGEST_A DIGEST_B --output "D:\密钥导出"
.\vibePlayerQT tssl export --all --output "D:\密钥导出"
.\vibePlayerQT tssl remove DIGEST_A DIGEST_B --yes
```

将 `DIGEST` 等替换为 `tssl list` 返回的完整摘要。导入重复包不会创建第二份。单包导出的目标父目录应已存在；批量导出会创建输出目录，文件使用 `摘要.tssl` 命名。导出全部/选定包和备份沿用存储模块的写入规则，不要依赖它们保护已有同摘要备份。

删除 TSSL 不会删除视频包，但缺少对应密钥将无法正常解密该视频。需要在别的机器还原时，传递加密视频包，并导出/导入对应 TSSL。

**当前没有 `tssl encrypt` / `tssl decrypt`。导出的 `.tssl` 是含 Base64 密钥的 JSON，Base64 不等于加密，也没有外层密码保护。** 视频加密和解密使用 [视频章节](#video) 的 `video` 命令。

<a name="backup"></a>

## 8. TSSL 备份与恢复：本地、WebDAV、S3

`tssl backup` 备份当前密钥包；`tssl restore` 找到备份并导入当前密钥库。它们只处理 TSSL，不备份或还原视频内容。

不传目标参数时使用 `tsslBackup/*` 保存设置。默认目标 `none` 尚未启用备份，需要配置或传 `--target`。临时参数不会修改保存配置。

备份默认生成标准 ZIP：先在本地工作线程整理、压缩全部 TSSL，再向目标上传一个压缩包，减少大量小文件的网络请求。本地目标也使用相同 ZIP 格式。压缩包保留原 `.tssl` 内容，并附带版本及完整性校验清单；**压缩不会给密钥增加密码保护**。

可使用 `tssl backup --format files` 继续逐个备份 `.tssl`，或者通过 `config set tsslBackup/format files` 保存旧模式。`--format zip` 可临时切回压缩包，不改变保存设置。恢复不需要也不接受 `--format`，会自动识别新旧备份。

兼容规则：新版恢复同时支持 ZIP 和旧的散装 `.tssl`；同一个目录有两种格式或多个快照时全部处理，按根摘要跳过重复项。旧版不会自动识别 ZIP，可先手动解压，再指向解压后直接包含 `.tssl` 的目录恢复，也可让新版使用 `files` 模式生成旧备份。ZIP 内文件平铺在根目录，旧版恢复会忽略 `manifest.json`，继续读取 `.tssl`。每次 ZIP 备份使用独立快照名称，保留旧备份，不自动清理旧散装文件。

ZIP 快照命名为 `tssl-backup-UTC时间-UUID.zip`。目录恢复只自动发现符合备份命名规则的 ZIP，避免把无关压缩包当作备份；改名后的合法 ZIP 可用 `tssl import FILE.zip` 直接导入。每个备份压缩包及其解压总量均上限 2 GiB，最多包含 10000 个 TSSL、每包最大 64 MiB；详细格式与校验规则见 [压缩包模块文档](TsslBackupArchive.md)。

### 8.1 本地目录

```powershell
.\vibePlayerQT tssl backup --target local --local-path "D:\备份\TSSL"
# 为旧客户端生成独立 .tssl 文件
.\vibePlayerQT tssl backup --target local --local-path "D:\备份\TSSL" --format files
.\vibePlayerQT tssl restore --target local --local-path "D:\备份\TSSL"
```

本地备份目录可以自动创建。本地恢复读取该目录直接包含的受支持 ZIP 备份和 `*.tssl`，不会递归扫描全部子目录。解压到临时目录并校验后才导入，结束或取消后清理临时文件，已成功导入的包保留。

保存默认目标后可以简写：

```powershell
.\vibePlayerQT config set tsslBackup/target local
.\vibePlayerQT config set tsslBackup/localPath "D:\备份\TSSL"
.\vibePlayerQT config set tsslBackup/format zip
.\vibePlayerQT tssl backup
.\vibePlayerQT tssl restore
```

### 8.2 WebDAV

```powershell
# 备份目录先创建
.\vibePlayerQT webdav mkdir "家庭 DAV" "/备份/TSSL" --parents
.\vibePlayerQT tssl backup --target webdav --service "家庭 DAV" --path "/备份/TSSL"
.\vibePlayerQT tssl restore --target webdav --service "家庭 DAV" --path "/备份/TSSL"
```

需要密码时追加 `--password-env DAV_PASSWORD`；隐私服务追加 `--private --pin-env PLAYER_PIN`。恢复按摘要去重，下载暂存会在导入完成或取消后清理。

### 8.3 S3 / S3 兼容存储

静态凭据示例：

```powershell
# S3_SECRET 已由环境提供；ACCESS_KEY 替换为实际 Access Key ID
.\vibePlayerQT tssl backup --target s3 --s3-endpoint "https://s3.example.com" --s3-bucket "media" --s3-region "us-east-1" --s3-prefix "tssl" --s3-access-key ACCESS_KEY --s3-secret-env S3_SECRET
.\vibePlayerQT tssl restore --target s3 --s3-endpoint "https://s3.example.com" --s3-bucket "media" --s3-region "us-east-1" --s3-prefix "tssl" --s3-access-key ACCESS_KEY --s3-secret-env S3_SECRET
```

| 参数 | 说明 |
| --- | --- |
| `--target local\|webdav\|s3` | 临时目标类型 |
| `--format zip\|files` | 仅 backup：临时压缩包或旧独立文件模式；默认保存值 `zip` |
| `--local-path DIR` | 本地备份/恢复目录 |
| `--service SERVICE --path PATH` | WebDAV 服务及备份目录 |
| `--s3-endpoint URL` | S3 服务端点 |
| `--s3-bucket NAME` | 桶名 |
| `--s3-region NAME` | 签名区域 |
| `--s3-prefix PATH` | 对象前缀 |
| `--s3-access-key KEY` | Access Key ID |
| `--s3-secret-env NAME` | Secret Access Key 所在变量名 |
| `--s3-session-token-env NAME` | 临时 Session Token 所在变量名 |
| `--s3-credentials env\|profile\|instance` | 显式选择 AWS 凭据来源 |
| `--s3-profile NAME` | `profile` 来源的配置名 |
| `--s3-role-arn ARN` | 使用来源凭据执行 STS AssumeRole |
| `--s3-role-session-name NAME` | 角色会话名，默认 `vibePlayerQT-cli` |
| `--s3-external-id ID` | AssumeRole 的 ExternalId |

S3 使用 HTTPS；仅 `localhost` 开发端点允许 HTTP。服务端点、桶和区域应填写实际存储提供方给出的值。

命令会显示整理、压缩、上传或下载、解压和导入的阶段进度；这些信息写入标准错误，`--json` 的标准输出仍只包含一个结果对象。`backedUp` 统计 TSSL 包数量，ZIP 模式上传文件数通常为 1。恢复的 `imported`、`duplicates`、`failed` 统计导入结果，可用 Ctrl+C 取消。

AWS 环境变量来源读取 `AWS_ACCESS_KEY_ID`、`AWS_SECRET_ACCESS_KEY`，并读取可选 `AWS_SESSION_TOKEN`：

```powershell
.\vibePlayerQT tssl backup --target s3 --s3-endpoint "https://s3.us-east-1.amazonaws.com" --s3-bucket "my-media-backup" --s3-region "us-east-1" --s3-credentials env

# 读取 ~/.aws/credentials 中的 default 配置
.\vibePlayerQT tssl backup --target s3 --s3-endpoint "https://s3.us-east-1.amazonaws.com" --s3-bucket "my-media-backup" --s3-region "us-east-1" --s3-credentials profile --s3-profile default

# 由已有配置取得角色临时凭据
.\vibePlayerQT tssl backup --target s3 --s3-endpoint "https://s3.us-east-1.amazonaws.com" --s3-bucket "my-media-backup" --s3-region "us-east-1" --s3-credentials profile --s3-profile default --s3-role-arn "arn:aws:iam::123456789012:role/Backup"
```

`profile` 支持 `AWS_SHARED_CREDENTIALS_FILE` 和 `AWS_PROFILE`。`instance` 只在明确选择时请求 EC2 IMDSv2。来源参数提供凭据，端点/桶/区域仍需设置。将上述 `backup` 换成 `restore` 即可恢复。

当前不会自动执行 SSO、MFA、`credential_process` 或 AWS 配置中的角色链，也不会在长任务中途刷新临时令牌。详见 [S3 凭据模块](S3CredentialProvider.md)。

<a name="video"></a>

## 9. 视频加密打包与解密还原

### 9.1 准备 FFmpeg

打包与还原需要外部 FFmpeg。程序没有捆绑 FFmpeg；当前数字版本最低为 `5.0`，具体编码器和封装能力也要满足所选参数。

```powershell
.\vibePlayerQT config set m3u8s/ffmpegExecutable "D:\Tools\ffmpeg\bin\ffmpeg.exe"
.\vibePlayerQT config get m3u8s/ffmpegExecutable
```

也可将 FFmpeg 加入 PATH，或通过 `--ffmpeg FILE` 临时指定。

### 9.2 加密到本地

```powershell
# 加密一个视频，输出单文件 M3U8SP
.\vibePlayerQT video pack "D:\视频\影片.mp4" --output "D:\加密输出" --container m3u8sp

# 批量输入文件和文件夹，递归发现视频并保留来源层级
.\vibePlayerQT video pack "D:\视频\合集" "D:\视频\另一部.mkv" --output "D:\加密输出" --jobs 2

# 视频/音频复制模式，避免主动重新编码（仍受源格式和容器兼容性限制）
.\vibePlayerQT video pack "D:\视频\影片.mp4" --output "D:\加密输出" --video copy --audio copy

# 目录式 M3U8S
.\vibePlayerQT video pack "D:\视频\影片.mp4" --output "D:\加密输出" --container m3u8s

# 查看对应密钥包，并另行导出或备份
.\vibePlayerQT tssl list
.\vibePlayerQT tssl backup --target local --local-path "D:\密钥备份"
```

实际输出路径和根摘要见结果中的 `items`。包名基于摘要生成，例如 `m3u8s_摘要.m3u8sp`，不会直接使用原视频文件名。成功打包会将对应密钥存入当前 TSSL 库。

- `m3u8sp`：索引式单文件容器，传输整个 `.m3u8sp` 文件。
- `m3u8s`：清单和加密分段组成目录，传输时必须保留整个打包目录，单独复制清单不能还原视频。

### 9.3 加密后上传 WebDAV

```powershell
.\vibePlayerQT video pack "D:\视频\合集" --webdav "家庭 DAV" --path "/加密视频" --container m3u8sp --jobs 2 --keep-local
```

`--output` 与 `--webdav` 互斥。WebDAV 模式可以准备输出目录祖先，先本地打包再上传。`--keep-local` 使上传成功后仍保留本地结果；上传失败或取消也会保留结果，返回 `localRoot`。

### 9.4 解密还原普通视频

当前 TSSL 库必须有匹配密钥；在新机器或新 profile 上先导入。下面的包文件名是示例，使用时应替换为打包结果中的真实路径：

```powershell
.\vibePlayerQT tssl import "D:\密钥\影片.tssl"
.\vibePlayerQT video restore "D:\加密输出\影片.m3u8sp" --output "D:\还原视频" --format original
.\vibePlayerQT video restore "D:\加密输出\合集" --output "D:\还原视频" --format mkv
.\vibePlayerQT video restore "D:\加密输出\a.m3u8sp" "D:\加密输出\b.m3u8sp" --output "D:\还原视频" --format mp4

# 从 WebDAV 下载包到临时目录，再解密还原到本地
.\vibePlayerQT video restore "/加密视频/影片.m3u8sp" --service "家庭 DAV" --output "D:\还原视频" --format mkv
.\vibePlayerQT video restore "/加密视频/合集" --service "家庭 DAV" --output "D:\还原视频"
```

`--output` 或保存的还原输出目录必须有一个已配置。本地、远程目录均递归发现 M3U8S/M3U8SP。远程指定 `.m3u8s` 时会下载其父目录中的分段，因此应将一个包的资源放在独立目录。

`original` 使用保存的原名称/扩展名规则；`mkv`、`mp4` 选择输出封装。还原是解密并重封装打包时留下的音视频，**不能撤销有损编码，也不能恢复打包时已丢弃的音轨或字幕，更不保证与原文件逐字节一致**。目前打包保留第一路视频和音频，不保留字幕。还原同名输出使用自动编号，避免直接覆盖。

### 9.5 视频参数全表

临时参数只影响当前命令。未提供时继承保存配置。

| 参数 | 命令 | 值及作用 |
| --- | --- | --- |
| `--output DIR` | pack / restore | 本地输出目录 |
| `--webdav SERVICE --path PATH` | pack | WebDAV 上传目的地 |
| `--service SERVICE` | restore | 输入来自该 WebDAV 服务 |
| `--container m3u8s\|m3u8sp` | pack | 加密包容器，默认 `m3u8sp` |
| `--video copy\|h264\|h265\|auto` | pack | 视频编码，默认 `h264` |
| `--audio copy\|aac` | pack | 音频编码，默认 `aac` |
| `--quality high\|balanced\|compact` | pack | 编码质量，默认 `balanced` |
| `--jobs N` | pack | 并行数，当前上限最多 `4`，以设备允许值为准 |
| `--auto-codecs h264,h265` | pack | auto 模式允许复制的编码列表 |
| `--auto-target h264\|h265` | pack | auto 模式回退编码，默认 `h264` |
| `--keep-local` | pack | WebDAV 上传成功后保留本地包 |
| `--format original\|mkv\|mp4` | restore | 还原容器，默认 `original` |
| `--ffmpeg FILE` | pack / restore | FFmpeg 可执行文件 |
| `--temp DIR` | pack / restore | 临时目录 |

`--keep-local` 只能请求保留；已保存的 `keepSuccessfulLocal=true` 也会生效。需要持久调整默认值时使用 `config set`。

### 9.6 完整流程：打包、导出密钥、在独立配置中还原

下面示例自动读取真实包名和摘要，避免手动猜测输出文件名。先替换源视频和 FFmpeg 路径；密钥导出目录使用新的文件名，已有同摘要文件会报冲突。

```powershell
$cliFile = (Resolve-Path ".\vibePlayerQT.com").Path
$ffmpegFile = "D:\Tools\ffmpeg\bin\ffmpeg.exe"
$sourceFile = "D:\视频\影片.mp4"
$packDirectory = "D:\加密输出"
$keyDirectory = "D:\密钥导出"
$restoreDirectory = "D:\还原视频"
$restoreProfile = "D:\vibe-restore-profile"

$packText = & $cliFile video pack $sourceFile --output $packDirectory --container m3u8sp --ffmpeg $ffmpegFile --json
$packExit = $LASTEXITCODE
if ($packExit -ne 0) { throw "打包失败，退出码：$packExit" }
$packResult = ($packText -join [Environment]::NewLine) | ConvertFrom-Json
$packagePath = $packResult.data.items[0].output
$rootDigest = $packResult.data.items[0].digest

New-Item -ItemType Directory -Path $keyDirectory -Force | Out-Null
$keyFile = Join-Path $keyDirectory ($rootDigest + ".tssl")
& $cliFile tssl export $rootDigest $keyFile
if ($LASTEXITCODE -ne 0) { throw "导出密钥失败" }

& $cliFile tssl import $keyFile --data-dir $restoreProfile
if ($LASTEXITCODE -ne 0) { throw "导入密钥失败" }
& $cliFile video restore $packagePath --output $restoreDirectory --format mkv --ffmpeg $ffmpegFile --data-dir $restoreProfile
if ($LASTEXITCODE -ne 0) { throw "视频还原失败" }
```

<a name="media"></a>

## 10. 媒体库与 IPTV 查询

| 命令 | 用途 |
| --- | --- |
| `media libraries SERVICE` | Emby/Jellyfin 媒体库列表 |
| `media ls SERVICE [PARENT_ID] [--offset N] [--limit N]` | 无父 ID 时显示媒体库；给出 ID 时列出内容 |
| `media search SERVICE QUERY [--offset N] [--limit N]` | 搜索 Emby/Jellyfin 视频 |
| `media show SERVICE ITEM_ID` | 查看条目、时长和续播位置等信息 |
| `media channels SERVICE` | IPTV 频道 ID、名称和分组 |

```powershell
.\vibePlayerQT service login "家庭 Emby" --password-env EMBY_PASSWORD
.\vibePlayerQT media libraries "家庭 Emby"
.\vibePlayerQT media ls "家庭 Emby" LIBRARY_ID --offset 0 --limit 50
.\vibePlayerQT media ls "家庭 Emby" LIBRARY_ID --offset 50 --limit 50
.\vibePlayerQT media search "家庭 Emby" "影片名称" --limit 20
.\vibePlayerQT media show "家庭 Emby" ITEM_ID
.\vibePlayerQT media channels "本地电视"
```

Jellyfin 用法相同。`offset` 默认为 `0`，必须非负；`limit` 默认 `100`，范围 `1..1000`。IPTV 也可用 `media ls SERVICE` 查看频道；WebDAV/SMB 浏览请使用各自文件命令。

<a name="player"></a>

## 11. 播放和播放器控制

### 11.1 播放来源

```powershell
.\vibePlayerQT play "D:\视频\影片.mp4" --start 30
.\vibePlayerQT play "https://example.com/video.mp4"
.\vibePlayerQT play "家庭 Emby" ITEM_ID
.\vibePlayerQT play "家庭 DAV" "/电影/影片.mp4"
.\vibePlayerQT play "家庭 DAV" "/加密视频/影片.m3u8sp"
.\vibePlayerQT play "NAS" "/电影/影片.mp4"
.\vibePlayerQT play "本地电视" CHANNEL_ID
```

IPTV 支持完整频道 ID 或唯一频道名称，重复名称需要 ID。加密包播放需要当前库中的 TSSL。

`play` 默认在提交播放请求后返回，实际播放由独立 CLI 播放宿主管理。需要等到播放结束并取得最终失败状态时，加 `--wait`：

```powershell
.\vibePlayerQT play "D:\视频\影片.mp4" --wait
.\vibePlayerQT play "D:\视频\影片.mp4" --headless --wait --json
```

`--start` 为非负起始秒数。Emby/Jellyfin 未指定时沿用已有续播位置。`--headless` 关闭音视频输出，适合自动验证；`--wait` 中 Ctrl+C 会停止播放并返回 `130`。

### 11.2 控制命令

| 命令 | 参数/行为 |
| --- | --- |
| `player status` | 查看状态、位置和轨道 |
| `player pause` | 暂停 |
| `player resume` | 继续 |
| `player stop` | 停止播放并结束播放宿主 |
| `player seek SECONDS` | 跳到绝对秒数，非负 |
| `player volume N` | 音量整数 `0..100` |
| `player speed N` | 倍速 `0.5..5` |
| `player subtitle ROW_OR_FILE` | 轨道行号、`-1` 关闭字幕，或本地字幕文件 |
| `player audio ROW` | 切换到状态中返回的音轨行号 |

```powershell
.\vibePlayerQT player status
.\vibePlayerQT player pause
.\vibePlayerQT player seek 120
.\vibePlayerQT player volume 75
.\vibePlayerQT player speed 1.25
.\vibePlayerQT player subtitle "D:\字幕\影片.srt"
.\vibePlayerQT player subtitle -- -1
.\vibePlayerQT player audio 1
.\vibePlayerQT player resume
.\vibePlayerQT player stop
```

`--` 表示后续内容作为位置参数，使 `-1` 不被当成选项。轨道行号从 `player status` 读取，不要猜测编号。

这些命令控制同一 profile 的 CLI 播放宿主，不控制已打开的桌面播放器。关闭播放窗口也会停止播放。

<a name="tasks"></a>

## 12. 后台任务与重启恢复

### 12.1 提交后台任务

只有下列业务命令支持 `--background`：

- `webdav upload` / `webdav download`
- `smb upload` / `smb download`
- `video pack` / `video restore`
- `tssl backup` / `tssl restore`

```powershell
.\vibePlayerQT webdav download "家庭 DAV" "/电影/合集" "D:\下载\合集" --background
.\vibePlayerQT video pack "D:\视频\合集" --output "D:\加密输出" --jobs 2 --background
.\vibePlayerQT tssl backup --target local --local-path "D:\备份\TSSL" --background
.\vibePlayerQT task list
```

提交成功返回任务 ID 和初始状态，表示已入队；是否完成要查看 `task show`。宿主自动启动，退出当前终端不会取消已提交任务。一次运行一个顶层任务，视频内部仍可并行。

| 状态 | 含义 |
| --- | --- |
| `queued` | 等待执行 |
| `running` | 正在执行 |
| `paused` | 已暂停 |
| `interrupted` | 宿主退出导致中断，需要显式恢复 |
| `completed` | 已成功完成 |
| `failed` | 执行失败，查看结果和错误 |
| `canceled` | 已取消 |

### 12.2 查询、暂停、恢复和删除记录

| 命令 | 用途 |
| --- | --- |
| `task list` | 查看当前 profile 的后台任务 |
| `task show TASK_ID` | 详细状态、结果及错误 |
| `task pause TASK_ID` | 暂停排队任务或可暂停的传输阶段 |
| `task resume TASK_ID` | 继续暂停任务，或将中断任务重新入队 |
| `task retry TASK_ID` | 重新排队可重试任务 |
| `task cancel TASK_ID` | 取消未完成任务 |
| `task remove TASK_ID [--yes]` | 清理记录、保存凭据和暂存，保留最终输出 |
| `daemon run` | 在当前终端显式运行任务宿主 |
| `daemon stop` | 停止当前 profile 的任务宿主 |

```powershell
.\vibePlayerQT task show TASK_ID
.\vibePlayerQT task pause TASK_ID
.\vibePlayerQT task resume TASK_ID
.\vibePlayerQT task retry TASK_ID
.\vibePlayerQT task cancel TASK_ID
.\vibePlayerQT task remove TASK_ID --yes
```

以上展示各命令写法，按任务状态选择操作。运行或排队任务不能直接删除记录；先取消或等待完成。已完成任务拒绝取消。

暂停支持 WebDAV/SMB 传输阶段；扫描规划、FFmpeg 和 TSSL 阶段会报告无法暂停。取消使用现有模块的取消流程。

### 12.3 进程停止后继续

```powershell
.\vibePlayerQT daemon stop

# 查询时可自动重启宿主；未完成任务显示 interrupted
.\vibePlayerQT task list
.\vibePlayerQT task resume TASK_ID
```

WebDAV 下载有强 ETag 时可恢复当前文件，并校验已完成文件；SMB 按完整文件检查点继续，当前文件重复制。视频/TSSL 重试重新执行业务流程，不恢复 FFmpeg 内部进度。

隐私任务重启后要重新提供 `--private --pin-env PLAYER_PIN`。持久任务需要的临时凭据保存到安全存储，不写入任务数据库；缺少安全后端时，含临时凭据的后台提交会报错。

后台任务属于 CLI 宿主，`task list` 不显示桌面传输列表。细节见 [后台任务与检查点](CliBackgroundTasks.md)。

<a name="config"></a>

## 13. 配置查询与全部配置键

```powershell
.\vibePlayerQT config list
.\vibePlayerQT config get m3u8s/containerFormat
.\vibePlayerQT config set m3u8s/containerFormat m3u8sp
.\vibePlayerQT config set m3u8s/parallelJobs 2
.\vibePlayerQT config set encryptedHlsRestore/outputDirectory "D:\还原视频"
.\vibePlayerQT config set player/volume 75
```

配置键采用白名单；未知键会报错。以下默认值指全新配置，已有桌面设置可能不同。`空` 表示未指定。布尔值使用 `true/false`；列表使用逗号分隔。

### 13.1 视频、下载与还原

| 配置键 | 默认值 | 允许值/说明 |
| --- | --- | --- |
| `webdav/defaultDownloadDirectory` | 空 | 默认下载目录 |
| `m3u8s/outputDirectory` | 空 | 本地打包输出；空时使用用户视频/文档目录下的 `vibePlayerQT/M3U8S` |
| `m3u8s/ffmpegExecutable` | 空 | FFmpeg 路径；空时按定位规则查找 |
| `m3u8s/outputMode` | `local` | `local/webdav` |
| `m3u8s/webDavServiceId` | 空 | WebDAV 输出服务 ID |
| `m3u8s/webDavPath` | 空 | WebDAV 输出路径 |
| `m3u8s/fallbackDirectory` | 空 | WebDAV 模式本地打包/保留结果目录 |
| `m3u8s/temporaryDirectory` | 空 | 临时目录，空时使用系统临时目录 |
| `m3u8s/keepSuccessfulLocal` | `false` | 上传成功后保留本地 |
| `m3u8s/videoEncoding` | `h264` | `copy/h264/h265/auto` |
| `m3u8s/autoVideoCodecs` | `h264,h265` | auto 模式复制编码列表 |
| `m3u8s/autoVideoTarget` | `h264` | `h264/h265` |
| `m3u8s/audioEncoding` | `aac` | `copy/aac` |
| `m3u8s/videoQuality` | `balanced` | `high/balanced/compact` |
| `m3u8s/containerFormat` | `m3u8sp` | `m3u8s/m3u8sp` |
| `m3u8s/parallelJobs` | `1` | 正整数，不能超过当前允许上限，最多 `4` |
| `encryptedHlsRestore/outputDirectory` | 空 | 还原输出目录；还原前必须配置或传 `--output` |
| `encryptedHlsRestore/format` | `original` | `original/mkv/mp4` |

普通 `webdav download` 仍需明确给出目标路径，不会因为保存默认下载目录而省略位置参数。

### 13.2 TSSL 备份

| 配置键 | 默认值 | 说明 |
| --- | --- | --- |
| `tsslBackup/target` | `none` | `none/local/webdav/s3` |
| `tsslBackup/format` | `zip` | `zip/files`；恢复自动识别两种格式 |
| `tsslBackup/localPath` | 空 | 本地备份目录 |
| `tsslBackup/webDavServiceId` | 空 | WebDAV 服务 ID |
| `tsslBackup/webDavPath` | `vibePlayerQT/tssl` | WebDAV 备份路径 |
| `tsslBackup/s3Endpoint` | 空 | S3 HTTP(S) 端点，执行时遵循 HTTPS 要求 |
| `tsslBackup/s3Bucket` | 空 | S3 桶名 |
| `tsslBackup/s3Region` | `us-east-1` | S3 签名区域 |
| `tsslBackup/s3Prefix` | `vibePlayerQT/tssl` | S3 对象前缀 |
| `tsslBackup/s3AccessKey` | 空 | S3 Access Key ID |

密码、PIN、Token 和 S3 Secret Access Key 不属于 `config` 白名单。S3 secret 使用安全存储或环境变量。

### 13.3 外观、历史、更新和播放器

| 配置键 | 默认值 | 允许值/说明 |
| --- | --- | --- |
| `desktop/minimizeToTray` | `true` | 最小化到托盘 |
| `appearance/themeMode` | `dark` | `dark/light/system` |
| `appearance/languageMode` | `system` | `system/zh_CN/en_US` |
| `appearance/embyHomeLayout` | `trendy` | `trendy/traditional` |
| `appearance/jellyfinHomeLayout` | `trendy` | `trendy/traditional` |
| `appearance/playerLayout` | `trendy` | `trendy/traditional` |
| `appearance/serviceCardSizePercent` | `100` | 整数 `50..150` |
| `appearance/pageTransitionsEnabled` | `true` | 页面切换动画 |
| `history/retentionDays` | `30` | 整数 `1..3650` |
| `webdav/showM3u8sIdentifier` | `true` | 显示加密包标识 |
| `webdav/showM3u8sSourceFileName` | `true` | 显示加密包原文件名 |
| `updates/channel` | `stable` | `stable/beta` |
| `updates/automaticCheck` | `true` | 自动检查更新 |
| `player/volume` | `100` | CLI 下次播放的默认音量，整数 `0..100` |
| `player/speed` | `1.0` | CLI 下次播放的默认倍速，`0.5..5` |
| `player/subtitleFile` | 空 | 存在的本地字幕文件；空值清除默认字幕 |

外观设置在桌面端下次读取时生效；`player/*` 在下一次 CLI `play` 读取，不改变正在运行的桌面播放器。`player volume` 等控制当前播放，`config set player/volume` 修改以后播放的默认值。

<a name="shell"></a>

## 14. 持续交互终端 Shell

### 14.1 当前服务与目录

启动：

```powershell
.\vibePlayerQT shell
# 或让后续命令使用已有环境凭据
.\vibePlayerQT shell --password-env DAV_PASSWORD --pin-env PLAYER_PIN
```

以下内容在 `vibe>` 提示符中输入，省略程序名前缀：

```text
list
use "家庭 DAV"
pwd
cd "/电影"
ls
stat "影片.mp4"
mkdir "新 文件夹"
upload "D:\视频\另一部.mp4" "新 文件夹/另一部.mp4"
download "影片.mp4" "D:\下载\影片.mp4"
copy "影片.mp4" "副本.mp4"
rename "副本.mp4" "新名称.mp4"
remove "新名称.mp4" --yes
cd ..
pwd
exit
```

| Shell 命令 | 说明 |
| --- | --- |
| `use SERVICE` | 选中 WebDAV 服务，当前目录重置为 `/` |
| `pwd` | 显示当前远程目录 |
| `cd PATH` | 验证目录后切换；相对路径基于当前远程目录 |
| `ls/stat/mkdir/upload/download/remove/copy/move/rename` | 当前 WebDAV 服务与目录的简写 |
| `privacy unlock [--pin-env NAME]` | 解锁本次 Shell 会话 |
| `privacy lock` | 锁定并清空当前服务和目录 |
| `history` / `history --clear` | 当前会话历史/清空历史 |
| `complete [PREFIX]` | 查看补全候选 |
| `exit` | 退出 Shell |

简写仅用于 WebDAV；SMB 使用完整 `smb` 命令。完整分组命令仍可指定其他服务，其路径按该服务根目录解析。Shell 的 `cd` 不改变本地工作目录。

### 14.2 隐私解锁

```text
privacy unlock --pin-env PLAYER_PIN
list
use "私密 DAV"
ls
privacy lock
exit
```

解锁仅在当前 Shell 有效。也可启动 `shell --private --pin-env PLAYER_PIN`。单条命令的 `--private` 不会永久解锁整个 Shell。

### 14.3 引号、变量、结果管道和后台提交

Windows 支持 Tab 补全、上下键历史和光标编辑。历史仅保存在当前会话内存，不写磁盘；`history --clear` 清空。

支持单/双引号，反斜线保留原样；引号内部连续两个相同引号表示字面引号。环境变量支持 `$NAME` 和 `${NAME}`：单引号不展开，双引号展开，缺失变量会报错。

下面是 **vibePlayerQT Shell 内部语法**，请在 `vibe>` 提示符中输入：

```text
list | select id name type
list | where type WebDAV | count
config get appearance/themeMode | get appearance/themeMode
config set appearance/themeMode $PLAYER_THEME
config set appearance/themeMode $(config get appearance/themeMode | get appearance/themeMode)
complete "webdav m"
use "家庭 DAV"
download "/电影/长片.mp4" "D:\下载\长片.mp4" &
task list
```

`PLAYER_THEME` 必须在启动 Shell 前由环境设置。结果管道处理 CLI 的结构化 `data`：

| 过滤器 | 说明 |
| --- | --- |
| `select FIELD...` | 选择对象字段 |
| `where FIELD VALUE` | 按字段值相等过滤 |
| `get FIELD` | 提取对象中的指定字段 |
| `count` | 返回数组/对象元素数量 |

`$(...)` 执行 CLI 内部命令并取结果，可嵌套最多 8 层。行末 `&` 等价于 `--background`，仅适用于支持后台的命令。

这里的管道/替换不调用外部 PowerShell 或 CMD，也不执行外部程序。`Get-ChildItem`、`dir`、系统重定向和系统管道应在外部终端使用。Shell 内不能嵌套 `shell`、启动桌面、执行 `daemon run` 或切换 `--data-dir`。

### 14.4 从文件执行 CLI 命令

创建 UTF-8 文件 `commands.txt`，每行一个 CLI 命令；不要写程序前缀，也不要写 PowerShell 注释：

```text
list
use "家庭 DAV"
ls "/"
mkdir "/脚本上传" --parents
upload "D:\视频\影片.mp4" "/脚本上传/影片.mp4"
exit
```

CMD：

```bat
vibePlayerQT.com shell --fail-fast --password-env DAV_PASSWORD < commands.txt
echo %ERRORLEVEL%
```

PowerShell 7：

```powershell
Get-Content -LiteralPath ".\commands.txt" -Encoding utf8 | .\vibePlayerQT.com shell --fail-fast --password-env DAV_PASSWORD
$LASTEXITCODE
```

Windows PowerShell 5.1 向原生程序传输中文脚本时，先将 `$OutputEncoding` 设置为 UTF-8，或使用 CMD 的文件输入重定向。

默认脚本错误后继续执行，最终保留最后一次非零退出码；`--fail-fast` 在首个错误退出。Shell 启动时的 `--yes` 不继承到各条删除命令，每条删除都要自己写 `--yes`。

<a name="automation"></a>

## 15. JSON、退出码与自动化脚本

### 15.1 输出结构与重定向

普通输出是 UTF-8 表格/字段列表，包含完整 ID、路径和批量计数，适合人阅读。自动化优先使用 `--json`。

独立命令输出一个 JSON 对象：

```json
{"schemaVersion":1,"command":"service list","ok":true,"data":[],"error":null}
```

失败时 `ok=false`，`error` 包含数字 `code` 与 `message`；批量失败时 `data` 仍可能包含计数和项目明细。Shell 的 `--json` 模式每条输入命令输出一行对象，不是一个整体 JSON 数组。

```powershell
.\vibePlayerQT list --json > ".\services.json" 2> ".\services-errors.log"
.\vibePlayerQT webdav upload "家庭 DAV" "D:\视频\影片.mp4" "/电影/影片.mp4" --json 1> ".\upload-result.json" 2> ".\upload-progress.log"
```

结果写标准输出，进度、日志和错误说明写标准错误。需要解析 JSON 时，不要用 `2>&1` 把两个流合并。

PowerShell 5.1 的 `>` 输出文件编码可能是 UTF-16；CMD 和新版 PowerShell 的行为不同。需要固定 UTF-8 文件时，捕获后明确写入：

```powershell
$resultText = & .\vibePlayerQT.com list --json
$commandExit = $LASTEXITCODE
if ($commandExit -ne 0) { throw "查询失败，退出码：$commandExit" }
[System.IO.File]::WriteAllText(
    (Join-Path (Get-Location).Path "services.json"),
    ($resultText -join [Environment]::NewLine),
    [System.Text.UTF8Encoding]::new($false)
)
```

### 15.2 退出码

| 退出码 | 含义 |
| --- | --- |
| `0` | 成功 |
| `1` | 执行失败；部分批量命令全部失败也返回此值 |
| `2` | 参数错误，或脚本删除缺少 `--yes` |
| `3` | 认证或隐私解锁失败 |
| `4` | 目标不存在 |
| `5` | 同名冲突或当前状态不允许操作 |
| `6` | 批量部分失败，需要检查 `data` 的计数/明细 |
| `130` | 已取消 |

CMD 读取 `%ERRORLEVEL%`；PowerShell 读取 `$LASTEXITCODE`。应在紧接着 CLI 调用的位置保存退出码，避免被后续命令覆盖。

### 15.3 可直接保存的 PowerShell 上传脚本

保存为 `upload.ps1`。`DAV_PASSWORD` 由运行环境注入；脚本参数指定已有服务、源文件和最终远程路径：

```powershell
param(
    [Parameter(Mandatory = $true)][string]$Service,
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Target,
    [string]$Cli = ".\vibePlayerQT.com"
)

$jsonText = & $Cli webdav upload $Service $Source $Target --password-env DAV_PASSWORD --json 2> ".\upload-progress.log"
$commandExit = $LASTEXITCODE
$result = ($jsonText -join [Environment]::NewLine) | ConvertFrom-Json

if ($commandExit -ne 0 -or -not $result.ok) {
    [Console]::Error.WriteLine("上传失败，退出码 {0}：{1}", $commandExit, $result.error.message)
    exit $commandExit
}

Write-Output $result.data
exit 0
```

调用：

```powershell
.\upload.ps1 -Service "家庭 DAV" -Source "D:\视频\影片.mp4" -Target "/电影/影片.mp4"
```

目标父目录需要先存在；默认保护同名文件。需要覆盖时在脚本的 CLI 调用处明确加入 `--overwrite`。

### 15.4 按 ID 操作服务与获取后台任务 ID

```powershell
$text = & .\vibePlayerQT.com list --json
$code = $LASTEXITCODE
if ($code -ne 0) { throw "无法查询服务，退出码：$code" }
$services = ($text -join [Environment]::NewLine) | ConvertFrom-Json
$davServices = @($services.data | Where-Object { $_.name -eq "家庭 DAV" -and $_.type -eq "WebDAV" })
if ($davServices.Count -ne 1) { throw "需要唯一的家庭 DAV 服务，或改用已知完整 ID" }
$serviceId = $davServices[0].id

$text = & .\vibePlayerQT.com webdav download $serviceId "/电影/合集" "D:\下载\合集" --background --password-env DAV_PASSWORD --json
$code = $LASTEXITCODE
if ($code -ne 0) { throw "后台提交失败，退出码：$code" }
$job = ($text -join [Environment]::NewLine) | ConvertFrom-Json
$taskId = $job.data.id
.\vibePlayerQT.com task show $taskId --json
```

`--background` 的提交成功不表示任务成功，自动化还应根据 `task show` 的状态和最终结果决定后续动作。

### 15.5 在 GitHub Actions 中运行

下面是可加入 Windows 工作流的**步骤片段**。工作流需先构建/部署 CLI 和运行依赖，并把 `VIBE_CLI` 设置为 `.com` 的实际完整路径。仓库的 `DAV_PASSWORD` secret 提供密码；示例测试地址、用户和文件请替换。

```yaml
- name: Upload with vibePlayerQT CLI
  shell: pwsh
  env:
    VIBE_CLI: D:\a\example\example\package\bin\vibePlayerQT.com
    VIBE_PROFILE: D:\a\example\example\cli-profile
    DAV_PASSWORD: ${{ secrets.DAV_PASSWORD }}
  run: |
    & $env:VIBE_CLI service add --type webdav --name CI-DAV --url "https://dav.example.com/dav/" --username ci --data-dir $env:VIBE_PROFILE
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $env:VIBE_CLI webdav upload CI-DAV ".\artifact.zip" "/artifact.zip" --password-env DAV_PASSWORD --json --data-dir $env:VIBE_PROFILE
    exit $LASTEXITCODE
```

这是独立工作流教程示例，不会自动创建或修改仓库 secret。一次性上传可以直接用环境密码；需要后续持久会话时再执行 `service login`。

<a name="troubleshooting"></a>

## 16. 常见问题与当前边界

| 情况 | 处理方式 |
| --- | --- |
| 运行后打开桌面，没有 CLI 输出 | 显式运行 `vibePlayerQT.com`，检查是否误用了 `.exe` 或没有命令 |
| 首次 `list` 是空列表 | 没有已保存服务，先添加；也检查是否选错 `--data-dir` |
| 同名服务报错 | 使用 `list` 中的完整 ID |
| `Saved` 但连接失败 | 保存状态不代表在线，运行 `service check`；Emby/Jellyfin 重新登录 |
| 认证失败、变量未设置 | 检查变量名称和作用域；脚本必须提供环境凭据或保存凭据 |
| 知道隐私 ID 仍找不到卡片 | 加 `--private --pin-env NAME`，或在 Shell 中解锁 |
| 同名冲突 | 换最终目标路径，或对支持的命令明确加 `--overwrite` |
| 下载提示父目录不存在 | 先创建本地父目录，下载不会自动补齐该父目录 |
| WebDAV 新建/上传提示缺失目录 | 先运行 `webdav mkdir ... --parents` |
| WebDAV COPY/MOVE 失败 | 检查服务器是否支持对应方法以及账号权限 |
| SMB 系统错误 `1219` | 检查 Windows 中该主机已有连接使用的账号 |
| 缺少 FFmpeg、编码器或封装能力 | 检查 `m3u8s/ffmpegExecutable` 或 `--ffmpeg`，确认所选参数被本机 FFmpeg 支持 |
| 视频还原缺少密钥 | 先 `tssl import` 对应密钥，确认 profile 和摘要匹配 |
| M3U8S 分段找不到 | 保留完整打包目录，不能只移动 `.m3u8s` |
| 还原结果格式与原文件不同 | 还原受打包保留轨道/编码影响，`original` 不保证字节级原样恢复 |
| 停止宿主后任务没有自动跑 | 用 `task list` 找到 `interrupted` 项，再 `task resume` |
| 暂停视频/TSSL 任务失败 | 当前阶段不支持暂停；可取消，之后按业务规则重试 |
| JSON 解析失败 | 标准错误另存，避免 `2>&1`；Shell 输出是每命令一行 |
| 中文脚本在 PowerShell 5.1 中乱码 | 设置原生管道的 `$OutputEncoding` 为 UTF-8，或用 CMD 输入重定向 |

当前能力边界：

- CLI 播放控制与后台任务管理仅针对各自的 CLI 宿主。
- SMB 直接服务入口在 Windows 实现，添加 SMB 卡片不代表已有桌面 SMB 浏览页。
- TSSL 文件和 ZIP 备份本身没有密码加密；备份命令不备份视频。
- S3 不支持自动 SSO/MFA/`credential_process`/角色链和长任务中途刷新。
- Shell 只执行内部 CLI 命令及结果过滤，不是系统 Shell。
- WebDAV 续传依赖服务器的 ETag/Range；SMB 和视频/TSSL 恢复规则不同。

更多实现和验证信息：

- [CLI 命令与模块说明](CLI.md)
- [终端输出格式](CliOutput.md)
- [媒体与播放](CliMedia.md)
- [后台任务与检查点](CliBackgroundTasks.md)
- [Shell 表达式](CliShell.md)
- [Windows SMB](SmbClient.md)
- [S3 临时凭据](S3CredentialProvider.md)
- [加密视频还原](EncryptedHlsRestore.md)
- [Windows 验收记录](CliVerification.md)
