# AGENTS.md

# 项目说明

本项目是一个基于 Qt Quick 与 libmpv 的跨平台桌面媒体播放器。

项目目标：

- 提供现代化桌面媒体中心体验
- 支持 Emby、Jellyfin、WebDAV、SMB、IPTV 等媒体来源
- 提供高性能视频播放能力
- 支持 Windows、macOS、Linux
- 保持长期可维护性与可扩展性

本项目定位类似：

- Jellyfin Media Player
- Infuse
- VidHub
- Kodi（轻量化方向）

项目优先级：

1. 播放体验
2. 跨平台兼容性
3. 代码可维护性
4. 界面美观性

---

# 特别注意 EMBY Jellyfin等一定要查询相关的官方API文档后再编写代码，任何不确定的实现问题也要查询官方文档

---

# 技术栈

## 编程语言

C++23

新增代码应优先使用现代 C++23 特性：

- std::expected
- std::ranges
- std::span
- std::filesystem
- std::chrono
- std::format
- Concepts

避免使用过时写法。

---

## UI框架

Qt 6.x

Qt Quick（QML）

QML仅负责：

- 页面展示
- 动画效果
- 用户交互
- 页面导航

禁止在QML中实现复杂业务逻辑。

业务逻辑必须放在C++层。

---

## 播放器

播放器核心：

libmpv

采用：

Window Embedding（窗口嵌入）

方案实现。

禁止：

- mpv IPC控制模式
- 外部启动 mpv.exe
- JSON Socket 控制模式

播放器生命周期由本程序统一管理。

---

## 网络层

统一使用：

QNetworkAccessManager

用于：

- Emby API
- Jellyfin API
- WebDAV API
- 元数据获取
- 图片加载

避免引入额外HTTP库。

---

## 数据存储

SQLite

用于：

- 播放记录
- 历史记录
- 断点续播
- 用户设置
- 缓存索引

---

# 支持的媒体来源

## Emby

需要支持：

- 用户登录
- 多服务器管理
- 媒体库浏览
- 搜索
- 继续观看
- 断点续播
- 用户信息获取

必须优先使用官方REST API。

---

## Jellyfin

需要支持：

- 用户登录
- 多服务器管理
- 媒体库浏览
- 搜索
- 继续观看
- 断点续播
- 用户信息获取

必须优先使用官方REST API。

---

## WebDAV

需要支持：

- 用户认证
- 文件浏览
- 文件夹浏览
- 视频直接播放

使用标准WebDAV协议实现。

涉及：

- PROPFIND
- OPTIONS
- GET

禁止自行设计协议。

---

## SMB

需要支持：

- SMB2
- SMB3

推荐：

libsmb2

禁止自行实现SMB协议。

---

## IPTV

需要支持：

- M3U
- M3U8

基础功能：

- 频道列表
- 分类分组
- 收藏
- 搜索

未来支持：

- EPG节目单
- 台标
- 回看

流媒体播放优先交由libmpv处理。

---

# 架构要求

采用分层架构。

推荐结构：

UI层(QML)
↓
ViewModel层
↓
Service层
↓
Repository层
↓
基础设施层

禁止：

QML → 网络请求

QML → 数据库

QML只能与ViewModel通信。

---

# 推荐目录结构

src/

    app/

    player/

    services/

        emby/
        jellyfin/
        webdav/
        smb/
        iptv/

    database/

    network/

    cache/

    models/

    viewmodels/

    settings/

    utils/

qml/

resources/

tests/

docs/

---

# 播放器架构

播放器统一由：

PlayerController

管理。

PlayerController负责：

- 播放
- 暂停
- 停止
- 快进
- 音量控制
- 字幕切换
- 音轨切换
- 倍速播放

除PlayerController外：

禁止任何模块直接调用libmpv API。

---

# 开发规范

## 修改代码之前

必须：

1. 阅读相关代码
2. 理解现有实现
3. 搜索已有类似功能
4. 评估影响范围

禁止直接猜测实现方式。

---

## 积极复用&提升状态

