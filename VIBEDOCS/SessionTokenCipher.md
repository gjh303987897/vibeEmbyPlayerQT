# 会话 Token 安全存储

`src/database/SessionTokenCipher.h/.cpp` 是 `SessionRepository` 的存储边界。
桌面和 CLI 共用它；服务客户端收到的仍是正常内存 Token，输出和日志不包含
Token。WebDAV/SMB 密码及 S3 Secret 共用
[CredentialStore 原生后端](NativeCredentialStore.md)。

## 格式与账户范围

Windows 使用用户范围的 DPAPI，禁止系统交互，不启用机器范围加密。
SQLite `sessions.access_token` 保存 `dpapi:v1:` 前缀和 Base64 密文。
保护或解密失败返回 `std::expected` 错误；保存失败不会降级为明文。
损坏密文、换用 Windows 账户或无法解密时，需要重新登录。

macOS/Linux 默认把 Token 保存到系统凭据库，SQLite 只保存
`keychain:v1:<规范 UUID>` 引用。对应凭据键为
`vibePlayerQT/Secret/Session/<UUID>`。macOS 使用 login Keychain，Linux 使用
libsecret/Secret Service。缺失或锁定后端、保存或读取失败均报错，不降级为
明文写入；引用损坏或对应凭据缺失时要求重新登录。

`Protection::SystemCredentials` 也可在 Windows 选择 Credential Manager；
默认仍为 DPAPI。回归测试用此安全策略验证共享引用、事务及清理逻辑，不
代表已验证非 Windows 原生 API。系统凭据引用也依赖原账户和凭据库；单独
复制数据库不包含凭据，删除原会话可能让数据库副本失去会话，需重新登录。

DPAPI 不保护正在运行的本账户进程内存，也不阻止本账户使用 DPAPI 解密。
它保护磁盘上的 Token 明文。直接复制数据库到其他账户或机器不保证会话
可用，应重新登录。历史备份、外部副本和仍被其他进程持有的旧数据库页不由
本模块清除。

## 旧数据迁移与清除

写模式初始化先迁移表结构，并对有 `id` 的重复会话按 `created_at`、`id`
保留最新行。先完成系统凭据 I/O，再在事务中更新旧明文 Token；更新条件
包含旧值，避免覆盖另一进程刚保存的新会话。所有平台的写连接启用
`PRAGMA secure_delete=ON`，
迁移更新和会话删除清理 SQLite 当前文件中的旧单元内容。
重复身份清理及加密操作失败时均报告错误，不丢弃未成功迁移的会话。

只读命令不写数据库、不迁移；读取旧明文会话用于向后兼容，后续写模式
初始化完成保护。DPAPI 密文随行删除。原生凭据使用
`session_credential_cleanup` 表和删除/更新触发器记录待清理引用，SQL 提交后
确认当前数据库已无会话引用它，再删除系统凭据；去重同样清理被移除行。
隐藏卡片保留会话，真正删除卡片或退出登录才清理。

保存/迁移回滚会删除本次新凭据，保留原会话及凭据。清理失败保留重试记录，
下次写模式初始化重试；错误明确说明数据库修改已完成，不能误认为 SQL
已回滚。仍被当前数据库引用的凭据不会被清理。

系统凭据和 SQLite 不共享原子事务：若进程在创建凭据后、记录引用或清理
日志前被强制终止，可能留下无引用的系统凭据；它仍受凭据库保护，可从
系统凭据管理界面清理。正常错误/事务回滚有清理流程，不承诺跨存储的崩溃
原子性。历史备份与外部数据库副本中的旧明文不由此模块擦除。

## 平台边界与验证

本地验收环境为 Windows。macOS Keychain/Linux Secret Service 已实现并
接入 CI，但未在本机进行这两个平台的编译和运行验收，不声称已全平台测试
通过。在其他平台读取 Windows 密文会明确报错。

`AuditRegressionTest::sessionMigrationAndEncryption` 验证带 `id` 的旧表重复
身份迁移、最新 Token 保留、数据库中无测试明文、只读解密及会话清除。
`SessionCredentialsTest` 覆盖引用往返、损坏/缺失凭据、替换和删除、失败保存
回滚、保护失败拒绝写入、迁移全批回滚与清理队列重试。Windows 本地为
**9 passed，0 failed，0 skipped**。Linux CI 另有无可用 D-Bus 的子进程测试，
要求保护失败并拒绝明文保存。
CryptoAPI 通过 Windows 自带的 `crypt32` 链接，不增加需要安装的运行库。

官方依据：[CryptProtectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)、
[CryptUnprotectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptunprotectdata)。
