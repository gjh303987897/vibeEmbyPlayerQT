# rclone OneDrive + Caddy 部署手册

## 适用范围

本文用于在一台新的 Debian/Ubuntu 服务器上部署以下服务：

- 使用 rclone 访问 OneDrive；
- 将 `onedrive:` 挂载到 `/mnt/onedrive`；
- 使用 `rclone serve webdav` 提供本机 WebDAV 服务；
- 使用 Caddy 提供公网 HTTPS 和反向代理；
- 支持数百 MiB 到数 GiB 的 WebDAV 大文件上传。

本文只使用占位符，不包含真实主机地址、用户名、密码、OAuth token、Drive ID 或证书私钥。部署前请将 `<WEBDAV_HOST>`、`<WEBDAV_USER>` 等占位符替换为目标环境的值。

本文配置规则基于 2026-08-31 查阅的 rclone 与 Caddy 官方文档。部署时应记录实际安装版本，并在升级后重新执行回归测试。

## 架构

```text
WebDAV 客户端
    |
    | HTTPS 443
    v
Caddy
    |
    | HTTP 127.0.0.1:8100
    v
rclone serve webdav
    |
    v
OneDrive

onedrive: ---- rclone mount ----> /mnt/onedrive
```

部署必须遵守以下规则：

1. rclone WebDAV 只监听 `127.0.0.1`，不能直接暴露到公网。
2. 公网只开放 Caddy 使用的 HTTP/HTTPS 端口。
3. 挂载服务和 WebDAV 服务必须使用不同的 VFS 缓存目录。
4. OneDrive 大文件必须显式启用分块上传。
5. WebDAV 密码使用 bcrypt `htpasswd` 文件保存，不能写入 systemd unit、Caddyfile、脚本或 Git。
6. Caddy 不配置请求体缓存，也不设置小于业务文件大小的请求体上限。
7. HTTP `201 Created` 只表示 WebDAV 请求已被接受；使用 VFS 写缓存时，还要等待 rclone 将文件提交到 OneDrive。

## 关键参数

推荐从以下保守配置开始：

```text
--onedrive-upload-cutoff=4Mi
--onedrive-chunk-size=10Mi
--transfers=1
--timeout=10m
```

rclone 官方约束如下：

- `--onedrive-upload-cutoff` 决定超过多大后改用分块上传；该参数默认关闭。
- `--onedrive-chunk-size` 必须是 320 KiB（327,680 字节）的整数倍。
- OneDrive chunk 不应超过 250 MiB，否则可能被 Microsoft API 拒绝。
- chunk 会缓存在内存中；提高 chunk size 或并发数会直接增加内存压力。
- `10Mi` 是 320 KiB 的 32 倍，满足限制，也是 rclone 当前默认 chunk size。

先使用 `--transfers=1` 验证稳定性。只有在持续监控内存、磁盘缓存和 OneDrive 限流后，才逐步增加并发数。

## 1. 准备 DNS、端口和磁盘

部署前确认：

- `<WEBDAV_HOST>` 的 DNS A/AAAA 记录指向新服务器；
- TCP 80 和 443 可从公网访问，以便 Caddy 自动申请和续期证书；
- TCP 8100 不对公网开放；
- 缓存磁盘可用空间大于“最大单文件大小 × 同时上传数”，并留有安全余量；
- 服务器时间与时区正常。

检查端口和磁盘：

```bash
ss -lntp
df -h /var/cache /mnt
timedatectl status
```

如果服务器前面还有 CDN、WAF、负载均衡器或云厂商代理，还必须单独检查这些上游是否限制请求体大小或上传时长。Caddy 配置无法绕过上游产生的 `413`、`408` 或连接超时。

## 2. 安装依赖

从 rclone 和 Caddy 官方仓库安装适配当前架构的稳定版本。不要使用来源不明的二进制文件。

Debian/Ubuntu 需要的基础软件包括：

```bash
apt-get update
apt-get install -y ca-certificates curl fuse3 apache2-utils
```

按照 Caddy 官方安装文档添加软件源并安装 `caddy`，按照 rclone 官方安装文档安装稳定版 `rclone`。完成后记录版本：

```bash
rclone version
caddy version
systemctl status caddy --no-pager
```

## 3. 创建专用用户和目录

不要让长期运行的 rclone 服务使用普通登录用户。

