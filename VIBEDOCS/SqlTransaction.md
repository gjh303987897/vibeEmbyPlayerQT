# SQLite 事务清理

`src/database/SqlTransaction.h` 提供 `SqlTransaction`，供桌面和 CLI 共用的
`SessionRepository` 使用。它保存当前连接的一个事务，不创建连接，不跨线程，
不支持嵌套事务。

## 使用与失败处理

构造后调用 `begin()`，成功才会执行 `BEGIN IMMEDIATE` 后的业务操作。
所有退出路径由析构回滚；只有 `commit()` 成功才取消回滚。
两者使用 `std::expected<void, QString>` 返回数据库错误。

SQLite 的 `COMMIT` 遇到锁竞争时可以失败而保留事务。`commit()` 在失败时
立即回滚并返回原始提交错误。回滚本身失败时关闭连接，保证后续调用不会
沿用状态不明的事务；SQLite 已经自动回滚的错误也采用关闭连接的保守收尾。
仓库随后通过 `ensureOpen()` 重新打开连接。

仓库的数据库等待上限仍为 3000 ms。读命令的只读打开不执行迁移或清理。
多步服务删除在一个事务内完成，托管 IPTV 文件仅在提交成功后清理；数据库
事务不保证文件系统操作的原子性。

## 回归验证

`AuditRegressionTest::commitFailureLeavesConnectionUsable` 用另一个 SQLite
连接保留读事务，使提交失败，确认写入回滚且下一次事务成功。
`failedDeletionPreservesRelatedRows` 用触发器阻止最终服务删除，确认会话和
服务均保留。原有历史、IPTV、调度和 CLI 测试继续覆盖仓库调用。

依据：[SQLite 事务错误语义](https://www.sqlite.org/lang_transaction.html)。
