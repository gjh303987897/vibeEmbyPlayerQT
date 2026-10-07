# WorkflowSettings

位置 `src/services/management/WorkflowSettings.*`。提供业务配置的白名单
快照、验证、保存和打包参数映射，接口见
[SharedWorkflows.md](SharedWorkflows.md#workflowsettings)。

正常模式显式使用 QSettings 的 NativeFormat 与桌面组织/应用名；
隔离 profile 使用 IniFormat、UserScope 及指定根目录，禁用系统回退。
带组织名的 QSettings 构造函数忽略 setDefaultFormat，因此必须显式传 format。
依据：[Qt QSettings 构造与格式](https://doc.qt.io/qt-6/qsettings.html)。

快照包含下载、打包、还原、备份及现有外观/历史/更新配置，
另有 CLI 宿主的 player/volume、player/speed、player/subtitleFile；
不包含 PIN、Token、密码或 S3 secret。
验证拒绝未注册键、非法枚举和并行数，转换布尔/CSV 编码列表，规范化本地路径；
保存同步检查 QSettings 状态。配置不会在临时命令覆盖时写回。

GUI 在后台发现视频之前冻结当前界面值；CLI 从快照构造请求并应用覆盖；
二者调用同一个 applyPackaging，避免编码/容器规则随入口漂移。
FFmpeg 的进程级路径仍由现有定位器管理。

验证：CliTest 检查类型/取值；集成测试检查配置持久化、临时视频覆盖不持久化、
隔离 INI 的实际创建，并比较原生配置，防止测试写入用户配置。

音量为 0..100 整数，倍速为 PlayerController 支持的 0.5..5；
字幕默认路径必须指向现有文件，保存绝对路径，空字符串清除默认字幕。
卡片大小为 50..150，历史保留为 1..3650 天。布局为 trendy/traditional，
更新渠道 stable/beta。外观在桌面下次读取时生效；player 默认值在下一次
CLI play 读取，不修改已运行的桌面播放器。
