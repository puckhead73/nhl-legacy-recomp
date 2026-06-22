@echo off
REM Incremental build of ONLY the Release rexruntime.dll in the canonical
REM (no-mtune) SDK build dir, for Phase-2 A/B. Output:
REM E:\Tools\rexglue-sdk\src\out\win-amd64\Release\rexruntime.dll. No install.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "VULKAN_SDK=C:\VulkanSDK\1.4.350.0"
set "PATH=C:\Program Files\LLVM\bin;%VULKAN_SDK%\Bin;%PATH%"
cmake --build "E:\Tools\rexglue-sdk\src\out\build\win-amd64-ffx" --config Release --target rexruntime
