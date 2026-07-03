# vibePlayerQT

[English](README.md) | [简体中文](README.zh-CN.md)

vibePlayerQT 是一个基于 Qt Quick、C++23 和 libmpv 的跨平台桌面媒体中心。项目目标是在原生桌面应用里提供现代化的媒体浏览与播放体验，覆盖 Emby、Jellyfin、WebDAV、IPTV 等媒体来源，并保持代码结构长期可维护。

项目仍在积极开发中。当前代码重点覆盖原生桌面播放、Emby/Jellyfin 媒体库浏览、WebDAV 文件访问和本地 IPTV 播放列表播放。

## 功能亮点

- 原生 Qt 6 / Qt Quick 桌面应用。
- 使用 libmpv Window Embedding 播放，播放生命周期统一由 `PlayerController` 管理。
- 支持 Emby 和 Jellyfin 登录、媒体库浏览、继续观看、媒体详情、剧集季/集列表。
- 支持 Emby 和 Jellyfin 断点续播与播放进度上报。
- 支持 WebDAV 目录浏览、上传、下载、文件夹传输、空间提示和视频播放。
- 支持 IPTV M3U/M3U8 导入、分组筛选、搜索和播放。
- 支持服务卡片保存、同一服务器多账号、可选自动登录和拖拽排序。
- 支持深色/浅色/跟随系统主题、中英文界面文本和最小化到系统托盘。
- 使用 CMake 构建，并通过 GitHub Actions 打包 Windows、macOS、Linux 版本。

## 项目状态

| 模块 | 状态 |
| --- | --- |
| Emby | 已实现登录、媒体库、媒体列表、继续观看、详情、播放链接生成和播放上报。 |
| Jellyfin | 已实现登录、媒体库、媒体列表、继续观看、详情、播放链接生成和播放上报。 |
| WebDAV | 已实现 PROPFIND 浏览、MKCOL、GET/PUT 传输、传输列表、空间提示和视频播放。 |
| IPTV | 已实现本地 M3U/M3U8 导入、频道列表、分组、搜索和播放。 |
| SMB | 规划中。SMB2/SMB3 应通过 libsmb2 等成熟库实现，不自行实现协议。 |
| 打包 | CI 会构建 Windows x86_64、macOS x86_64/arm64、Linux x86_64/arm64 tar.gz 与 AppImage 发布包。 |

## 架构

项目采用分层桌面应用结构：

```text
QML UI
  -> ViewModel
    -> Service
      -> Repository
        -> Infrastructure
```

QML 只负责页面展示、动画、输入和导航。网络请求、数据库访问、媒体解析和播放编排放在 C++ 层。

主要目录：

```text
src/
  app/          应用启动、系统托盘和窗口行为
  database/     SQLite / QSettings 持久化
  models/       共享领域模型
  network/      QNetworkAccessManager 封装
  player/       libmpv 集成和 QML 视频项
  services/     Emby、Jellyfin、WebDAV、IPTV 和凭据管理
  settings/     应用设置
  utils/        日志和 JSON 辅助工具
  viewmodels/   暴露给 QML 的状态和列表模型
qml/            Qt Quick UI
docs/           构建说明
scripts/        本地构建脚本
VIBEDOCS/       模块设计文档
```

## 技术栈

- C++23
- Qt 6.5+，包含 Qt Quick、QML、Quick Controls 2、Network、SQL、Widgets、XML
- SQLite，基于 Qt SQL
- libmpv
- CMake 3.24+

## 依赖

### Windows

需要安装：

- Visual Studio 2022 C++ 工具链
- 使用项目脚本时需要 LLVM/clang-cl
- Qt 6.7.3 或其他 Qt 6.5+ 且兼容 MSVC 的版本
- CMake 和 Ninja

本地 Windows 构建期望 libmpv 开发包位于：

```text
third_party/mpv/dev/
  include/mpv/client.h
  libmpv.dll.a
  libmpv-2.dll
```

大型 libmpv 压缩包和运行时二进制不会提交到仓库。CI 与本地 setup 使用的固定版本记录在 `VIBEDOCS/PlayerRuntime.md`。

### macOS 和 Linux

通过系统包管理器安装 Qt 6、CMake、Ninja、pkg-config 和 libmpv 开发文件。GitHub Actions 在 macOS 使用 Homebrew，在 Ubuntu 使用 apt。

## 构建

### Windows Clang

在 PowerShell 中运行：

```powershell
.\scripts\configure-clang.cmd
.\scripts\build-clang.cmd
```

可执行文件生成在：

```text
build-clang/vibePlayerQT.exe
```

更多 Windows 构建方式见 `docs/BuildWindows.md`。

### 通用 CMake

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix package
```

Linux 和 macOS 上，CMake 通过 `pkg-config` 查找 libmpv。

## GitHub Releases

`.github/workflows/build-release.yml` 会在推送到 `main` 时构建所有平台。推送 `v1.0.0` 这类标签时，还会发布 GitHub Release，并上传以下平台的构建产物：

- Windows x86_64
- macOS x86_64
- macOS arm64
- Linux x86_64 tar.gz 和 AppImage
- Linux arm64 tar.gz 和 AppImage

## 文档

设计说明和实现记录位于 `VIBEDOCS/`。

建议从这些文件开始：

- `docs/BuildWindows.md`
- `VIBEDOCS/MediaServices.md`
- `VIBEDOCS/PlayerRuntime.md`
- `VIBEDOCS/WebDAV.md`
- `VIBEDOCS/IPTV.md`
- `VIBEDOCS/GitHubActionsRelease.md`
- `TODO.md`

部分早期本地文档可能存在编码问题。公开说明请以源码和当前 README 为准。

## 安全说明

- 不要提交真实服务器凭据、访问令牌、Cookie、本地 SQLite 数据库或包含令牌的日志。
- Emby/Jellyfin 访问令牌当前保存在本地 SQLite 会话数据库中。
- WebDAV 密码在 Windows 上优先使用 Credential Manager；其他平台当前会再次请求密码。
- 日志和界面不得暴露密码、Token、Cookie 或完整的含 Token 播放 URL。

## 路线图

- 在 macOS 和 Linux 上补充 Keychain / Secret Service 安全凭据存储。
- 增加图片代理/缓存，避免 QML 直接持有含 Token 的图片 URL。
- 增强播放错误映射和外挂字幕支持。
- 在更多 WebDAV/IPTV 服务实现上验证播放兼容性。
- 通过 libsmb2 或其他成熟库加入 SMB2/SMB3 支持。
- 扩展自动化测试和发布包冒烟验证。
