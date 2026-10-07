# WebDavUploadPlanner

位置 `src/services/webdav/WebDavUploadPlanner.*`。纯规划器，输入本地源、
最终远程 URL 与取消标志，返回 TransferManager::TaskRequest 列表或错误。
完整边界见 [SharedWorkflows.md](SharedWorkflows.md#webdavuploadplanner)。

工作线程扫描，先根 MKCOL，再按父目录先于子目录排序，最后文件 PUT；
目录保留相对层级。每段单独百分号编码，已有根目录的编码保留，避免
中文、空格、#/% 被误当查询、片段或把整个层级编码成一个名字。
拒绝根符号链接，扫描不跟随子符号链接；每次枚举检查取消与可读性。
规划器不管理网络、Qt 对象或宿主任务状态。

GUI 把返回列表交给现有分组传输管理器；CLI 按条执行并等待完成，使用
显式 OverwritePolicy::Refuse 设置条件 PUT，Allow 保留原默认上传行为。
CLI 自己处理目录碰撞及允许合并时的目录类型确认。
取消保留已完成远程文件；规划失败不会开始上传。

验证：CliTest 检查目录层级、顺序、中文/特殊字符及取消；
CliIntegrationTest 检查递归 PUT、已有目标保护、检查之后的竞争写入 412、
允许覆盖、中文目录 stat 和下载往返。
协议：[RFC 9110 If-None-Match](https://www.rfc-editor.org/rfc/rfc9110.html#section-13.1.2)。
