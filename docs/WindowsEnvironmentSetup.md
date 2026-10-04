# Windows 本机开发环境配置记录

配置与验证日期：**2026-10-05**。项目位置：`D:\workspace\vibeEmbyPlayerQT`。

本机环境已配置完成，可以使用现有脚本编译和运行项目。本次只配置 Windows x64 开发环境；其他平台由 GitHub Actions 处理，未修改 CI 或播放器业务代码。

## 1. 本次安装及更新的组件

下面的大小来自安装完成后的实际文件统计，不是下载包大小。

| 组件 | 实际版本 | 安装位置 | 大小 |
| --- | --- | --- | ---: |
| Qt（MSVC x64 套件） | 6.7.3，`msvc2019_64` | `D:\Qt\6.7.3\msvc2019_64` | 1,684.36 MiB |
| Visual Studio 2022 Build Tools | 17.14.41；内部版本 17.14.37710.0 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` | 2,902.72 MiB |
| Windows SDK | 10.0.26100；安装包版本 10.1.26100.7705 | `C:\Program Files (x86)\Windows Kits\10` | 1,713.05 MiB |
| Visual Studio Installer（共享安装器，已更新） | 文件版本 4.10.30.62513 | `C:\Program Files (x86)\Microsoft Visual Studio\Installer` | 125.38 MiB |
| LLVM（含 clang-cl、lld-link） | 23.1.2 | `C:\Users\30398\scoop\apps\llvm\23.1.2` | 3,052.85 MiB |
| CMake | 4.4.4 | `C:\Users\30398\scoop\apps\cmake\4.4.4` | 136.67 MiB |
| Ninja | 1.13.2 | `C:\Users\30398\scoop\apps\ninja\1.13.2` | 0.58 MiB |
| aqtinstall（Qt 安装工具） | 3.3.0 | `C:\Users\30398\scoop\apps\aqtinstall\3.3.0` | 14.48 MiB |
| OpenSSL（Qt 提供的预编译包） | 3.0.16 | `D:\Qt\Tools\OpenSSLv3` | 38.38 MiB |
| libmpv 开发包目录（补齐运行时 DLL） | 固定包 `2026-09-07-989d32716e` | `D:\workspace\vibeEmbyPlayerQT\third_party\mpv\dev` | 114.23 MiB |

**以上主要安装目录合计约 9.55 GiB。** 此数值包含 Windows SDK 和共享安装器，不包含下载缓存、构建产物、Windows Installer 缓存或其他系统共享运行库；它是目录文件总量，并非本次系统磁盘净增量。libmpv 目录中原有的头文件和导入库也包含在统计内。

统计口径：`1 MiB = 1,048,576 字节`，`1 GiB = 1,073,741,824 字节`；累加文件长度，跳过符号链接和目录联接，避免重复计算 Scoop 的 `current` 别名。文件系统实际分配空间可能略有差异。

### Qt 与 C++ 工具链

Qt 已具备项目使用的 `Concurrent`、`Qml`、`Quick`、`QuickControls2`、`QuickDialogs2`、`Network`、`Sql`、`Widgets`、`Xml`、`Test` 模块，并安装了 `qtimageformats`。SQLite 驱动位于 `D:\Qt\6.7.3\msvc2019_64\plugins\sqldrivers\qsqlite.dll`。

VS Build Tools 安装了 C++ 构建工作负载、MSVC v143 x86/x64 工具和 Windows 11 SDK，MSVC 工具目录版本为 **14.44.35207**。指定的组件 ID 为：

- `Microsoft.VisualStudio.Workload.VCTools`
- `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`
- `Microsoft.VisualStudio.Component.Windows11SDK.26100`

组件含义见 [Microsoft Build Tools 组件列表](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022)。本次使用 Build Tools，没有安装完整 Visual Studio IDE。

### libmpv 与 OpenSSL

libmpv 运行日志显示实际版本为 `mpv v0.41.0-1037-g989d32716`。运行时文件为：

`D:\workspace\vibeEmbyPlayerQT\third_party\mpv\dev\libmpv-2.dll`

该 DLL 大小为 **119,583,744 字节（约 114.04 MiB）**。下载归档、DLL 和导入库的 SHA-256 均已按项目的 `D:\workspace\vibeEmbyPlayerQT\deps\libmpv.lock.json` 校验。

OpenSSL 用于当前加密相关功能和测试。其 x64 程序及 DLL 目录为 `D:\Qt\Tools\OpenSSLv3\Win_x64\bin`，其中 `libcrypto-3-x64.dll` 和 `libssl-3-x64.dll` 已复制到本次程序输出目录。

## 2. 原有工具：本次直接复用

| 组件 | 版本 | 原有位置 | 大小 |
| --- | --- | --- | ---: |
| Git | 2.56.0 | `C:\Users\30398\scoop\apps\git\2.56.0` | 387.18 MiB |
| 7-Zip | 26.03 | `C:\Users\30398\scoop\apps\7zip\26.03` | 5.64 MiB |
| FFmpeg | 9.0.2 | `C:\Users\30398\scoop\apps\ffmpeg\9.0.2` | 664.20 MiB |
| Vulkan Loader | 1.4.321.0 | `C:\Windows\System32\vulkan-1.dll` | 1.55 MiB |

这些组件未计入上面的 9.55 GiB。FFmpeg 已验证支持 `libx264`、`libx265`、`aac`，供项目的加密 HLS 处理和相关测试使用。Vulkan Loader 为系统现有文件，本次未安装完整 Vulkan SDK。aqtinstall 使用独立可执行程序，本次也未额外安装 Python。

## 3. 已配置的工具路径

以下目录已经写入当前用户的 `PATH`，并保留了原有条目：

```text
C:\Users\30398\scoop\shims
C:\Users\30398\scoop\apps\llvm\current\bin
D:\Qt\6.7.3\msvc2019_64\bin
D:\Qt\Tools\OpenSSLv3\Win_x64\bin
```

Scoop 的 `current` 指向上表中的版本目录；`shims` 提供 `cmake`、`ninja`、`aqt`、`git`、`7z`、`ffmpeg` 等命令入口。

**重新打开终端后即可继承这些路径。** 若在已打开的编辑器中构建，也应重新启动编辑器。MSVC 的 `INCLUDE`、`LIB` 等变量由现有构建脚本调用 `vcvars64.bat` 初始化，无需全局手动设置。

现有配置脚本使用 `D:/Qt/6.7.3/msvc2019_64`，优先读取 Scoop 的 CMake/Ninja；本机通过 `PATH` 找到 Scoop 安装的 `clang-cl`，实际链接器为 `C:\Users\30398\scoop\apps\llvm\current\bin\lld-link.exe`。

## 4. 安装方式记录

Scoop 本次安装的包为：

```powershell
scoop install cmake ninja llvm aqtinstall
```

Qt 和 OpenSSL 使用以下命令安装到 `D:\Qt`：

```powershell
aqt install-qt windows desktop 6.7.3 win64_msvc2019_64 -O D:\Qt -m qtimageformats --external 7z
aqt install-tool windows desktop tools_opensslv3_x64 -O D:\Qt --external 7z
```

参数说明见 [aqtinstall 命令文档](https://aqtinstall.readthedocs.io/en/latest/cli.html)。

VS Build Tools 使用 [Microsoft 官方 VS 2022 安装程序](https://aka.ms/vs/17/release/vs_buildtools.exe)，安装目录和组件如第 1 节所列。安装器完成状态正常，无待重启要求。

libmpv 使用仓库锁定的发布包。后续重新获取 DLL 时，在项目目录运行：

```powershell
pwsh -NoProfile -File .\scripts\fetch-mpv-dev.ps1
```

## 5. 编译、测试和运行

在新开的 PowerShell 中执行：

```powershell
Set-Location 'D:\workspace\vibeEmbyPlayerQT'
.\scripts\configure-clang.cmd
.\scripts\build-clang.cmd
ctest --test-dir build-clang --output-on-failure
```

脚本当前生成 **Debug** 构建。部署 Qt 运行依赖并启动程序：

```powershell
& 'D:\Qt\6.7.3\msvc2019_64\bin\windeployqt.exe' --debug --qmldir qml build-clang\vibePlayerQT.exe
Copy-Item -LiteralPath 'D:\Qt\Tools\OpenSSLv3\Win_x64\bin\libcrypto-3-x64.dll','D:\Qt\Tools\OpenSSLv3\Win_x64\bin\libssl-3-x64.dll' -Destination 'D:\workspace\vibeEmbyPlayerQT\build-clang'
.\build-clang\vibePlayerQT.exe
```

`windeployqt` 的用途见 [Qt Windows 部署文档](https://doc.qt.io/qt-6.8/windows-deployment.html)。上面的 Debug 目录用于本机开发，发布包应使用 CI 对应的 Release 构建和部署流程。

本次实际验证结果：

- CMake 配置和 Ninja 编译成功，主程序及测试目标均构建完成。
- **22 / 22 个 CTest 测试目标通过**，耗时约 110 秒。
- Qt 平台插件、SQLite 插件、libmpv、Vulkan Loader 及 OpenSSL DLL 已部署到程序输出目录。
- 程序启动与本地 H.264 测试视频播放检查通过；日志确认 libmpv 窗口嵌入初始化、D3D11VA 硬件解码及视频输出成功。

验证记录保存在 `D:\workspace\vibeEmbyPlayerQT\build-clang\Testing\Temporary\LastTest.log` 和 `D:\workspace\vibeEmbyPlayerQT\build-environment\startup-verification.log`。本次未连接真实 Emby、Jellyfin、WebDAV 或 SMB 服务执行端到端验收。

## 6. 缓存和构建产物占用

这些大小与主要安装目录分开计算：

| 类型 | 位置 | 大小 | 处理方法 |
| --- | --- | ---: | --- |
| 本次四个 Scoop 包的下载缓存 | `C:\Users\30398\scoop\cache` 中 cmake/ninja/llvm/aqtinstall 对应文件 | 676.48 MiB | 可用下方命令清理，已安装工具仍可使用 |
| 全部 Scoop 下载缓存，含原有包 | `C:\Users\30398\scoop\cache` | 896.72 MiB | 包含上一行，不重复相加 |
| VS 共享包目录 | `C:\ProgramData\Microsoft\VisualStudio\Packages` | 39.99 MiB | 交由 VS Installer 管理 |
| 本次安装包、日志及检查文件 | `D:\workspace\vibeEmbyPlayerQT\build-environment` | 40.22 MiB | 不再需要安装记录时可删除此目录 |
| 本次 Debug 构建及部署产物 | `D:\workspace\vibeEmbyPlayerQT\build-clang` | 646.74 MiB | 不再需要本次构建时可删除此目录 |

只清理本次新增工具的 Scoop 下载缓存：

```powershell
scoop cache rm cmake ninja llvm aqtinstall
```

构建目录也包含一份 libmpv 和 Qt/OpenSSL 运行依赖，所以卸载原始安装目录后，构建目录仍会占用空间。上述统计为本次验证结束时的快照，后续构建和下载会改变大小。

## 7. 如何卸载

以下为后续卸载说明，本次没有执行卸载。

### 7.1 CMake、Ninja、LLVM、aqtinstall

在普通 PowerShell 中执行需要卸载的那一行：

```powershell
scoop uninstall cmake
scoop uninstall ninja
scoop uninstall llvm
scoop uninstall aqtinstall
```

也可以一次卸载四个包：`scoop uninstall cmake ninja llvm aqtinstall`。下载缓存如需一并删除，另执行第 6 节的清理命令。卸载 aqtinstall 不会自动删除已经安装的 Qt 和 OpenSSL。

Scoop、Git、7-Zip 和 FFmpeg 在配置前已存在。如果只撤销本次环境配置，应保留这些工具和 `C:\Users\30398\scoop\shims` 路径。

### 7.2 Qt 和 OpenSSL

这两项由 aqt 解压安装。关闭播放器、构建任务及使用它们的程序后，在资源管理器中删除对应目录：

- Qt：`D:\Qt\6.7.3\msvc2019_64`
- OpenSSL：`D:\Qt\Tools\OpenSSLv3`

然后从用户 `PATH` 删除两项 `bin` 路径。只删除上述目录，保留 `D:\Qt` 中其他版本或工具。程序输出目录中已复制的 DLL 需要随构建产物单独清理。

### 7.3 Visual Studio Build Tools 和 Windows SDK

打开 **Visual Studio Installer**，找到 **Visual Studio 生成工具 2022 / Build Tools 2022**，选择“更多 → 卸载”。这是首选方式，参见 [Microsoft 卸载说明](https://learn.microsoft.com/en-us/visualstudio/install/uninstall-visual-studio?view=vs-2022)。

也可以在管理员 PowerShell 中执行下面的命令；运行时不要处于 Installer 自身目录：

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe' uninstall --installPath 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools' --quiet --norestart
```

