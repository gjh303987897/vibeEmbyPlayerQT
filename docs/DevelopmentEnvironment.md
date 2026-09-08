# 开发环境记录 (Windows 开发机)

> 本文档记录 vibePlayerQT 在当前 Windows 开发机上已补齐/验证的开发环境，
> 包含每个依赖的来源、版本、安装路径与验证方式。
> 新环境搭建时按本文档逐项安装即可复现可构建状态。
>
> 最后验证日期：见 git 提交记录（本次全部 18 个测试通过，主程序冒烟运行正常）。

---

## 1. 环境总览

| 组件 | 版本 | 来源 / 安装方式 | 路径 |
| --- | --- | --- | --- |
| 操作系统 | Windows x86_64 | - | - |
| Visual Studio 2022 Build Tools (MSVC C++ 工具链) | VS2022 | 官方安装器 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| CMake | 4.3.4 | `scoop install cmake` | `~/scoop/shims/cmake.exe` |
| Ninja | 1.13.2 | `scoop install ninja` | `~/scoop/shims/ninja.exe` |
| LLVM (clang-cl) | 已安装 | 官方安装包 | `C:\Program Files\LLVM\bin\clang-cl.exe` |
| Qt | 6.7.3 msvc2019_64 | `aqtinstall`（scoop 安装 aqt） | `D:\Qt\6.7.3\msvc2019_64` |
| aqtinstall | 3.3.0 | `scoop install aqtinstall` | `~/scoop/shims/aqt.exe` |
| 7-Zip | 26.02 | `scoop install 7zip` | 用于解压 mpv-dev 7z 包 |
| Git | 2.55.0.2 | `scoop install git` | - |
| Python | 3.13.13 | `scoop install miniconda3`（aqt 依赖） | - |
| libmpv 开发包 | mpv-dev-x86_64-20260907-git-989d32716e | GitHub release `zhongfly/mpv-winbuild` | `third_party/mpv/dev/` |

### scoop 已安装的相关辅助工具

```
7zip  aqtinstall  cmake  ffmpeg  git  ninja  uv  (miniconda3)
```

其中 `ffmpeg` 为本地调试辅助，不是构建必需依赖。

---

## 2. Qt 6.7.3 安装细节

项目 CMake 要求 `Qt6 6.5+`，组件需求（见 `CMakeLists.txt`）：

`Concurrent, Qml, Quick, QuickControls2, QuickDialogs2, Network, Sql, Widgets, Xml, Test`

当前安装位置：`D:\Qt\6.7.3\msvc2019_64`

已确认存在：

- `bin/windeployqt.exe`、`bin/qmake.exe`
- `plugins/sqldrivers/qsqlite.dll`（SQLite 驱动，断点续播/历史依赖）
- `qml/QtQuick/Controls`、`qml/QtQuick/Dialogs`（QML 模块）

新环境用 aqtinstall 复现安装：

```powershell
pip install aqtinstall   # 或 scoop install aqtinstall
aqt install-qt windows desktop 6.7.3 win64_msvc2019_64 `
    -m qtquick3d qt5compat qtimageformats `
    -D D:\Qt
```

> 注意：MSVC 版 Qt 包（`msvc2019_64`）与 MSVC / clang-cl 编译的应用 ABI 兼容。

---

## 3. libmpv 开发包（Windows 专用，版本固定）

> **libmpv 已通过 `deps/libmpv.lock.json` 版本固定**（tag + SHA-256）。
> CI、CMake 配置期校验和本地拉取脚本共读这一份 lock，本地与发布产物链接
> 字节一致的 libmpv。升级流程：改 lock 的 tag/asset/哈希 → 本地重拉 → 把
> 刷新后的 git 跟踪文件（headers、`libmpv.dll.a`）与 lock 一起提交。

Windows 下 CMake 从 `third_party/mpv/dev` 读取（可用 `-DMPV_ROOT=` 覆盖，
但会触发“超出固定契约”的警告），要求三个文件缺一不可：

```text
third_party/mpv/dev/include/mpv/client.h
third_party/mpv/dev/libmpv.dll.a
third_party/mpv/dev/libmpv-2.dll
```

本机已按 lock 下载并校验（当前 pin 的 release）：

| 项目 | 值 |
| --- | --- |
| 仓库 | `zhongfly/mpv-winbuild` |
| Release tag（lock 固定） | `2026-09-07-989d32716e` |
| 资产 | `mpv-dev-x86_64-20260907-git-989d32716e.7z` |
| SHA-256 | 以 `deps/libmpv.lock.json` 的 `windows.assetSha256` 为准（本文不复制副本，副本会过期） |