```bash
useradd --system \
  --home-dir /var/lib/rclone \
  --create-home \
  --shell /usr/sbin/nologin \
  rclone

install -d -o rclone -g rclone -m 700 /var/lib/rclone/.config/rclone
install -d -o rclone -g rclone -m 700 /var/cache/rclone/mount
install -d -o rclone -g rclone -m 700 /var/cache/rclone/webdav
install -d -o rclone -g rclone -m 755 /mnt/onedrive
install -d -o rclone -g rclone -m 750 /etc/rclone
```

如果挂载点需要被 rclone 用户之外的本机进程访问，确认 `/etc/fuse.conf` 中存在：

```text
user_allow_other
```

只有确实需要跨用户访问挂载点时才启用 `--allow-other`。

## 4. 配置 OneDrive remote

使用专用用户创建名为 `onedrive` 的 remote：

```bash
runuser -u rclone -- rclone config \
  --config=/var/lib/rclone/.config/rclone/rclone.conf
```

按照交互提示选择 OneDrive，并在可信环境中完成 OAuth 授权。无浏览器服务器可按 rclone 的 remote setup 流程在另一台可信设备完成授权。

配置文件预期位于：

```text
/var/lib/rclone/.config/rclone/rclone.conf
```

检查权限和基本访问：

```bash
chown rclone:rclone /var/lib/rclone/.config/rclone/rclone.conf
chmod 600 /var/lib/rclone/.config/rclone/rclone.conf

runuser -u rclone -- rclone lsd onedrive: \
  --config=/var/lib/rclone/.config/rclone/rclone.conf
runuser -u rclone -- rclone about onedrive: \
  --config=/var/lib/rclone/.config/rclone/rclone.conf
```

不要执行会输出完整配置、OAuth token 或 Drive ID 的命令，也不要将 `rclone.conf` 复制到仓库。

## 5. 配置 WebDAV 认证

rclone 官方支持标准 Apache `htpasswd` 文件，并推荐 bcrypt。创建认证文件：

```bash
htpasswd -cB /etc/rclone/webdav.htpasswd <WEBDAV_USER>
chown rclone:rclone /etc/rclone/webdav.htpasswd
chmod 600 /etc/rclone/webdav.htpasswd
```

命令会交互式读取密码，不要把密码写在命令行中。增加第二个用户时不要再使用 `-c`，否则会覆盖原文件：

```bash
htpasswd -B /etc/rclone/webdav.htpasswd <ANOTHER_USER>
```

`htpasswd` 文件可以在 rclone 运行期间更新。客户端仍然使用标准 WebDAV Basic Auth，但凭据只通过 HTTPS 传输。

## 6. 创建挂载服务

创建 `/etc/systemd/system/rclone-onedrive-mount.service`：

```ini
[Unit]
Description=Rclone OneDrive Mount
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=rclone
Group=rclone
ExecStart=/usr/bin/rclone mount onedrive: /mnt/onedrive \
  --config=/var/lib/rclone/.config/rclone/rclone.conf \
  --allow-other \
  --vfs-cache-mode=full \
  --cache-dir=/var/cache/rclone/mount \
  --vfs-cache-max-size=20Gi \
  --vfs-cache-min-free-space=5Gi \
  --dir-cache-time=72h \
  --poll-interval=15s \
  --buffer-size=64Mi \
  --onedrive-upload-cutoff=4Mi \
  --onedrive-chunk-size=10Mi \
  --timeout=10m
ExecStop=/bin/fusermount3 -uz /mnt/onedrive
Restart=on-failure
RestartSec=10
TimeoutStopSec=30
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

注意：

- 如果 `command -v fusermount3` 返回其他路径，应修改 `ExecStop`。
- 如果没有启用 FUSE `user_allow_other`，删除 `--allow-other`。
- `20Gi` 和 `5Gi` 只是示例，必须根据磁盘容量和最大文件调整。
- VFS 缓存限制不是硬上限；仍被打开的文件可能使缓存暂时超过 `--vfs-cache-max-size`。

## 7. 创建 WebDAV 服务

创建 `/etc/systemd/system/rclone-webdav.service`：

```ini
[Unit]
Description=Rclone OneDrive WebDAV
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=rclone
Group=rclone
ExecStart=/usr/bin/rclone serve webdav onedrive: \
  --config=/var/lib/rclone/.config/rclone/rclone.conf \
  --addr=127.0.0.1:8100 \
  --htpasswd=/etc/rclone/webdav.htpasswd \
  --vfs-cache-mode=writes \
  --cache-dir=/var/cache/rclone/webdav \
  --vfs-cache-max-size=20Gi \
  --vfs-cache-min-free-space=5Gi \
  --dir-cache-time=72h \
  --poll-interval=15s \
  --buffer-size=64Mi \
  --onedrive-upload-cutoff=4Mi \
  --onedrive-chunk-size=10Mi \
  --transfers=1 \
  --tpslimit=5 \
  --timeout=10m \
  --server-read-timeout=6h \
  --server-write-timeout=6h