命令参数见 [Microsoft 安装器参数文档](https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio?view=vs-2022)。

Windows SDK 是共享组件。如果卸载 Build Tools 后它仍存在，并且其他项目也不需要，可在 Windows“设置 → 应用 → 已安装的应用”中卸载 **Windows Software Development Kit - Windows 10.0.26100.7705**。

Visual Studio Installer 也可能被其他 VS 实例使用。全部 VS 实例都已卸载后，再按 Windows 已安装应用中的对应条目卸载安装器。不要直接删除共享的 `Windows Kits` 或 `VisualStudio\Packages` 目录来代替卸载。

### 7.4 libmpv DLL

libmpv 没有系统安装器。关闭播放器后，如需移除本次补齐的运行时，可以删除这个文件：

```powershell
Remove-Item -LiteralPath 'D:\workspace\vibeEmbyPlayerQT\third_party\mpv\dev\libmpv-2.dll'
```

保留仓库已有的头文件、`libmpv.dll.a` 和依赖锁文件。以后需要恢复时，运行 `scripts\fetch-mpv-dev.ps1` 即可重新下载并校验。`build-clang` 中已部署的 DLL 副本需要另外清理。

### 7.5 路径和本次产物

在 Windows“编辑账户的环境变量 → 用户变量 → Path”中，删除已卸载组件对应的条目：

```text
C:\Users\30398\scoop\apps\llvm\current\bin
D:\Qt\6.7.3\msvc2019_64\bin
D:\Qt\Tools\OpenSSLv3\Win_x64\bin
```

保留仍在使用的工具路径，重新打开终端使变更生效。

关闭构建任务和播放器后，可通过资源管理器删除本次项目产物目录 `D:\workspace\vibeEmbyPlayerQT\build-clang` 和 `D:\workspace\vibeEmbyPlayerQT\build-environment`。后者包含日志、占用统计和路径备份，删除前可按需保存。系统原有的 `C:\Windows\System32\vulkan-1.dll` 不属于本次安装内容，保留即可。
