# Windows 会话 Token 加密

`src/database/SessionTokenCipher.h` 是 `SessionRepository` 的存储边界。
桌面和 CLI 共用它；服务客户端收到的仍是正常内存 Token，输出和日志不包含
Token。WebDAV/SMB 密码及 S3 Secret 继续使用原有 `CredentialStore`。

## 格式与账户范围

Windows 使用用户范围的 DPAPI，禁止系统交互，不启用机器范围加密。
SQLite `sessions.access_token` 保存 `dpapi:v1:` 前缀和 Base64 密文。
保护或解密失败返回 `std::expected` 错误；保存失败不会降级为明文。
损坏密文、换用 Windows 账户或无法解密时，需要重新登录。

DPAPI 不保护正在运行的本账户进程内存，也不阻止本账户使用 DPAPI 解密。
它保护磁盘上的 Token 明文。直接复制数据库到其他账户或机器不保证会话
可用，应重新登录。历史备份、外部副本和仍被其他进程持有的旧数据库页不由
本模块清除。

## 旧数据迁移与清除

写模式初始化先迁移表结构，并对有 `id` 的重复会话按 `created_at`、`id`
保留最新行，再在事务中保护旧明文 Token。连接启用 `PRAGMA secure_delete=ON`，
迁移更新和会话删除清理 SQLite 当前文件中的旧单元内容。
重复身份清理及加密操作失败时均报告错误，不丢弃未成功迁移的会话。

只读命令不写数据库、不迁移；读取旧明文会话用于向后兼容，后续写模式
初始化完成加密。新版本删除会话不再需要清理额外的凭据对象，密文随行删除。

## 平台边界与验证

本次按 Windows 环境交付。其他平台保持既有存储行为；macOS Keychain 和
Linux Secret Service 尚未实现，不能将此改动描述为全平台 Token 加密。
在其他平台读取 Windows 密文会返回明确错误。

`AuditRegressionTest::sessionMigrationAndEncryption` 验证带 `id` 的旧表重复
身份迁移、最新 Token 保留、Windows 数据库中无测试明文、只读解密及会话清除。
CryptoAPI 通过 Windows 自带的 `crypt32` 链接，不增加需要安装的运行库。

官方依据：[CryptProtectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)、
[CryptUnprotectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptunprotectdata)。
