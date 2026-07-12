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

set "NINJA_EXE=%USERPROFILE%\scoop\shims\ninja.exe"
if not exist "%NINJA_EXE%" set "NINJA_EXE=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%NINJA_EXE%" set "NINJA_EXE=ninja"

set "CLANG_CL=C:/Program Files/LLVM/bin/clang-cl.exe"
if not exist "%CLANG_CL%" set "CLANG_CL=clang-cl"

"%CMAKE_EXE%" ^
  -S . ^
  -B build-clang ^
  -G Ninja ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_PREFIX_PATH=D:/Qt/6.7.3/msvc2019_64 ^
  -DCMAKE_C_COMPILER="%CLANG_CL%" ^
  -DCMAKE_CXX_COMPILER="%CLANG_CL%" ^
  -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%"
