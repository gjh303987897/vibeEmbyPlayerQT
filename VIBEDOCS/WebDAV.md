# WebDAV 模块说明

## 模块边界

WebDAV 第一版拆成四层：

- `WebDavClient`：标准 WebDAV 请求与 XML 解析。
- `CredentialStore`：WebDAV 密码的系统凭据存储。
- `TransferManager`：运行期后台上传/下载队列。
- `AppViewModel`：服务卡片、目录导航、上传下载入口、播放入口的编排。

QML 只负责展示和交互，不直接发网络请求。

## 协议实现

当前使用标准 WebDAV 方法：

- `PROPFIND` + `Depth: 1`：列出当前目录。
- `PROPFIND` + `Depth: 0`：获取单个资源信息。
- `MKCOL`：创建远程目录。
- `GET`：下载文件。
- `PUT`：上传文件。

认证使用 Qt 的 `QNetworkAccessManager::authenticationRequired`，由 `QAuthenticator` 处理 Basic/Digest。

## 凭据策略

SQLite 不保存 WebDAV 密码。

当前策略：

- Windows：使用 Credential Manager。
- 其他平台：返回不可用，自动降级为每次输入密码。

后续如果增加 macOS/Linux 支持，应继续放在 `CredentialStore` 内，不要把平台代码泄漏到 ViewModel 或 QML。

## 传输任务

`TransferManager` 是运行期队列：

- 任务类型：上传、下载、远程建目录。
- 支持进度、状态、取消。
- 第一版不支持断点续传，不跨重启恢复。
- 文件夹上传会把远程目录创建和文件上传按队列执行。
- 文件夹下载由 `AppViewModel` 递归展开后入队。

## 播放

WebDAV 视频播放会把远程 URL 传给 libmpv。

需要认证时，`MpvVideoItem` 暴露临时 `httpUsername/httpPassword`，`PlayerController` 播放前设置 libmpv 的 `http-user/http-password`。

注意：

- 不要记录密码到日志。
- 离开播放时清空临时播放凭据。

## 下载安全检查

下载前会检查目标磁盘空间：

- 单文件使用 `getcontentlength`。
- 文件夹会递归 `PROPFIND` 估算总大小。
- 若空间不足，弹警告但允许继续。
- 若大小无法确认，弹警告但允许继续。

本地重名文件使用自动重命名，规则为：

- `file.ext`
- `file (1).ext`
- `file (2).ext`

文件夹同理。
