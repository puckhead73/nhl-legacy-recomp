@echo off
REM Optimized Vulkan build (PERF: docs/vulkan-enhancements / profile verdict =
REM dense gameplay is CPU/draw-submission bound). Separate build dir so the dev
REM RelWithDebInfo build (win-amd64-vk) stays intact for iteration.
REM
REM This script: Release (-O3 -DNDEBUG, drops -g + asserts) + -march=x86-64-v3
REM   (AVX2/BMI/MOVBE codegen for the recomp's byte-swap/FP hot paths) + ThinLTO
REM   (-flto=thin + lld; whole-program opt over the ~13.4M-line recompiled guest
REM   code). NOTE: use a PORTABLE arch, NOT -march=native. native on an AMD dev box
REM   enables SSE4a, and LLVM then emits EXTRQ/INSERTQ (66 0F 78), which #UD ->
REM   0xC000001D on Intel CPUs (confirmed from an end-user crash dump). x86-64-v3
REM   matches the SDK's FFX build. PGO (the bigger lever) layers on via the
REM   _build_vk_pgogen.bat -> _build_vk_pgo.bat flow.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "VKSDK=E:/Tools/rexglue-sdk/src/out/install/win-amd64-ffx"
set "BDIR=e:/Repositories/nhl-legacy-recomp/out/build/win-amd64-vk-opt"
if "%1"=="configure" (
  REM Base config stays RelWithDebInfo so the prebuilt SDK's "rd"-suffixed imported
  REM libs (snappyrd.lib, etc.) resolve and the perf-instrumentation ABI matches the
  REM SDK binary. We just OVERRIDE the RelWithDebInfo flags to Release-grade codegen:
  REM -O3 (was -O2), keep -DNDEBUG (asserts off), drop -g (no debug info), and add
  REM -march=x86-64-v3 (AVX2/BMI/MOVBE for the recomp's byte-swap/FP hot paths).
  cmake -S e:/Repositories/nhl-legacy-recomp -B %BDIR% -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v3 -flto=thin" ^
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v3 -flto=thin" ^
    -DCMAKE_EXE_LINKER_FLAGS="-flto=thin -fuse-ld=lld" ^
    -DCMAKE_PREFIX_PATH=%VKSDK% ^
    -DNHLLEGACY_VULKAN_BACKEND=ON ^
    -DNHLLEGACY_BUILD_PACKAGER=OFF -DNHLLEGACY_BUILD_TRACE_TOOLS=OFF
  echo CONFIGURE_EXIT=%ERRORLEVEL%
) else (
  cmake --build %BDIR% --target nhllegacy
  echo BUILD_EXIT=%ERRORLEVEL%
)