Restart=on-failure
RestartSec=5
TimeoutStopSec=30
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

rclone 的 `--server-read-timeout` 和 `--server-write-timeout` 是整个传输的总时长，不是空闲超时。默认值为 1 小时；这里提高到 6 小时，适合低速大文件。若业务可能超过 6 小时，应按最大文件和最低可接受上传速度重新计算。

不要让挂载服务和 WebDAV 服务共用同一个 `--cache-dir`。两个服务也不应并发写入同一个远端文件路径。

## 8. 启动 rclone 服务

先校验 unit，再启动：

```bash
systemd-analyze verify /etc/systemd/system/rclone-onedrive-mount.service
systemd-analyze verify /etc/systemd/system/rclone-webdav.service
systemctl daemon-reload
systemctl enable --now rclone-onedrive-mount.service
systemctl enable --now rclone-webdav.service
```

检查：

```bash
systemctl is-active rclone-onedrive-mount.service rclone-webdav.service
findmnt -T /mnt/onedrive
ss -lntp | grep ':8100'
journalctl -u rclone-onedrive-mount.service -u rclone-webdav.service -n 100 --no-pager
```

`8100` 必须只监听 `127.0.0.1`。

## 9. 配置 Caddy

先备份现有配置：

```bash
cp -a /etc/caddy/Caddyfile /etc/caddy/Caddyfile.backup
```

在 `/etc/caddy/Caddyfile` 中添加独立站点：

```caddy
<WEBDAV_HOST> {
    log {
        output file /var/log/caddy/webdav-access.log {
            roll_size 100MiB
            roll_keep 10
            roll_keep_for 720h
        }
        format json
    }

    reverse_proxy 127.0.0.1:8100
}
```

确保 Caddy 能写日志目录：

```bash
install -d -o caddy -g caddy -m 750 /var/log/caddy
```

### Caddy 与大文件上传

Caddy 与 nginx 的配置方式不同：

- Caddy 默认不限制上传请求体大小。
- 只有显式添加 `request_body { max_size ... }` 才会在超限时返回 `413`。
- 不要为 WebDAV 配置 `request_buffers`；官方文档说明它会在向上游发送前先把指定大小的请求体读入缓冲，效率较低。
- `flush_interval` 控制的是上游响应缓冲，不解决上传请求体问题。
- Caddy 默认的客户端 `read_body`、响应 `write`、上游 `read_timeout`、`write_timeout` 都没有超时，不需要为大文件机械添加 nginx 风格的超时指令。
- `reverse_proxy` 默认保留原 HTTP 方法和 URI，因此 `PROPFIND`、`MKCOL`、`PUT`、`DELETE` 等 WebDAV 方法可直接转发。
- Caddy 默认保留传给上游的 `Authorization` 请求头；认证仍由 rclone 的 `htpasswd` 处理。

检查整个 Caddyfile 及其 `import` 文件，确保没有作用到该站点的全局小请求体限制。如果业务需要统一限制其他站点，应使用 matcher 将 WebDAV 域名排除。

格式化、校验并平滑加载：

```bash
caddy fmt --overwrite /etc/caddy/Caddyfile
caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
systemctl reload caddy
systemctl is-active caddy
journalctl -u caddy -n 100 --no-pager
```

配置错误时恢复备份并重新校验，不要在未校验的情况下反复重启 Caddy。

## 10. 分层验证

### 10.1 验证本机 WebDAV

下面的 `curl --user <WEBDAV_USER>` 会交互式询问密码，不会把密码写入 shell 历史：

```bash
curl --user <WEBDAV_USER> \
  -X PROPFIND \
  -H 'Depth: 1' \
  http://127.0.0.1:8100/
```

正常结果为 HTTP `207 Multi-Status`。不带认证的请求应返回 `401 Unauthorized`。

### 10.2 验证 Caddy HTTPS

