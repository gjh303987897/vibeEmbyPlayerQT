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
.\build-clang\vibePlayerQT.exe
```

## Verification

The Debug build was compiled successfully and the deployed executable was smoke-tested by launching it for several seconds. The process remained running, which confirms the executable can start and load its Qt/QML runtime dependencies.

The Clang Debug build was also compiled successfully with `clang-cl`, deployed with `windeployqt`, and smoke-tested the same way.
