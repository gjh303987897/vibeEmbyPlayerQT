# TsslImportWorkflow

位置 `src/services/backup/TsslImportWorkflow.*`。输入 TsslStore 值快照及路径
列表，在调用线程顺序执行已有安全导入。接口与结构见
[SharedWorkflows.md](SharedWorkflows.md#tsslimportworkflow)。

宿主在 QtConcurrent 工作线程调用 run，不阻塞 GUI/Core 事件循环。
每个包由原解析器验证并由存储模块保存；重复根摘要按
packageAlreadyExistsError 统计，损坏/不可读包统计失败并继续。
统计只包含数量、取消标记和首个错误，不包含密钥。

取消在每个导入项开始前检查；导入成功项不回滚。进度回调来自工作线程，
只能由宿主通过线程安全方式显示，不可直接访问 QML。
GUI 备份恢复与 CLI import/restore 使用同一流程；远程临时目录由
TsslBackupService 持有，CLI 完成导入后释放，不删除远程源。

验证：集成测试一次导入有效、重复和损坏包，检查 1/1/1 统计及退出 6；
分别通过本地、WebDAV、S3 备份，移除本地包后恢复；现有备份模块测试覆盖
请求取消、解析取消与临时文件生命周期。