新克隆后唯一需要的动作：

```powershell
pwsh -NoProfile -File scripts\fetch-mpv-dev.ps1
```

脚本幂等：目标文件存在且哈希匹配 lock 时直接跳过；否则下载、校验归档
SHA-256、解压后再逐一校验解包文件哈希。CMake 配置期也会比对
`libmpv.dll.a` 哈希，不一致直接 FATAL 并提示运行该脚本。

<details>
<summary>手动步骤（与脚本等价）</summary>

```powershell
# 1. 查询最新 release 资产名（CI 使用同样的正则筛选）
#    ^mpv-dev-x86_64-[0-9]{8}-git-[0-9a-fA-F]+\.7z$
gh release view --repo zhongfly/mpv-winbuild --json assets -q .assets

# 2. 下载并校验
curl.exe -L -o mpv-dev.7z `
  "https://github.com/zhongfly/mpv-winbuild/releases/download/2026-09-07-989d32716e/mpv-dev-x86_64-20260907-git-989d32716e.7z"
Get-FileHash -Algorithm SHA256 mpv-dev.7z   # 必须与 release digest 一致

# 3. 解压到 third_party/mpv/dev
7z x mpv-dev.7z -othird_party\mpv\dev -y
```

</details>

> Linux / macOS 不需要此包，CMake 通过 `pkg-config` 查找系统 libmpv
> （macOS: `brew install mpv pkg-config`，Linux: `libmpv-dev` 包）。
> 这两处无法字节固定（homebrew 滚动 / apt 跟镜像），当前基线记录在
> `deps/libmpv.lock.json` 的 `baselines`（Linux 0.37.0-1ubuntu4，
> macOS 0.41.0）。`PlayerController` 启动时会把实际 libmpv 运行时版本与
> 构建期 pin 信息一起写入日志，保证问题可追查。

---

## 4. 编译器说明

两条可用工具链，均已在 `docs/BuildWindows.md` 提供脚本：

1. **MSVC**（Visual Studio 17 2022 生成器）——见 `docs/BuildWindows.md`
2. **clang-cl**（推荐，Ninja + Debug）——`scripts/configure-clang.cmd` + `scripts/build-clang.cmd`
   - 脚本会自动通过 `vswhere` 定位 VS 并调用 `vcvars64.bat`
   - 本机 vswhere 定位结果：`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`

---

## 5. 构建与验证命令（本机已验证）

```powershell
# 配置（clang-cl + Ninja + Debug，Qt 前缀 D:/Qt/6.7.3/msvc2019_64）
.\scripts\configure-clang.cmd

# 构建主程序 + 全部测试
cmake --build build-clang --parallel

# 部署运行时依赖
D:\Qt\6.7.3\msvc2019_64\bin\windeployqt.exe --release --qmldir qml build-clang\vibePlayerQT.exe

# 运行全部单元测试（18 个）
cd build-clang
$env:QT_QPA_PLATFORM = "offscreen"   # 无显示环境下跑 QML/QtTest
ctest --output-on-failure

# 启动应用
.\vibePlayerQT.exe
```

## 6. 本次验证结果

- `cmake configure`（clang-cl/Ninja）：✅ 成功
- `vibePlayerQT.exe` 完整链接：✅ 成功（仅有少量既有代码的 `[[nodiscard]]` 警告）
- `windeployqt` 运行时部署：✅ 成功
- `ctest` 全量测试：**18/18 全部通过** ✅
- 应用启动冒烟测试：✅ 进程持续运行，Qt/QML/libmpv 运行时加载正常

## 7. 已知环境注意事项

- 在 MSYS/Git-Bash 等环境调用 `.cmd` 脚本需写成 `cmd //c "scripts\configure-clang.cmd"`（反斜杠 + 引号）。
- 直接运行测试 exe 需要 Qt bin 在 `PATH` 中；用 `ctest` 时由
  `ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:..."` 自动处理。
- `third_party/mpv/dev/libmpv-2.dll` 约 114 MB，已被 `.gitignore` 排除，
  新克隆仓库后必须按第 3 节重新下载，否则 CMake 配置阶段会直接报错。
- 项目尚引入 spdlog/libsmb2 的依赖计划（见 AGENTS.md / SMB-TODO.md），
  当前代码库实际仅依赖 Qt + libmpv + SQLite，未安装也不影响构建。
