# Windows Build Notes

## Installed Tooling

This workspace has been verified with:

- Visual Studio 2022 Community MSVC toolchain
- CMake from Visual Studio: `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- LLVM: `C:\Program Files\LLVM\bin\clang-cl.exe`
- Ninja from Visual Studio: `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe`
- Qt installed by `aqtinstall`
- Qt path: `D:\Qt\6.7.3\msvc2019_64`

## MSVC Configure

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  -S . `
  -B build `
  -G 'Visual Studio 17 2022' `
  -A x64 `
  -DCMAKE_PREFIX_PATH='D:\Qt\6.7.3\msvc2019_64'
```

## Build

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --build build `
  --config Debug `
  --parallel
```

## Deploy Runtime Dependencies

```powershell
& 'D:\Qt\6.7.3\msvc2019_64\bin\windeployqt.exe' `
  --debug `
  --qmldir qml `
  build\Debug\vibePlayerQT.exe
```

## Run

```powershell
.\build\Debug\vibePlayerQT.exe
```

## Clang Configure

The recommended Windows Clang setup uses `clang-cl`, so it remains ABI-compatible with the MSVC Qt package.

```powershell
.\scripts\configure-clang.cmd
```

The script creates `build-clang` with:

- Generator: Ninja
- Compiler: `clang-cl`
- Build type: Debug
- Qt prefix: `D:\Qt\6.7.3\msvc2019_64`

## Clang Build

```powershell
.\scripts\build-clang.cmd
```

## Clang Deploy Runtime Dependencies

```powershell
& 'D:\Qt\6.7.3\msvc2019_64\bin\windeployqt.exe' `
  --debug `
  --qmldir qml `
  build-clang\vibePlayerQT.exe
```

## Clang Run

```powershell
.\scripts\run-desktop.ps1
```

The launcher supplies the local Qt/OpenSSL paths to the child process and uses
the executable directory as its working directory. Its `-Executable`, `-QtRoot`
and `-OpenSslBin` parameters support other build/runtime locations. It restores
the calling PowerShell environment after launch and does not alter user or
system settings.

### Intel D3D11 startup crash workaround

If the application crashes immediately with a memory-read error, use:

```powershell
.\scripts\run-desktop.ps1 -SoftwareRendering
```

On 2026-10-08, the local Intel driver `27.20.100.9565` crashed at
`igd10iumd64.dll+0x291660`, reading address `0xE8`, while Qt Quick initialized
its D3D11 rendering path. A native debug-event capture reproduced the exact
instruction address in the screenshot. A process merely remaining alive or
briefly displaying a window is insufficient to rule out this crash.

The same native debug-event capture observed a WARP launch for 90 seconds with
no access violation. The desktop was then launched in the normal user environment
with the compatibility option. This verifies startup for the reported machine;
it is not a video-playback or long-duration performance test.

The compatibility launch selects D3D11 WARP using
`QSG_RHI_BACKEND=d3d11` and `QSG_RHI_PREFER_SOFTWARE_RENDERER=1`, as described
in [Qt's Windows graphics documentation](https://doc.qt.io/qt-6.8/windows-graphics.html).
WARP renders the Qt interface on the CPU while preserving RHI shader effects;
animations may cost more CPU. This does not change libmpv's separate video
renderer or hardware-decoding configuration and does not repair the Intel
driver. Hardware rendering remains the default for other launches. Do not set
these variables globally unless you intend to affect other Qt applications.

## QML Runtime Packaging

`qt_add_qml_module` must include every QML type instantiated by `Main.qml` in
`QML_FILES`. The current module includes `ImageCacheManager`, its dynamically
loaded `ImagePreloader`, `DataProcessManager`, and `BindingOptimizer`.
`DataWorker.mjs` is embedded as a resource at the same relative path as
`DataProcessManager.qml` so `WorkerScript` can load it.

If the GUI executable exits before a window appears, set `VIBEPLAYER_LOG_FILE`
to a writable absolute path before launching it. Startup QML warnings and root
creation failures are written there.

## Verification

The Debug build was compiled successfully and the deployed executable was smoke-tested by launching it for several seconds. The process remained running, which confirms the executable can start and load its Qt/QML runtime dependencies.

The Clang Debug build was also compiled successfully with `clang-cl`, deployed with `windeployqt`, and smoke-tested the same way.
