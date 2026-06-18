@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "VULKAN_SDK=C:\VulkanSDK\1.4.350.0"
set "PATH=C:\Program Files\LLVM\bin;%VULKAN_SDK%\Bin;%PATH%"
REM -fuse-ld=lld is REQUIRED now that the SDK is built with -flto=thin: its static
REM libs (e.g. snappyrd.lib) are LLVM bitcode, which the default MSVC link.exe can't
REM consume but lld can. (The shipping vk-pgo/vk-opt builds already pass it.)
cmake -S "E:\Repositories\nhl-legacy-recomp" -B "out\build\win-amd64-vk-ffx" ^
  -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" ^
  "-DCMAKE_PREFIX_PATH=E:/Tools/rexglue-sdk/src/out/install/win-amd64-ffx" ^
  -DNHLLEGACY_VULKAN_BACKEND=ON
if errorlevel 1 exit /b 1
cmake --build "out\build\win-amd64-vk-ffx" --target nhllegacy
