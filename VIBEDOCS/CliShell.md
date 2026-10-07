# CLI Shell 表达式和输入

CliShell 复用 Engine/Invocation/输出，保存服务、目录和会话解锁状态。
Windows 原生输入支持 Tab、左右/Home/End、退格/Delete 和上下历史；
其他平台保留普通输入，可调用 complete。重定向保持单一 UTF-8 流。

~~~text
list | select name type
list | where type WebDAV | count
config get appearance/themeMode | get appearance/themeMode
config set appearance/themeMode $PLAYER_THEME
config set appearance/themeMode $(config get appearance/themeMode | get appearance/themeMode)
use "家庭 DAV"
cd /电影
mkdir "新 文件夹" --parents
download "影片.mp4" "D:\下载\影片.mp4" &
task list
history
history --clear
complete "webdav m"
exit
~~~

环境变量支持 $NAME 和大括号形式；缺失时报错。单引号不展开，双引号展开。
展开结果保持字面参数，不产生新操作符。$(...) 使用 CLI data 值，最多嵌套 8 层。
内部管道过滤 JSON：select 选字段，where 比较值，get 提取对象字段，
count 返回数组/对象元素数；上游失败不继续过滤。末尾 & 等同 --background。
不执行外部 bash/PowerShell 命令。history 仅保留当前会话原始输入，
最多 1000 条，不把展开后的密码写盘。

--yes 不从启动参数继承。脚本普通错误继续执行，最终返回最后一次非零退出码；
--fail-fast 在首个错误退出。真实交互终端 exit 返回 0，取消后可继续输入。
Shell 不切换 profile、不嵌套启动宿主。
测试覆盖引号、展开/替换、管道、历史、补全候选、脚本退出码和原生 Ctrl+C。

服务名补全使用独立只读连接，仅查询卡片元数据，不调用 `service list`，
不读取 CredentialStore 或解密 Token。候选缓存 500 ms，连续 Tab 共用它；
服务/隐私命令结束后失效，隐私访问状态变化也立即失效。其他进程修改的卡片
最迟在缓存到期后的下一次补全中更新。文件路径候选仍按当前目录实时查询。
回归测试在数据库持有排他锁时连续补全，验证缓存不等锁；另验证外部重命名
及隐私锁定后不再返回私有服务名。
