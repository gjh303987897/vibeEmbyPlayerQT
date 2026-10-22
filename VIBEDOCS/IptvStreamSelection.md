# IPTV 线路选择与探测

## 模块与调用关系

`IptvParser::mergeChannels` 负责静态合并。共享 `ServiceOperations::savePlaylist`
在导入后调用；桌面服务加载与 CLI 读取也调用，兼容旧版本保存的重复频道。
合并不联网，不依赖 QML，保留原始 URL 和第一条频道 ID。

`IptvStreamSelector` 是无副作用的公共模块，桌面与 CLI 均可使用。
`candidates(channel, preference, now)` 返回按偏好排序并按实际 URL 去重的线路及 HLS 子清单候选。
`select(channel, preference, now)` 返回第一个候选的原始线路 `sourceUrl`、实际播放 URL
`playbackUrl` 和对应信息。`quality` 优先高分辨率，`dataSaver` 优先低码率。
未知、过期或失败信息不能当作零码率来比较。所有未知线路按原始顺序选择。
有效成功信息优先于未知，未知优先于失败；全部失败时仍保留首条，允许用户重试。

`IptvStreamInfo` 按原始 URL 记录可用性、宽高、bit/s 码率、编码、状态、UTC
检查时间及 `IptvStreamVariant` 数组。`serialize` / `deserialize` 处理 JSON；
数据库将完整数据保存在 `iptv_channels.stream_info`，频道摘要字段用于列表展示。
含有效分辨率的成功信息有效 6 小时，失败或缺少分辨率的信息有效 60 秒；这些期限通过 `fresh` 统一判断。
`qualityCheckedAt` 记录深入采样时间，避免重复开关菜单反复尝试失败的采样；
HLS 子清单通过 `probeState` 保存各自的探测结果。字段均在已有 JSON 中持久化，兼容旧数据。
数据库只读加载允许缺失新字段，不隐式迁移；写入初始化通过 `ensureColumn` 迁移。

`IptvProbe` 是 QObject 异步单任务探测器，网络统一使用 QNetworkAccessManager。
`probe(channelId, url, preference, detailed)` 取消上一个任务，`finished` 携带频道、原始
URL 与结果。`cancel` 先清除任务身份再调用 abort，防止同步 finished 回调误报结果；
排队的异步不支持协议结果也使用代次检查。析构取消活动任务。

## 资源限制与 HLS

- 仅主动探测 HTTP/HTTPS；其他协议交给 libmpv 播放并校正。
- 浅探测保持 128 KiB/请求与 6 秒超时。打开线路菜单才启用深入探测：512 KiB/请求、
  2 MiB/原始线路、最多 12 个请求、整个线路最多 12 秒，单个请求最多 4 秒。
  忽略 Range 的服务器同样由读缓冲与主动中止限制样本；拒绝起始 Range 的 400/416
  响应允许再试一次有相同限制的普通 GET。非零字节偏移必须返回正确的 Content-Range。
  重定向最多 4 次，不允许 HTTPS 降级到 HTTP。
- HLS 主清单解析 STREAM-INF，子清单相对地址以最终响应 URL 解析。
  简单主清单实际播放选中的子清单，不仅修改显示标签。
  有独立 AUDIO/SUBTITLES 组时保留主清单，由 libmpv 处理，以保存音轨和字幕。
  此类流的内部清晰度控制不由本模块强制覆盖。
- 清单已给出分辨率时直接复用。深入探测会继续读取缺失分辨率的 HLS 子清单、一个媒体
  分片及可选 EXT-X-MAP 初始化片段，支持 MPEG-TS、fMP4 和显式字节范围，递归深度最多 4。
  多画质主清单中缺少分辨率的子清单分别采样；独立音轨/字幕组仍保留主清单作为播放地址。
- 已下载的样本交给配置的 FFmpeg 同目录、应用目录或 PATH 中的 FFprobe。
  子进程只允许 file/pipe 协议，probesize 限于样本大小、analyzeduration 为 1 秒，
  解码线程数为 1、外部超时 2 秒。不会完整播放视频，也不允许 FFprobe 直接请求网络。
  缺少工具、超时、连接失败、加密分片、样本不足或不支持协议会显示明确状态，不虚构分辨率。
- 取消子进程时保留样本到进程退出，避免 Windows 文件占用导致清理失败。
- 默认服务页首批最多 12 个频道、36 条线路，串行浅探测；播放时优先当前频道。
  打开线路菜单会按需深入探测当前频道的全部原始线路，不受 36 条浅探测上限影响，
  始终只有一个网络请求或 FFprobe 任务运行，线路间留出 150 毫秒间隔。
  关闭菜单、切换频道或退出播放取消深入任务；重复打开复用缓存。导入不联网，不扫描全部频道；
  探测结果不会打断成功播放，失败时才会切换候选。
  首次未知频道会立即播放原始首条；后续播放利用已保存的探测结果选择线路。

## 桌面集成与数据校正

AppViewModel 管理队列和偏好，列表模型只暴露线路数和分辨率等展示字段。
MpvVideoItem 的实际媒体信息变化通过 ViewModel 写入对应线路，优先于后台结果。
QML 不包含网络、数据库或线路排名逻辑。

元数据在 2 秒窗口内按频道合并最新快照，后台连接在工作线程创建和销毁。
写入任务串行，避免旧快照晚写覆盖新快照；失败使用现有 AppLogger 记录，不打印
源 URL、Token 或凭据。播放偏好独立保存在 `iptv/streamPreference`。
修改偏好从下一次播放生效，不打断当前频道。