```bash
curl --user <WEBDAV_USER> \
  -X PROPFIND \
  -H 'Depth: 1' \
  https://<WEBDAV_HOST>/
```

必须使用受信任证书。生产验证不要使用 `-k` 跳过 TLS 校验。

### 10.3 验证中等大小 PUT

先创建专用测试目录和 256 MiB 测试文件：

```bash
runuser -u rclone -- rclone mkdir onedrive:test-dev \
  --config=/var/lib/rclone/.config/rclone/rclone.conf
truncate -s 256M /var/tmp/rclone-webdav-256MiB.test
```

通过公网入口上传：

```bash
curl --fail-with-body \
  --user <WEBDAV_USER> \
  --upload-file /var/tmp/rclone-webdav-256MiB.test \
  https://<WEBDAV_HOST>/test-dev/rclone-webdav-256MiB.test
```

预期为 `201 Created` 或覆盖已有文件时的成功状态。随后等待 VFS 后台写回，并核对远端大小：

```bash
runuser -u rclone -- rclone lsjson \
  onedrive:test-dev/rclone-webdav-256MiB.test \
  --config=/var/lib/rclone/.config/rclone/rclone.conf \
  --files-only \
  --no-modtime \
  --no-mimetype
```

远端大小必须为 `268435456` 字节。

### 10.4 验证目标级大文件

使用接近实际业务最大值的非敏感测试文件重复 PUT。至少覆盖：

- 16 MiB 基础上传；
- 256 MiB 中等上传；
- 1 GiB 以上大文件上传；
- 应用实际生成的 `.m3u8sp` 文件。

同时观察：

```bash
journalctl -fu rclone-webdav.service
journalctl -fu caddy
watch -n 2 'du -sh /var/cache/rclone/webdav; df -h /var/cache/rclone/webdav'
```

不要只看客户端进度。最终成功标准是 OneDrive 远端对象存在且大小与本地文件完全一致。

### 10.5 清理测试文件

确认测试完成后，仅删除本节创建的明确测试对象：

```bash
runuser -u rclone -- rclone deletefile \
  onedrive:test-dev/rclone-webdav-256MiB.test \
  --config=/var/lib/rclone/.config/rclone/rclone.conf
rm -f /var/tmp/rclone-webdav-256MiB.test
```

不要清空整个 VFS 缓存目录。缓存中可能仍有尚未提交的用户文件。

## 11. 应用配置

客户端 WebDAV 服务应填写：

```text
地址：https://<WEBDAV_HOST>/
用户名：<WEBDAV_USER>
密码：创建 htpasswd 时输入的原始密码
```

如果浏览目录成功但上传持续返回 `401`，通常是应用的 M3U8S 输出功能读取到了空密码或旧密码。处理顺序：

1. 删除客户端中保存的旧 WebDAV 密码；
2. 重新登录 WebDAV 服务；
3. 重新选择 M3U8S 的 WebDAV 输出服务和目录；
4. 确认浏览请求先返回 `401` 后认证重试为 `207`；
5. 再次上传，并检查 PUT 是否由 `401` 变为成功状态。

服务端更换密码后，必须同步更新所有客户端。不要通过关闭认证规避凭据不一致。

## 12. 状态码排障

| 状态/现象 | 最可能来源 | 检查方向 |
| --- | --- | --- |
| `401 Unauthorized` | rclone WebDAV 认证 | 用户名、客户端保存密码、`htpasswd` 用户是否一致 |
| `413 Request Entity Too Large` | Caddy 上游、Caddy `request_body` 或其他代理 | 搜索所有代理/WAF 的请求体限制；检查 Caddyfile 的 `max_size` |
| `502 Bad Gateway` | Caddy 无法连接 rclone，或上游连接被重置 | rclone 服务状态、8100 监听、Caddy 和 rclone 同时段日志 |
| `408`、超时或连接重置 | 客户端、上游代理、rclone 总传输超时 | 文件大小、实际速率、`--server-*-timeout`、CDN/WAF 限制 |
| HTTP 成功但远端仍为 0 字节 | VFS 仍在后台写回，或 OneDrive 提交失败 | VFS 缓存、rclone journal、远端最终大小 |
| `upload chunks may be taking too long` | OneDrive 分块或并发压力 | 保持 `10Mi` chunk，将 `--transfers` 降为 `1`，检查网络和限流 |
| 磁盘空间快速下降 | VFS 缓存承接完整上传 | 缓存容量、并发上传数、未完成写回和最小剩余空间 |

