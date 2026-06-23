@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%

"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" ^
  -S . ^
  -B build-clang ^
  -G Ninja ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_PREFIX_PATH=D:/Qt/6.7.3/msvc2019_64 ^
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_MAKE_PROGRAM="C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
