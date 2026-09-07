# NetworkTrafficCoalescer 模块说明

位置：`src/utils/NetworkTrafficCoalescer.{h,cpp}`

## 解决的问题

`QNetworkReply::readyRead` / `QTcpSocket::readyRead` 在大文件传输时每秒可触发
数十到上百次。历史实现每次回调都 `emit networkTrafficSample(...)`，导致：

- UI 线程被高频信号淹没（流量统计、历史页刷新等下游都在主线程）；
- 每行 UI 重排 / 重绘被间接放大，表现为下载或列表加载时 GPU 占用飙升。

## 模块职责

把「每次网络块」粒度的字节计数合并为「每 200ms 一次」的聚合样本：

```cpp
void record(serviceId, serviceName, serviceType, bytesReceived, bytesSent = 0);
void flush();                                  // 传输结束时立即冲刷
signals:
void flushed(serviceId, serviceName, serviceType, bytesReceived, bytesSent);
```

- 按 `serviceId` 分桶累加，定时器单次触发式（200ms），触发时冲刷全部桶。
- `record()` 对空 id / 非正字节数直接忽略。
- 对外部订阅者完全透明：宿主对象只需把自己的公开信号转发接过来：

```cpp
connect(&m_traffic, &NetworkTrafficCoalescer::flushed,
        this, &MyProxy::networkTrafficSample);
```

## 现有使用者

| 宿主 | 原始信号源 |
| --- | --- |
| `WebDavPlaybackProxy` | 播放代理逐块转发 |
| `TransferManager` | `updateProgress` 进度增量 |
| `EncryptedHlsPlaybackProxy` | `fetchRemoteBytes` / `fetchRemoteRange` 逐块 |

三者的 `networkTrafficSample` 对外签名保持不变，`AppViewModel` 消费端零改动。
`WebDavClient` 的 PROPFIND 每次请求只 emit 一次，无需合并。

## 使用边界

- 该组件只做「计数合并」，不做速率计算；速率仍由消费端根据样本间隔估算。
- 若未来加入 UI 实时速率表，注意 200ms 合并窗口即速率采样粒度；如需更细
  粒度应调整 `coalesceIntervalMs` 而不是回到逐块信号。
- 语义上样本不保证请求边界对齐（一个样本可能覆盖一个请求的尾部与下一个请
  求的头部），任何需要按请求精确归因的逻辑必须继续走请求级统计。

## 测试

`encrypted_hls_playback_proxy`、`transfer_manager` 目标均链接本模块作为回归
覆盖（构建通过 + 统计链路行为一致）。
