# Windows SMB 后端

SmbClient 封装 smb://host/share/root 到 UNC、WNetAddConnection2W 临时认证、
MoveFileExW 移动。Windows redirector 执行协议与 SMB2/SMB3 协商，遵循系统策略。
不自制 SMB，不创建盘符/永久映射，不自动断开用户已有连接。

~~~powershell
.\vibePlayerQT service add --type smb --name NAS --url "smb://nas/videos" --username "DOMAIN\user"
.\vibePlayerQT service login NAS --password-env NAS_PASSWORD
.\vibePlayerQT smb ls NAS /
.\vibePlayerQT smb mkdir NAS /电影/新目录 --parents
.\vibePlayerQT smb upload NAS "D:\视频\合集" /电影/合集
.\vibePlayerQT smb download NAS /电影/合集 "D:\下载\合集"
.\vibePlayerQT smb copy NAS /电影/a.mp4 /电影/b.mp4
.\vibePlayerQT smb move NAS /电影/b.mp4 /电影/c.mp4 --overwrite
.\vibePlayerQT smb remove NAS /电影/c.mp4 --yes
.\vibePlayerQT play NAS /电影/a.mp4
~~~

虚拟根目录为 share/root，.. 不得逃出。复制不跟随链接，默认同名冲突，
路径检查拒绝 Windows 设备名、非法字符、末尾点/空格及链接/junction 祖先；
递归删除前检查整棵目录中是否存在链接，拒绝服务根目录。
新文件通过独立暂存/不覆盖重命名发布。--overwrite 使用 QSaveFile；
move 替换文件，不合并/替换目录。remove 拒绝根目录。
认证/递归文件操作在后台线程，逐块检查取消；密码复用 CredentialStore。
失败保留 Windows 错误码，如 1219 表示已有不兼容认证，不自动删除系统连接。
Windows 原生连接请求由系统完成；连接期间取消会等待其返回，然后停止后续操作。
直接 SMB URL 当前在 Windows 实现；其他平台需挂载共享，再用本地文件命令。
CLI 添加 SMB 不代表新增桌面浏览页面。真实 NAS 权限/协商需要实际服务器验收。
upload/download 可加 --background，由 CLI 任务宿主管理暂停/恢复/取消。
重启后依据完整文件摘要跳过已完成项，当前文件重新复制；
接口与恢复规则见 [CliBackgroundTasks.md](CliBackgroundTasks.md)。
依据：[WNetAddConnection2W](https://learn.microsoft.com/en-us/windows/win32/api/winnetwk/nf-winnetwk-wnetaddconnection2w)、
[MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)。
