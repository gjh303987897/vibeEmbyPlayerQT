# FfmpegCapability 模块说明

位置：`src/services/ffmpeg/FfmpegCapability.{h,cpp}`

## 背景与决策（方案 B）

M3U8S 打包依赖**外部 ffmpeg 可执行文件**，它有意不捆绑、不固定版本（未来任务，
见 `TODO.md` 与 `deps/libmpv.lock.json` 的 `externalFfmpeg` 字段）。用户机器上
可能是任意构建：缺失、过旧、或缺少必需组件（如 `--disable-gpl` 构建没有
libx264/libx265）。

选定策略：程序启动后异步探测一次，不满足时弹出一次性警告对话框，明确告知
"M3U8S 打包不可用，其余功能不受影响"。不捆绑、不阻断、不持久化静音——每次
启动都会重新检测，用户装好 FFmpeg 后重启即恢复，无需任何清除设置的步骤。

## 探测契约

`FfmpegCapabilityProbe::run(executable)`（同步、生成子进程，必须在 worker 线程调用）：

1. `executable` 为空 → `Unavailable`（定位由调用方负责：
   `EncryptedHlsPackager::locateFfmpegExecutable()`，用户配置 → bundle → PATH；探针模块
   与打包栈零链接依赖，便于单测）。
2. 只跑一次 `ffmpeg -version`（不起 `-encoders` / `-muxers` 子进程）。先验
   **横幅身份**：首行必须匹配 `^ffmpeg\s+version`，否则判为 `Incompatible` 并
   在 `detail` 里回显横幅。这一道不可省：ffprobe.exe / ffplay.exe 与 ffmpeg.exe
   同目录，而 M3U8S 页允许手填可执行文件——没有身份校验时，错选的二进制会“解不
   出版本”而走进取宽分支，被当成可用。身份确认后解析版本，支持 `7.1.1`、
   `7.1-essentials_build-...`、`n6.1.1-3-g...`。
3. **版本下限 5.0 是唯一的硬约束**（打包管线使用的 HLS flags 与 force_key_frames
   行为自 5.x 起稳定）：低于下限 → `Incompatible`。
4. `git-2024-05-01-...` 这类日期戳横幅解不出版本号时**跳过下限**并记
   `Available`（日期戳构建按定义晚于下限），`detail` 里写明原因。

**有意不再做特性扫描**：曾经还要求 `libx264` / `libx265` / `aac` 编码器与 `hls`
muxer，现在去掉了。理由：启动阶段不该为两个额外子进程付代价，而且一个
`--disable-gpl` 的裁剪构建在打包失败时，ffmpeg 自己的诊断比表扫描说得清楚得多；
“对 ffmpeg 只限制版本”也是有意的策略选择——不捆绑、不锁构建，不把“官方标准构建才有
哪些组件”当启动门。

结果三种状态：`available` / `unavailable`（找不到或跑不起来）/ `incompatible`
（版本低于下限，或所选文件根本不是 FFmpeg），`detail` 为单行的人读摘要（写入日志，
字段报告可带出用户 FFmpeg 指纹）。

纯函数 `parseVersionBanner` 直接暴露给单测（`tests/FfmpegCapabilityTest.cpp`）。

## 与 UI 的接线（AppViewModel / Main.qml）

- `AppViewModel::initialize()` 尾部延迟 800ms 经 `QtConcurrent` 启动探测
  （子进程绝不进 GUI 线程；也避开首帧窗口抖动）。
- QML 属性：`ffmpegCapabilityState`（含 `probing` 过渡态）、
  `m3u8sFfmpegAvailable`（= usable，M3U8S 页状态徽标与开始按钮沿用）、
  `ffmpegCapabilityDetail`、`ffmpegWarningVisible`。
- `ffmpegWarningDialog`（ModernDialog，警告色感叹徽章 + 等宽 detail + 要求
  说明 + 官网下载按钮），`onFfmpegCapabilityChanged` 驱动打开；关闭只影响
  本次会话。
