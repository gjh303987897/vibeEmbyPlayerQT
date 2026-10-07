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

该模块不控制 libmpv 自己发出的 HTTP 请求。桌面媒体服务器直连播放的
`allowInsecureTls` 仍需单独处理，不能据此宣称所有播放入口都遵守本策略。

系统 CA bundle 每次初始化 libmpv 时从 Qt 系统证书导出，写入本进程随机
临时目录，进程退出删除。生成失败可重试，不复用磁盘上的旧持久化缓存。
这不隔离拥有本账户完整权限的恶意进程，也不强制操作系统即时刷新自身缓存。

`AuditRegressionTest::selfSignedTrustDoesNotWaiveOtherTlsChecks` 覆盖禁用策略、
空列表和自签名与其他错误混合的情况。真实自签名证书连接测试仍需补充。

依据：[Qt SSL 错误枚举](https://doc.qt.io/qt-6/qsslerror.html)、
[按错误接受证书](https://doc.qt.io/qt-6/qnetworkreply.html#ignoreSslErrors-1)。