推荐按以下层次定位，不要一次修改多个变量：

1. 直接访问 `127.0.0.1:8100` 验证 rclone；
2. 通过 Caddy 域名验证同一请求；
3. 使用 `curl` 和正确凭据执行 PUT；
4. 使用应用执行同一路径 PUT；
5. 检查 OneDrive 最终对象大小；
6. 认证正确后仍失败，才调整 chunk、并发和超时。

## 13. 运维规则

### 修改配置

每次修改前备份：

```bash
cp -a /etc/systemd/system/rclone-onedrive-mount.service \
  /etc/systemd/system/rclone-onedrive-mount.service.backup
cp -a /etc/systemd/system/rclone-webdav.service \
  /etc/systemd/system/rclone-webdav.service.backup
cp -a /etc/caddy/Caddyfile /etc/caddy/Caddyfile.backup
```

修改 systemd unit 后：

```bash
systemd-analyze verify /etc/systemd/system/rclone-onedrive-mount.service
systemd-analyze verify /etc/systemd/system/rclone-webdav.service
systemctl daemon-reload
systemctl restart rclone-onedrive-mount.service rclone-webdav.service
```

修改 Caddyfile 后：

```bash
caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
systemctl reload caddy
```

### 升级

升级 rclone 或 Caddy 后至少验证：

- 服务启动和挂载；
- WebDAV `PROPFIND`；
- 256 MiB PUT；
- 1 GiB 以上 PUT；
- OneDrive 最终大小；
- 应用 `.m3u8sp` 实际上传。

不要在 VFS 仍有待上传文件时重启或升级 rclone。先检查：

```bash
du -sh /var/cache/rclone/mount /var/cache/rclone/webdav
lsof +D /var/cache/rclone/webdav
journalctl -u rclone-webdav.service -n 200 --no-pager
```

### 备份范围

应备份：

- systemd unit 模板；
- 已脱敏的 Caddyfile 模板；
- rclone 与 Caddy 版本；
- 部署日期和测试结果；
- WebDAV 用户名清单，但不包含密码哈希内容。

必须通过安全秘密管理或加密备份单独保护：

- `rclone.conf`；
- `webdav.htpasswd`；
- OAuth token；
- Caddy 私钥及账户数据。

禁止将这些内容写入应用日志、工单、聊天记录或 Git。

## 14. 上线检查清单

- [ ] DNS 指向正确服务器。
- [ ] 仅开放必要的 80/443，8100 只监听回环地址。
- [ ] rclone 和 Caddy 来自官方渠道并记录版本。
- [ ] rclone 使用专用系统用户。
- [ ] `rclone.conf` 权限为 `0600`。
- [ ] WebDAV 使用 bcrypt `htpasswd`，没有明文密码参数。
- [ ] mount 与 WebDAV 使用不同缓存目录。
- [ ] chunk size 为 320 KiB 的整数倍，当前使用 `10Mi`。
- [ ] 大文件启用 `--onedrive-upload-cutoff=4Mi`。
- [ ] 初始 `--transfers=1`。
- [ ] Caddyfile 没有作用于 WebDAV 的小 `request_body max_size`。
- [ ] Caddy 没有配置 `request_buffers`。
- [ ] Caddy 配置校验通过并使用 HTTPS 受信任证书。
- [ ] 本机和公网 PROPFIND 均通过。
- [ ] 16 MiB、256 MiB、1 GiB 以上 PUT 均通过。
- [ ] OneDrive 最终对象大小与本地一致。
- [ ] 应用重新保存了正确 WebDAV 凭据。
- [ ] `.m3u8sp` 实际上传通过。
- [ ] 测试对象已清理，未清空仍在使用的 VFS 缓存。

## 官方参考

- [rclone OneDrive backend](https://rclone.org/onedrive/)
- [rclone serve webdav](https://rclone.org/commands/rclone_serve_webdav/)
- [rclone mount and VFS cache](https://rclone.org/commands/rclone_mount/)
- [rclone remote setup](https://rclone.org/remote_setup/)
- [Caddy reverse_proxy](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy)
- [Caddy request_body](https://caddyserver.com/docs/caddyfile/directives/request_body)
- [Caddy global server timeouts](https://caddyserver.com/docs/caddyfile/options#server-options)
- [Caddy running as a service](https://caddyserver.com/docs/running)
- [Caddy installation](https://caddyserver.com/docs/install)