## 自动回退与手动切换

桌面播放器现在支持以下行为：

- 预探测不可用的线路排在未知/可用线路之后；实际 libmpv 播放失败时，再从
  `candidates` 选择本轮尚未尝试的候选。优先级沿用画质偏好，包括同一个 HLS
  主清单中的其他清晰度。每个实际播放 URL 一轮只尝试一次，全部失败才显示错误。
- 首次加载 20 秒内没有收到播放开始通知，也会触发回退。已开始播放后的普通
  暂停、缓冲、用户停止和正常 EOF 不触发线路切换；不承诺自动识别无限缓冲或黑屏。
- 失败的普通线路与 HLS 子清单缓存暂不可用状态 60 秒。子清单使用 `failedAt`，
  不把整张 HLS 主清单都判为失效。真实播放恢复后清除对应子清单的失败状态。
- 新潮和传统布局都提供“播放线路”按钮，复用现有播放器 Tool Window 菜单，避免被 libmpv 原生子窗口遮挡。
  显示原始线路编号、分辨率、已知码率/编码、当前项和暂不可用状态。
  未完成时显示“等待探测”或“正在探测分辨率”，完成后显示分辨率或具体失败原因；
  不再显示笼统的“画质未知”。用户仍可显式尝试暂不可用线路。
  菜单按导入线路编号稳定排列，选择器仍按用户画质/省流偏好排名。
- 菜单完整位于控制栏窗口上方，保留 12 像素间距；可用高度不足时缩短列表并滚动。
  两种布局切换、窗口缩放与线路数量变化均重新计算位置。鼠标移动只延长控制栏
  可见时间，已显示时不重复抬高所有 Tool Window，避免重叠透明窗口反复换序导致底部闪烁。
- ViewModel 暴露 `iptvPlaybackStreams` 和 `switchIptvPlaybackStream(selectionId)`；
  `selectionId` 是原始线路与实际播放地址的摘要，界面不接收含凭据的地址。
  列表更新/重新排序后摘要仍指向同一候选，过期或非法 ID 不会切换。
- 手动选择优先于画质偏好，并开启新一轮失败回退；不会持久修改偏好。
  切换使用 loadfile replace，从直播当前位置重新接入，不保证时间线无缝、音轨 ID
  或字幕 ID 跨线路不变。已在播放的当前项点击不重载，失败后可点击它重试。
- 自动替换延迟到 Qt 事件循环下一轮，使用播放代次取消过期回调。切频道、手动
  选线路、退出播放会取消旧回退；正常退出也停止初始加载超时。

PlayerController 在 loadfile 后读取 `playlist/0/id`，IPTV 的 START_FILE/END_FILE
只处理最新请求条目，防止快速手动切换后旧流的失败终止新流。一般命令失败和
加载失败分开处理：只有 loadfile 失败或 END_FILE error 驱动线路回退，字幕、音轨、
播放器初始化等错误仍通过原有错误处理显示。失败事件完成旧播放记录后才排队换线。

CLI 独立播放器也使用相同的候选排名、失败缓存、20 秒初始加载超时和真实画质校正；
`iptv streams` / `player streams` 查询或探测，`player stream ID` 手动切换。
共享数据变更接口见 [IptvStreamWorkflow.md](IptvStreamWorkflow.md)，
CLI 生命周期与性能限制见 [CliDesktopParity.md](CliDesktopParity.md)。

## 验证

`IptvMultiSourceTest` 验证身份合并、URL 去重、旧 ID 保留、两种偏好、未知与
失败信息的排序、缓存过期、HLS 相对地址和独立音轨保护、每线路信息写入并
只读加载、旧表结构只读查询，以及本地 HTTP 超量响应与取消替换。
`AppViewModelStateTest` 还验证合并旧频道、不同偏好的实际播放 URL、播放信息
回写、忽略空/过期通知，以及改变偏好不打断当前播放。Windows 原生 IPTV 测试
验证真实 libmpv 播放的 1280×720 信息校正，以及播放后的文件选择生命周期。

这些测试使用临时数据库和本地测试源。真实供应商直播、认证、失效时间和显卡负载
仍需结合实际频道验收。新增用例覆盖失败回退、全部失败停止、手动取消排队回退、
初始加载超时，以及真实缺失 HD 文件后播放备用文件、打开线路菜单并点击切换。
原生菜单回归还覆盖 7 条线路、小窗口滚动、底部悬停，以及打开菜单期间切换两种布局，
校验菜单与控制栏的原生窗口几何不重叠。这些检查验证闪烁诱因已移除，不能代替所有显卡的视觉验收。

深入探测测试使用本地 HTTP 服务和 FFmpeg 生成的真实样本，验证直接 TS、重定向后的 HLS、
fMP4 初始化片段、缺少 RESOLUTION 的多画质主清单、Range 拒绝回退、循环清单、加密提示和取消。
ViewModel 测试验证 40 条线路逐条完成、缓存命中、失败状态以及探测不会切断当前播放。

## 官方依据

- [Qt QNetworkReply](https://doc.qt.io/qt-6/qnetworkreply.html)：异步回复、abort 和读缓冲。
- [HLS RFC 8216](https://www.rfc-editor.org/rfc/rfc8216)：STREAM-INF、相对 URI 与独立媒体组。
- [FFprobe](https://ffmpeg.org/ffprobe.html)：样本解析和 JSON 媒体信息输出。

线路切换事件依据：[mpv loadfile 与 playlist 条目 ID](https://mpv.io/manual/stable/#playlist-manipulation)。
