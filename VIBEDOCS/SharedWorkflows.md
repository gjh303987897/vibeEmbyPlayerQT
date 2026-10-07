# CLI / 桌面共享流程

## ServiceOperations

位置：`src/services/management/ServiceOperations.*`。
负责服务 ID、字段校验、卡片保存、IPTV 托管导入、删除和隐私 PIN 校验，
不依赖 QML、播放器或窗口。GUI 的卡片保存、删除、标识及 PIN 验证和 CLI
使用相同函数。ID 算法和旧版本保持一致，避免升级后重复卡片。

- `cardId(endpoint,type,user)`：SHA-256 类型/地址/用户。
- `iptvId(source)`：SHA-256 IPTV 源路径。
- `save(repository,server)`：校验 HTTP(S) 地址及账户字段后保存。
- `savePlaylist(repository,server,source,storageRoot,origin)`：解析、导入托管文件、
  设置频道所属播放列表及本地流指向，再保存 SQLite；可显式提供隔离目录和远程来源 URL。
- `remove(repository,id,deleteLocalData,deleteCredentials)`：CLI 使用默认参数，
  清理保存会话及系统密码，隐藏卡片，保留媒体/历史/TSSL。
  GUI 显式传原有“删除本地数据”选择，保留既有删除选项的含义。
- `verifyPrivacyPin(repository,pin)`：沿用 SHA-256 `salt:pin`，不建立新 PIN 格式。

返回 `std::expected`，不持有调用端状态。系统凭据操作沿用 CredentialStore。
IPTV 写入使用既有导入器、数据库事务及频道解析器，不把业务放到 QML。
迁移/持久配置由 Repository 负责；CLI 查询通过 `OpenMode::ReadOnly` 避开初始化。
`clearServerSession` 支持只清会话而保留媒体和历史。

## WorkflowSettings

位置：`src/services/management/WorkflowSettings.*`。
白名单快照包含下载目录、M3U8S 编码/容器/并行数/输出/FFmpeg/临时目录、
还原输出及格式、TSSL 备份目标配置、外观/历史/更新设置及 CLI 播放默认值；
没有 PIN、密码或会话 Token。

`snapshot()` 使用桌面相同的 QSettings 组织/应用名并同步读取；
`validate()` 校验枚举、并行范围、布尔值、路径、S3 地址；
`set()` 验证后同步保存；`applyPackaging()` 把快照映射到现有打包请求。

CLI 把临时参数应用到快照副本，只有 `config set` 保存配置。GUI 在启动
后台源扫描前冻结当前页面设置，然后也用 `applyPackaging` 构建请求，
防止扫描期间更改设置造成批次内行为不一致。

## WebDavUploadPlanner

位置：`src/services/webdav/WebDavUploadPlanner.*`。
`plan(source,target,canceled)` 在工作线程枚举文件系统，返回
`std::expected<std::vector<TransferManager::TaskRequest>,QString>`。

顺序为根 MKCOL、按层级排序的子目录 MKCOL、文件 PUT。目标表示完整最终
文件/目录；相对路径逐段 URL 编码，目录层级 `/` 不编码成文件名字符。
不跟随符号链接，包含隐藏项；扫描检查取消及可读性。无网络操作，不访问 UI。

桌面继续用原 TransferManager 的组任务及默认覆盖语义；
CLI 文件使用新增 `OverwritePolicy::Refuse`，在发送 PUT 时设置
`If-None-Match: *`。显式 `Allow` 与原默认行为一致。既有目录由 CLI 在允许
合并时重新 stat 确认是目录；不会把同名文件误判为已有目录。

下载沿用 `WebDavDownloadPlanner`，额外校验服务器资源的同源及目录范围；
CLI 各文件使用独立临时目录，在完成后无覆盖发布，取消会释放临时目录。
显式 --overwrite 使用 QSaveFile，--resume/后台任务保存独立检查点与暂存，
强 ETag 一致时采用 Range/If-Range；桌面调用保留默认的运行期队列行为。
下载规划失败时不会创建最终本地目录。

## TsslImportWorkflow

位置：`src/services/backup/TsslImportWorkflow.*`。
`run(store,files,canceled,progress)` 在工作线程逐项调用
`TsslStore::restoreFromFile`，统计 `restored/alreadyExists/failed/canceled/firstError`。
相同摘要跳过、损坏包继续处理，默认不覆盖已保存包。取消只在项边界生效，
已成功导入项保留。进度回调运行在调用它的工作线程，宿主不得直接修改 UI。

GUI 备份恢复后的本地导入和 CLI import/local restore/remote restore 共用此
流程。TsslBackupService 仍负责网络发现、签名、下载与临时文件生命周期；
导入完成后由宿主调用 `cleanupRestoreFiles()`，取消网络任务等待现有完成回调。

## 验证与依赖边界

`cmake/Cli.cmake` 把无 GUI 基础设施编译为 `vibeCore` 静态库；
桌面和 CLI 共用该库，CLI 不链接 Qt Quick/Widgets/libmpv。
Qt Core/Concurrent/Network/Sql/Xml 是核心依赖，Windows 凭据接口单独条件编译。
测试见 `CliTest.cpp`、`CliIntegrationTest.cpp` 及既有模块测试。
FFmpeg 短诊断失败的回归由 CLI 批量部分失败测试覆盖。

协议依据：[RFC 9110 条件请求](https://www.rfc-editor.org/rfc/rfc9110.html#section-13.1.2)、
[RFC 4918](https://www.rfc-editor.org/rfc/rfc4918.html)；
Qt 数据库打开依据：[QSqlDatabase](https://doc.qt.io/qt-6/qsqldatabase.html#setConnectOptions)。