每次编写一个模块都考虑这个模块是否可以被单独提取出来，对于每个被单独提取的模块都应该记录一个详细的文档放在 /VIBEDOCS下面
每次实现某个功能时先去阅读文档查找是否有现有的模块可以使用

---

## 文档优先

如果遇到不确定内容：

优先查阅官方文档。

包括但不限于：

- Qt官方文档
- libmpv文档
- Emby API文档
- Jellyfin API文档
- WebDAV RFC
- libsmb2文档

禁止编造接口。

禁止猜测函数签名。

---

## 最小修改原则

优先采用：

最小改动

完成需求。

避免：

- 大规模重构
- 无关代码修改
- 风格统一型修改

除非明确要求。

---

## 错误处理

优先使用：

std::expected

避免：

- bool返回值+错误字符串
- 魔法数字

错误必须明确可追踪。

---

## 日志规范

重要操作必须记录日志：

包括：

- 登录
- 播放
- 网络请求
- 错误信息
- 服务连接

日志必须便于问题排查。

推荐使用：

- spdlog

统一日志接口，避免直接使用 printf 或 qDebug。

---

# UI规范

界面风格参考：

- Infuse
- VidHub
- Apple TV
- Jellyfin Media Player

要求：

- 简洁
- 现代化
- 深色主题优先
- 适合遥控器和鼠标操作

避免：

- 复杂嵌套页面
- 冗余设置项
- Windows XP风格界面

---

# 性能要求

必须保证：

- 4K视频在线播放流畅
- 大型媒体库浏览不卡顿
- IPTV频道切换快速响应
- 元数据加载不阻塞界面

禁止在UI线程执行：

- 网络请求
- 文件扫描
- 数据库大量查询

优先采用异步实现。

---

# 跨平台要求

所有功能设计时必须考虑：

- Windows
- macOS
- Linux

禁止仅针对单平台实现。

如果必须使用平台特定代码：

需要：

- 单独封装
- 添加注释
- 提供兼容方案

---

# 安全要求

禁止：

- 明文存储密码
- 在日志中记录Token
- 在日志中记录Cookie

所有外部输入都视为不可信数据。

必须进行必要校验。

---

# 测试要求

涉及播放器修改时：

至少验证：

- 本地视频播放
- HTTP视频播放
- WebDAV播放
- SMB播放
- 字幕加载
- 音轨切换

涉及服务端修改时：

验证：

- 登录
- 获取媒体库
- 搜索
- 播放链接获取

---

# 禁止事项

禁止：

- 替换libmpv
- 引入Electron
- 引入CEF
- 使用WebView实现播放器
- 将Qt Quick改为Qt Widgets
- 引入无必要依赖

未经明确要求：

禁止重写现有模块。

---

# AI Agent工作流程

对于每一个任务：

1. 阅读相关代码
2. 理解现有架构
3. 查阅相关文档
4. 设计最小修改方案
5. 编写代码
6. 检查编译是否通过
7. 检查功能是否正常
8. 更新相关文档

禁止跳过代码阅读步骤。

禁止在不了解现有实现的情况下直接修改代码。

禁止凭经验猜测第三方库行为。

---

# 架构决策

## 为什么选择 Qt Quick

原因：

- 原生跨平台
- 性能优秀
- 动画能力强
- 适合媒体中心界面
- 长期维护成本低

---

## 为什么选择 libmpv

原因：

- 基于 FFmpeg
- 编码格式支持广泛
- HDR支持完善
- 字幕支持优秀
- 硬件解码成熟
- 跨平台能力强

---

## 为什么选择 Window Embedding

采用 libmpv 窗口嵌入方案。

原因：

- 实现简单
- 稳定性高
- 跨平台成熟
- 性能损耗极低
- 易于维护

当前阶段不采用：

- OpenGL Render API
- Vulkan Render API

除非未来有明确需求。



# 最终目标

打造一个现代化、跨平台、高性能媒体播放器。

核心体验参考：

- Infuse
- VidHub

兼容生态参考：

- Emby
- Jellyfin
- WebDAV
- SMB
- IPTV

在保证播放体验的前提下，实现长期可维护、可扩展的架构设计。