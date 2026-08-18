# Release Tag Rules

项目发布标签必须使用 `v` 开头的 Semantic Versioning 2.0.0 格式：

```text
vMAJOR.MINOR.PATCH
```

## Stable

Stable 版本必须显式使用 `stable` 标识：

```text
v1.0.0-stable
v1.2.3-stable
```

Stable 通道只接受 `-stable` 版本。

## Beta

Beta 版本必须使用 `beta` 标识，可选数字序号：

```text
v1.0.1-beta
v1.0.1-beta.1
v2.0.0-beta.12
```

Beta 通道只接受 Beta 版本，不会接收 Stable 或 Alpha 版本。

## Alpha

Alpha 版本必须使用 `alpha` 标识，可选数字序号：

```text
v1.0.1-alpha
v1.0.1-alpha.1
v2.0.0-alpha.12
```

Alpha 通道只接受 Alpha 版本，不会接收 Stable 或 Beta 版本。

## Invalid Tags

以下标签不会进入更新列表，也不会被自动下载：

```text
1.0.1                  # 缺少 v 前缀
v1.0                   # 缺少 PATCH
v1.0.1-rc.1            # 不支持 rc
v1.0.1-nightly         # 不支持 nightly
v1.0.1-dev             # 不支持 dev
v1.0.1-beta.preview    # beta 序号必须是数字
v1.0.1-alpha.01         # 数字序号不能有前导零
v1.0.1                  # 缺少 stable、alpha 或 beta 标识
```

## Build Metadata

Semantic Versioning 的 build metadata 可以存在，但不参与版本比较：

```text
v1.0.1-stable+build.42
v1.0.1-beta.2+commit.abc123
```

## Comparison

更新服务只提供高于当前版本的版本，不提供降级版本。预发布版本按照 SemVer 规则比较：

```text
alpha < alpha.1 < beta < beta.1 < stable
```

GitHub Actions 在发布前会校验标签格式；客户端从 GitHub Releases 列表中再次进行相同的通道筛选。
