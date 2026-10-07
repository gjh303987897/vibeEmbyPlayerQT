# ServiceOperations

本模块共享服务身份、配置保存、IPTV 导入、删除及 PIN 校验，位置
`src/services/management/ServiceOperations.*`。调用与返回结构详见
[SharedWorkflows.md](SharedWorkflows.md#serviceoperations)。

服务身份沿用历史 SHA-256 算法；地址或账户变更产生新 ID，名称/开关更新保留
原 ID。CLI 名称匹配先检查完整 ID，再检查唯一名称；隐私过滤在解析服务之前
完成。GUI 与 CLI 均调用同一保存和 PIN 流程，网络认证仍由现有客户端负责。

IPTV 导入复用 IptvParser/IptvPlaylistStore：成功解析后复制到托管目录，更新
频道 playlistId，并将指向源文件的本地流改为托管文件。元数据保持
`LocalFile`、卡片名、原始路径、导入时间，SQLite 使用原事务写入。
可指定 storageRoot 供隔离 profile 使用，默认与桌面存储相同。
可选 origin 参数支持在线 IPTV 列表，存储 RemoteURL 和原 URL，
相对频道/台标地址以该 URL 解析。CLI 只改名称/开关时保留已有频道和来源，
不把托管文件重新当成本地列表解析。

删除参数明确区分本地数据和凭据。CLI 移除会话/密码并隐藏卡片，保留历史、
媒体、TSSL；桌面传入原删除选项。失败返回可追踪的 `std::expected`。
PIN 只校验，不创建新 PIN、不记录输入；解锁状态由宿主会话持有。

SMB 配置使用 smb://host/share/root，凭据仍交给安全存储。
验证：CLI 集成测试覆盖媒体/WebDAV/IPTV 卡片、同名、更新字段、登录/检查/移除及同时打开
数据库；CliTest 覆盖 ID 无法绕过隐私过滤、错误 PIN 和会话范围。
