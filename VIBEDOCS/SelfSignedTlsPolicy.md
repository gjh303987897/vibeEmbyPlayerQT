# 自签名 TLS 错误处理

`src/network/SelfSignedTlsPolicy.h` 是 Qt HTTP 客户端共用的证书异常策略。
NetworkClient、WebDAV 浏览/普通播放/加密播放/传输，以及 TSSL WebDAV/S3
备份都使用它。只有用户已启用自签名信任且错误列表非空、全部属于
`SelfSignedCertificate` 或 `SelfSignedCertificateInChain` 时，才把含有原始
证书的错误列表传给 `ignoreSslErrors(errors)`。

主机名不匹配、过期、尚未生效、吊销、签名错误、缺少证书和其他未知错误
仍拒绝连接。不会将“信任自签名”解释为“关闭 TLS 验证”。这项布尔设置仍然
是信任策略，不是证书指纹固定；TOFU/固定指纹尚未实现。无法提供可验证
链的私有 CA 应正确安装到系统信任库。

桌面和 CLI 播放启用自签名信任的 Emby/Jellyfin HTTPS 视频时，复用
`WebDavPlaybackProxy` 转发原始视频流，让 Qt 执行上述验证。媒体 API 的
`api_key` 只存在于代理内部的上游 URL；本地地址只包含随机会话 ID。
代理继续转发 Range/HEAD，保留有界缓冲、超时和同源重定向规则。媒体登录
用户名不作为 HTTP Basic 凭据传递。播放结束或换片时撤销代理会话。

libmpv 的 `tls-verify` 始终为 `yes`；旧 `allowInsecureTls=true` 调用会明确
报错，不能通过关闭证书验证绕过代理。普通可信 HTTPS 视频继续直连。
这覆盖当前原始文件播放路径，不提供新的服务端转码或 HLS 回退功能。

系统 CA bundle 每次初始化 libmpv 时从 Qt 系统证书导出，写入本进程随机
临时目录，进程退出删除。生成失败可重试，不复用磁盘上的旧持久化缓存。
这不隔离拥有本账户完整权限的恶意进程，也不强制操作系统即时刷新自身缓存。

`AuditRegressionTest::selfSignedTrustDoesNotWaiveOtherTlsChecks` 覆盖禁用策略、
空列表和自签名与其他错误混合的情况。真实 loopback HTTPS 测试验证：
禁用时拒绝自签名证书、启用时接受有效 localhost 证书、错误域名和过期证书
始终拒绝，并验证 Range、Token 查询参数及无 Basic 头。测试 PEM 和公开的
测试私钥位于 `tests/data/tls`，不得用于部署。

依据：[Qt SSL 错误枚举](https://doc.qt.io/qt-6/qsslerror.html)、
[按错误接受证书](https://doc.qt.io/qt-6/qnetworkreply.html#ignoreSslErrors-1)。
