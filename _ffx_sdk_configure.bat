@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "VULKAN_SDK=C:\VulkanSDK\1.4.350.0"
set "PATH=C:\Program Files\LLVM\bin;%VULKAN_SDK%\Bin;%PATH%"
set "BLD=E:\Tools\rexglue-sdk\src\out\build\win-amd64-ffx"
:: Clear the poisoned cache (CMAKE_GENERATOR_PLATFORM) but keep _deps (FidelityFX clone).
if exist "%BLD%\CMakeCache.txt" del /q "%BLD%\CMakeCache.txt"
if exist "%BLD%\CMakeFiles" rmdir /s /q "%BLD%\CMakeFiles"
cmake -S "E:\Tools\rexglue-sdk\src" -B "%BLD%" ^
  -G "Ninja Multi-Config" ^
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
  "-DCMAKE_C_FLAGS=-march=x86-64-v3" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3" ^
  -DCMAKE_CXX_STANDARD=23 ^
  "-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo" ^
  "-DCMAKE_CROSS_CONFIGS=all" "-DCMAKE_DEFAULT_CONFIGS=all" -DCMAKE_DEFAULT_BUILD_TYPE=RelWithDebInfo ^
  "-DCMAKE_INSTALL_PREFIX=E:\Tools\rexglue-sdk\src\out\install\win-amd64-ffx" ^
  -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=ON ^
  -DREXGLUE_ENABLE_FIDELITYFX=ON -DREXGLUE_FIDELITYFX_BACKEND=vk ^
  "-DREXGLUE_FIDELITYFX_PREBUILT_DIR=E:\Tools\rexglue-sdk\src\out\ffx-prebuilt\vk" ^
  -DREXGLUE_ENABLE_TRACY=ON -DREXGLUE_ENABLE_PERF_COUNTERS=ON
