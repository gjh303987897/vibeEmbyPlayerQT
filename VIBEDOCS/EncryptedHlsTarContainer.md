# M3U8SP TAR 容器

`.m3u8sp` 是加密 HLS 的单文件容器。它解决的是文件数量和备份管理问题，不替代现有的 AES-256-GCM 加密和 TSSL 密钥存储。

## 文件布局

容器使用 POSIX/PAX TAR，TAR 的每个成员由 512 字节头、内容和 512 字节对齐填充组成，末尾使用两个零块。第一个成员固定为 `.vibe/index.cbor`，随后是 `index.m3u8s`、加密 TS、字幕和其他资源。

索引记录逻辑路径、TAR 头偏移、数据偏移、大小和 SHA-256。播放代理先读取索引，再直接定位成员，不能通过扫描整个 TAR 查找片段。

只允许普通文件。绝对路径、`..`、重复路径、符号链接、硬链接和设备文件都被拒绝。索引和成员都有大小上限，所有偏移都进行非负和溢出检查。

## 加密和 TSSL

每个 TS 仍然独立使用 AES-256-GCM 加密，解密成功且 GCM Tag 验证通过后才会把明文交给 libmpv。TSSL 不放入 TAR。

`.m3u8sp` 使用 TSSL v4。v4 额外记录：

- `containerFormat`: `m3u8sp-tar-index-v1`
- `containerIndexSha256`: 索引摘要
- `containerLength`: 容器长度

这三项与根播放列表摘要、4096 字符识别码一起验证，防止把索引替换为另一个容器的索引。

## 播放

本地播放从容器读取索引和成员；WebDAV 播放使用 HTTP Range 请求读取索引、manifest 和 TS。远程返回 `200` 整个对象而不是 `206 Partial Content` 时，`.m3u8sp` 播放会被拒绝，不会把整个视频加载到内存。

容器通过现有回环 HTTP 代理提供给 libmpv，libmpv 不直接解析 TAR，也不会接触 TSSL 密钥。

### 播放内存管理

- 本地索引先读取 512 字节 TAR 头，再按头内已校验的长度读取 CBOR，避免每次打开都分配 16 MiB 的固定探测窗口。
- 容器成员的读取和 SHA-256 校验在工作线程执行；任务仅捕获已验证的成员信息，不复制完整索引，也不持有代理对象。
- 播放分片使用 `AesGcmDecryptor::decryptTsSegmentInPlace` 接管密文缓冲，在原分配内解密。完整 GCM tag 验证通过之前不会发送任何明文；验证失败会清零暂存结果。
- 单次异步结果使用 `QFuture::takeResult()` 转移所有权，避免隐式共享的结果在清零时复制整个分片。
- HTTP 响应按 64 KiB 分块发送，Qt socket 待写队列上限为 256 KiB。慢速读取时保留一份已验证分片；发送完成、断开连接或撤销会话时清零并释放明文。
- 每条连接只处理一个请求，撤销会话会关闭该会话的连接，防止旧播放继续保留发送缓冲。

本地 `.m3u8s` / `.m3u8sp` 的来源信息由 `AppViewModel.localEncryptedPlayback` 传给 `MpvVideoItem` 和 `PlayerController`。libmpv 的单文件选项将前向缓存限制为 64 MiB、回看缓存限制为 8 MiB、预读限制为 8 秒，并关闭前向空间向回看缓存的转移。换片后选项自动恢复，WebDAV 等远程来源使用原有网络缓存策略。该限制不包含解码器、视频输出和当前正在认证的完整分片，因此不是应用总内存上限。

实现依据：[Qt QFuture](https://doc.qt.io/qt-6/qfuture.html#takeResult)、[Qt socket 写缓冲](https://doc.qt.io/qt-6/qabstractsocket.html)、[OpenSSL EVP 原地解密](https://docs.openssl.org/3.0/man3/EVP_EncryptInit/)、[mpv 缓存与单文件选项](https://mpv.io/manual/stable/)。

`EncryptedHlsFormatTest` 验证原地解密复用分配、共享数据不会被改写、错误 key/tag 和截断数据被拒绝。`EncryptedHlsPlaybackProxyTest` 验证 32 MiB 大分片的慢速读取、完整摘要、Range/HEAD、会话撤销，以及实际 libmpv 的普通本地 HLS / 加密容器播放、跳转、字幕加载、音轨切换和切换为模拟 WebDAV Range 来源后的缓存恢复；实际播放测试需要 PATH 中有 FFmpeg 来生成临时素材。

2026-09-30 本地对比：Windows 11、Qt 6.7.3、clang-cl Debug，使用相同测试及 Qt/libmpv 依赖，对比 `c4e50d7` 的代理/解密/容器代码与本次修改。32 MiB + 7 字节分片通过回环代理发送，客户端读取缓冲限制为 64 KiB，先等待 1 秒再读取；随后验证 Range、HEAD 和撤销会话。以 20 ms 间隔采样测试进程的私有内存，峰值从 146.29 MiB 降至 82.68 MiB（约 43.5%）；峰值工作集从 160.57 MiB 降至 97.05 MiB。数字包含素材生成和测试开销，仅证明该传输场景的内存改善，不代表实际 4K 播放进程的总内存降幅。

## 兼容性

- `.m3u8s` 目录格式继续支持，旧 TSSL v2/v3 不变。
- `.m3u8sp` 是新的只读、不可变格式；修改视频必须重新打包。
- TAR 不使用 gzip 或 zstd，避免破坏远程 Range 随机读取。
- 使用 POSIX/PAX 以保持跨平台 TAR 工具的列出和解包能力。

标准 TAR 工具可以列出和提取成员，但没有外部 TSSL 时不能播放加密 TS。

`EncryptedHlsTarContainerTest` 用系统 `tar -tf` 做跨工具兼容性校验时，会把工作目录设到归档所在目录并只传文件名：
git-bash 的 PATH 上是 MSYS 版 GNU tar，它会把 `D:/path/x.tar` 解析成"user D: 于 host path"并以 128 退出，
变成一个只有本机才会红的假失败（Windows 干净 PATH 下是 System32 的 bsdtar，读取正常）。