- M3U8S 页状态徽标四态：检测中 / 已就绪 / 不可用 / 未找到。检测中用中性色（答案
  未知不是错误）；`m3u8sFfmpegAvailable` 要求**探测已完成**，所以复检进行中“开始打包”
  会暂时置灰，而不是沿用上一个二进制的评价。
- 启动时 `initialize()` 先把保存的手填路径推给 `EncryptedHlsPackager`，再启动延迟探测，
  保证第一次回答就已经反映用户的选择。

## 手填 FFmpeg 路径（用户覆盖）

自动探测只看得见 bundle 目录与进程 `PATH`，而现实里两类情况看不见：便携/绿色安装
（未写进 PATH），以及进程 `PATH` 被启动链改写（服务、计划任务、某些 shell 拉起的子
进程只拿到被截断的 PATH——此时日志会写 "FFmpeg executable not found"，而用户机器上
其实装了 FFmpeg）。因此 M3U8S 页允许直接指定可执行文件。

- **存储**：`SessionRepository` → `QSettings` 键 `m3u8s/ffmpegExecutable`，绝对路径，
  空值 = 自动检测（`AppViewModel::m3u8sFfmpegPath`）。
- **生效点**：仍然只有 `EncryptedHlsPackager::locateFfmpegExecutable()` 一个入口，顺序为
  用户配置 → 程序同目录 → `PATH`；探针与打包共用，所以两者永远看到同一个二进制。
- **进程级状态**：`setConfiguredExecutablePath()` / `configuredExecutablePath()` 是静态
  方法。GUI 线程写、`QtConcurrent` 探针线程读（打包只在 `start()` 于 GUI 线程解析一次，
  再把副本交给 worker），因此值放在 mutex 后面，不是裸 `static QString`。
- **失效即回退**：配置值存在但已不是可用可执行文件（被移动/删除/指向目录）时记一条
  warning 并回退自动检测——宁可降级也不要把用户彻底锁死在打包功能外；UI 用
  `m3u8s.ffmpegPathIgnored` 明说"已改用自动检测"，不静默。
- **改完立刻复检**：`setM3u8sFfmpegPath()` 落盘 + 推给 packager + 立即重启能力探测，
  状态徽标和一次性警告对话框当场跟着变（不再要求重启应用）。探测有 in-flight 合并：
  探测期间再次改路径只记 `m_ffmpegReprobeRequested`，等上一次结果落地后再跑一次，
  避免两个子进程结果交错；复检过程中 `ffmpegCapabilityState` 回 `probing`，警告可见性
  保持原值以免闪烁。警告只对**未被确认过**的坏结果弹：用户关掉过一次后，反复试路径
  不会每次重现模态框，直到下次探测成功才重新布防。打包进行中禁止改路径（运行中的
  任务已经绑定了二进制），试图修改会记 warning 并弹错误提示，不静默失败。
- **QML 面**：`m3u8sFfmpegPath`（读写）、`ffmpegEffectivePath`（只读，上次探测真正用上
  的路径）、`chooseFfmpegExecutable()` / `clearFfmpegExecutablePath()` / `reprobeFfmpeg()`。
  M3U8S 页在状态徽标下方一行给出"选择 / 恢复自动检测 / 重新检测"与生效路径说明。

## 边界

- 探测本身只读：不下载、不安装；唯一会被这套流程写入的设置是用户手填的 FFmpeg 路径。
- 播放路径完全不经此处；警告关闭与否不影响除 M3U8S 打包外的一切功能。
- 打包真正启动时仍走原有 `start()` 错误处理（探测通过后用户又卸载 FFmpeg
  之类的时间窗由既有失败路径兜底）。
- 未来做捆绑固定版本（方案 A）时，本模块退化为对捆绑二进制的一次自检，
  接口无需变化。
