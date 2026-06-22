@echo off
REM Set up the build environment for RexGlue: MSVC (Windows SDK + libs) via
REM VsDevCmd, plus LLVM clang/clang++ and VS-bundled ninja on PATH.
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
