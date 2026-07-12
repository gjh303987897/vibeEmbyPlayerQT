@echo off
setlocal

set "VS_ROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%i"
)

if not defined VS_ROOT if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VS_ROOT=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VS_ROOT if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VS_ROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"

if not defined VS_ROOT (
  echo Visual Studio 2022 C++ tools were not found.
  exit /b 1
)

call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%

set "CMAKE_EXE=%USERPROFILE%\scoop\shims\cmake.exe"
if not exist "%CMAKE_EXE%" set "CMAKE_EXE=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE_EXE%" set "CMAKE_EXE=cmake"

"%CMAKE_EXE%" ^
  --build build-clang ^
  --parallel
