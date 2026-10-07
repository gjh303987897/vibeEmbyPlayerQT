# BoundedReplyWriter

`src/services/webdav/BoundedReplyWriter.h` 将 QNetworkReply 的连续响应转发到
本机 QTcpSocket，供普通 WebDAV 播放代理使用。与加密代理的已认证完整分片
writer 不同，此组件不持有整个文件；加密分片必须先完成 GCM 认证才能输出。

接收缓冲通过 `setReadBufferSize` 限定为 256 KiB，socket 的应用层发送队列
也限制为 256 KiB，每次最多读取 64 KiB。`readyRead`、`bytesWritten`、
`finished` 都驱动 pump；发送队列满时停止从 reply 读数据，由 Qt 和 TCP
施加背压。内核、TLS 和 Qt 协议处理还有额外缓冲，256 KiB 不是整个进程的
内存上限。响应头不计入此组件的正文分块预算。

reply 完成时仍可能有尾部数据；必须继续等发送空间，排空后才关闭 socket
并释放 reply。下游断开立即 abort 上游。writer 随 reply 销毁，socket 用
QPointer 保护，不产生相互持有的 shared_ptr 回路。

普通播放代理的 30 秒计时器改为网络空闲检测，下载进度使其续期；下游
发送队列已满时延期检查，暂停播放不会被当成上游超时。请求头仍有独立
15 秒期限。GET 使用同源重定向策略，防止认证挑战凭据被带到不同来源。

`AuditRegressionTest::replyWriterBoundsSlowConsumerAndDrainsTail` 使用真实
本机网络响应和停止读取的客户端检查队列上限，然后恢复读取，核对完整
8 MiB 正文和 reply 清理。该测试不证明真实公网吞吐或内核内存总量。

依据：[Qt QNetworkReply 读缓冲](https://doc.qt.io/qt-6/qnetworkreply.html#setReadBufferSize)、
[Qt QAbstractSocket 发送队列](https://doc.qt.io/qt-6/qabstractsocket.html#bytesToWrite)。
