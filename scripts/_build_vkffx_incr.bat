@echo off
call "%~dp0_env.bat" || exit /b 1
cmake --build "out\build\win-amd64-vk-ffx" --target nhllegacy
