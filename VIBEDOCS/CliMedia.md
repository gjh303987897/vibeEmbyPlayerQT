# CLI 媒体查询与播放

CliMedia 复用 MediaServiceClient、EmbyClient/JellyfinClient 和 SessionRepository。
libraries 返回媒体库；ls SERVICE [PARENT_ID] 浏览目录；ls/search 支持
offset>=0、limit 1..1000；show 返回详情/续播位置，不输出带 Token 的播放 URL。
CLI 现已补齐 IPTV 多线路探测/回退、全局播放历史、本地目录与定时播放；
命令与生命周期见 [CliDesktopParity.md](CliDesktopParity.md) 和
[教程第 17 节](CLI-Tutorial.md#desktop-parity)。
Emby/Jellyfin 先 service login。IPTV channels 返回 ID/名称/组，play 支持
完整 ID 或唯一名称。远程播放列表保存托管副本，相对地址基于原 URL 解析。

~~~powershell
.\vibePlayerQT media libraries "家庭 Emby"
.\vibePlayerQT media ls "家庭 Emby" LIBRARY_ID --limit 50
.\vibePlayerQT media search "家庭 Emby" "影片名称"
.\vibePlayerQT play "家庭 Emby" ITEM_ID
.\vibePlayerQT play "D:\视频\影片.mp4" --start 30
.\vibePlayerQT play "https://example.com/video.mp4" --wait
.\vibePlayerQT player status
.\vibePlayerQT player pause
.\vibePlayerQT player seek 120
.\vibePlayerQT player subtitle "D:\字幕\影片.srt"
.\vibePlayerQT player audio 1
.\vibePlayerQT player stop
~~~

play 默认确认请求，不等待影片结束；脚本使用 --wait，播放失败返回 1，
Ctrl+C 停止播放并返回 130。--headless 关闭视频和音频输出，用于自动验证。
volume 0..100 整数，speed 0.5..5，seek 绝对秒数。subtitle 可用轨道行号
（-1 关闭）或文件；audio 用 status 返回的行号。

vibePlayerPlayback 使用 QGuiApplication/QWindow 和 native window ID，
仅通过现有 PlayerController 调用 libmpv，没有外部 mpv.exe 或 mpv IPC。
普通 CLI 查询不初始化此宿主。M3U8S/M3U8SP 复用 EncryptedHlsPlaybackProxy。
普通 WebDAV 使用已有 WebDavPlaybackProxy 和 QNetworkAccessManager，
认证留在代理中，libmpv 读取本机流地址。
Emby/Jellyfin 在实际开始、每 10 秒、停止时发送已有播放报告。
宿主与桌面独立，每个 profile 一个；关闭视频窗口停止播放，player stop 结束宿主。
player/* 默认值每次 play 读取，不更改正在运行的桌面播放器。
Windows 启动与后台任务共用不继承重定向句柄的设置；
--wait 返回后，PowerShell 能立即完成 JSON 捕获，宿主仍可接受后续控制。

密码/Token/PIN 仅经用户范围的本地 socket 传输，不写文件或结果；
按数据库真实服务属性验证私密来源，私密播放的查询和控制也需 PIN。
每次请求刷新 PIN 配置，创建/更改 PIN 不要求重启播放宿主。
远程加密包准备阶段的 stop 会取消网络请求并等待准备回调完成后退出；
本地包检查等待现有有界读取完成。代理新增 cancelNetworkRequests 接口，
桌面端不调用该接口，保留现有播放流程。
依据：[Emby API](https://dev.emby.media/reference/RestAPI/UserLibraryService.html)、
[Jellyfin OpenAPI](https://fra2.mirror.jellyfin.org/main/openapi/jellyfin-openapi-stable.json)、
[mpv wid](https://mpv.io/manual/stable/#options-wid)。
