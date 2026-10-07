# 原生系统凭据库

`CredentialStore` 是桌面与 CLI 共用的 `std::expected` 接口。
`NativeCredentialStore.h/.cpp` 封装非 Windows 实现；会话引用协议与清理见
[SessionTokenCipher](SessionTokenCipher.md)。

## 保存位置与依赖

| 平台 | 保存位置 | 编译/运行要求 |
| --- | --- | --- |
| Windows | 本账户 Credential Manager；会话默认使用用户 DPAPI | 系统 Advapi32、Crypt32；未增加 Windows 运行库 |
| macOS | 本账户 login Keychain 的 Generic Password 项 | 系统 Security、CoreFoundation framework |
| Linux | Secret Service 默认 collection | 开发包 `libsecret-1-dev`、pkg-config；运行库 `libsecret-1-0`、用户会话 D-Bus 和已解锁的兼容凭据服务 |

目标分别为 `vibePlayerQT/WebDAV/<服务 ID>` 和 `vibePlayerQT/Secret/<键>`。
后者包含会话、S3 和 CLI 后台任务。密码/Token 不写入 QSettings 或明文文件；
空密码允许，含 NUL 的值拒绝，避免原生字符串接口截断。
`isAvailable()` 表示平台具备实现，实际读写仍可能因为锁定、访问拒绝、缺失
服务或 I/O 故障失败，调用者必须检查返回值。

## macOS

复用 `SecItemAdd/Update/CopyMatching/Delete`。查询禁止认证弹窗；需要交互
时明确报错，用户在 Keychain Access 解锁或授权后重试。
新条目的受信列表仅包含当前程序和同一安装目录的 `vibePlayerQT`、
`vibePlayerQT-cli`、`vibePlayerPlayback`；应用 bundle 按 bundle 路径建立
受信对象，允许产品入口共享凭据，不授予所有应用访问权限。

访问列表使用经典 login Keychain 的 `SecAccess`，未设置 Data Protection
Keychain 或 synchronizable 属性。更新后代码身份变化可能要求重新授权；
未增加 Developer ID 签名/公证。原生 I/O 在独立线程池执行；等待期间处理
Qt 事件，并禁止新的鼠标/键盘输入重入当前操作。

## Linux

直接调用 libsecret，不自行实现 D-Bus 密钥协议。
schema 为 `org.vibePlayerQT.Credentials`，检索属性 `target` 不包含密码/Token。
读取和删除前搜索匹配项，锁定项报错；删除后再次确认无剩余项，避免
`clear_sync` 只清理部分解锁项就报告成功。

同步 API 在独立线程池执行，每次操作设置 15 秒 `GCancellable` 取消期限。
返回通用错误，不输出可能包含私有元数据的 GError 文本，不提供明文 fallback。
保护程度依赖系统凭据库配置和账户权限，不能防止同账户已获权限的进程
读取已解锁凭据库。

无图形登录会话的脚本环境须提供已解锁的 Secret Service。临时密码可用于
不要求持久保存的操作；保存登录或后台任务仍需可用凭据库。Flatpak 已授予
`org.freedesktop.secrets` 的 D-Bus 访问。

## CI 与验证边界

Linux 构建安装 libsecret、D-Bus 和 GNOME Keyring。
`scripts/test-with-system-credentials.sh` 创建隔离 D-Bus/临时 XDG keyring，
仅保存测试凭据；macOS 使用临时 keychain，退出后恢复原默认/搜索列表并删除
测试 keychain。Windows 使用随机测试目标与现有安全后端。

本机只完成 Windows 编译与凭据测试。macOS/Linux 实现与 CI 已接入，对应
原生编译、权限边界和真实 provider 仍须在该平台验收；Windows 共享引用
逻辑测试不能代替这些结果。

## 官方依据

- [Apple SecItemCopyMatching](https://developer.apple.com/documentation/security/secitemcopymatching(_:_:))、[kSecUseAuthenticationUIFail](https://developer.apple.com/documentation/security/ksecuseauthenticationuifail)。
- [Apple SecAccessCreate](https://developer.apple.com/documentation/security/secaccesscreate(_:_:_:))、[kSecAttrAccess](https://developer.apple.com/documentation/security/ksecattraccess)。
- [libsecret password_store_sync](https://gnome.pages.gitlab.gnome.org/libsecret/func.password_store_sync.html)、[Service.search_sync](https://gnome.pages.gitlab.gnome.org/libsecret/method.Service.search_sync.html)。
- [GCancellable.cancel](https://docs.gtk.org/gio/method.Cancellable.cancel.html)：官方确认跨线程取消安全。
- [GNOME Keyring daemon](https://wiki.gnome.org/Projects/GnomeKeyring/RunningDaemon)：登录、初始化与环境设置。
