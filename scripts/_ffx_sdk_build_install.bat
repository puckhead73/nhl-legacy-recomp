@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "VULKAN_SDK=C:\VulkanSDK\1.4.350.0"
set "PATH=C:\Program Files\LLVM\bin;%VULKAN_SDK%\Bin;%PATH%"
set "BLD=E:\Tools\rexglue-sdk\src\out\build\win-amd64-ffx"
:: PERF: SHIP the Release config (-O3 -DNDEBUG, profiling stubbed) -> rexruntime.dll
:: (no "rd" suffix); package.ps1 maps the vk-pgo/vk-opt presets to Flavor="".
:: ALSO build/install RelWithDebInfo so the dev build (win-amd64-vk-ffx, which is
:: RelWithDebInfo and links snappyrd.lib) keeps working off the same install. Both
:: configs share REXGLUE_ENABLE_TRACY=OFF from the configure step.
cmake --build "%BLD%" --config Release
if errorlevel 1 exit /b 1
cmake --install "%BLD%" --config Release
if errorlevel 1 exit /b 1
cmake --build "%BLD%" --config RelWithDebInfo
if errorlevel 1 exit /b 1
cmake --install "%BLD%" --config RelWithDebInfo
