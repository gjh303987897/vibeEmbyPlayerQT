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
   `EncryptedHlsPackager::locateFfmpegExecutable()`，bundle-first + PATH；探针模块
   与打包栈零链接依赖，便于单测）。
2. `ffmpeg -version` 解析版本号，支持 `7.1.1`、`7.1-essentials_build-...`、
   `n6.1.1-3-g...`；`git-2024-05-01-...` 日期构建解析失败时**跳过版本门槛**，
   交给能力扫描兜底。
3. 主版本下限 **5.0**（打包管线使用的 HLS flags 与 force_key_frames 行为自 5.x
   起稳定）。
4. `-encoders` / `-muxers` 表扫描：要求 `libx264`、`libx265`、`aac` 编码器与
   `hls` muxer。任何一项缺失 → `Incompatible` 并列出缺失项。

结果三种状态：`available` / `unavailable` / `incompatible`，`detail` 为单行
人读摘要（写入日志，字段报告可带出用户 FFmpeg 指纹）。

纯函数 `parseVersionBanner` / `missingRequiredFeatures` 直接暴露给单测
（`tests/FfmpegCapabilityTest.cpp`）。

## 与 UI 的接线（AppViewModel / Main.qml）

- `AppViewModel::initialize()` 尾部延迟 800ms 经 `QtConcurrent` 启动探测
  （子进程绝不进 GUI 线程；也避开首帧窗口抖动）。
- QML 属性：`ffmpegCapabilityState`（含 `probing` 过渡态）、
  `m3u8sFfmpegAvailable`（= usable，M3U8S 页状态徽标与开始按钮沿用）、
  `ffmpegCapabilityDetail`、`ffmpegWarningVisible`。
- `ffmpegWarningDialog`（ModernDialog，警告色感叹徽章 + 等宽 detail + 要求
  说明 + 官网下载按钮），`onFfmpegCapabilityChanged` 驱动打开；关闭只影响
  本次会话。
- M3U8S 页状态徽标四态：检测中 / 已就绪 / 不完整 / 未找到。

## 边界

- 探测只读不改：不下载、不安装、不写任何设置。
- 播放路径完全不经此处；警告关闭与否不影响除 M3U8S 打包外的一切功能。
- 打包真正启动时仍走原有 `start()` 错误处理（探测通过后用户又卸载 FFmpeg
  之类的时间窗由既有失败路径兜底）。
- 未来做捆绑固定版本（方案 A）时，本模块退化为对捆绑二进制的一次自检，
  接口无需变化。
