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

## 兼容性

- `.m3u8s` 目录格式继续支持，旧 TSSL v2/v3 不变。
- `.m3u8sp` 是新的只读、不可变格式；修改视频必须重新打包。
- TAR 不使用 gzip 或 zstd，避免破坏远程 Range 随机读取。
- 使用 POSIX/PAX 以保持跨平台 TAR 工具的列出和解包能力。

标准 TAR 工具可以列出和提取成员，但没有外部 TSSL 时不能播放加密 TS。
