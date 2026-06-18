@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "VULKAN_SDK=C:\VulkanSDK\1.4.350.0"
set "PATH=C:\Program Files\LLVM\bin;%VULKAN_SDK%\Bin;%PATH%"
set "BLD=E:\Tools\rexglue-sdk\src\out\build\win-amd64-ffx"
cmake --build "%BLD%" --config RelWithDebInfo
if errorlevel 1 exit /b 1
cmake --install "%BLD%" --config RelWithDebInfo
