@echo off
REM PGO STAGE 1 (instrument): app build with -fprofile-generate to capture a
REM gameplay profile of the guest recomp. Links the (now ThinLTO) SDK. The
REM instrumented exe runs SLOWER (counter overhead) — that's expected; it only
REM needs to play a representative session, then Exit Game (which flushes the
REM profile via __llvm_profile_write_file, gated by NHL_PGO_INSTRUMENT).
REM Separate build dir; dev/opt builds untouched.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "VKSDK=E:/Tools/rexglue-sdk/src/out/install/win-amd64-ffx"
set "BDIR=e:/Repositories/nhl-legacy-recomp/out/build/win-amd64-vk-pgogen"
if "%1"=="configure" (
  REM Instrument on the SAME base the final build uses (Release + x86-64-v3) so the
  REM profile matches. NOTE: do NOT add -flto=thin here — ThinLTO + instrumentation
  REM slows the profiling build and is unnecessary (the profile is consumed by the
  REM Stage-2 LTO build). The instrumented exe runs slower by design.
  cmake -S e:/Repositories/nhl-legacy-recomp -B %BDIR% -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v3 -fprofile-generate" ^
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v3 -fprofile-generate -DNHL_PGO_INSTRUMENT" ^
    -DCMAKE_EXE_LINKER_FLAGS="-fprofile-generate" ^
    -DCMAKE_PREFIX_PATH=%VKSDK% ^
    -DNHLLEGACY_VULKAN_BACKEND=ON ^
    -DNHLLEGACY_BUILD_PACKAGER=OFF -DNHLLEGACY_BUILD_TRACE_TOOLS=OFF
  echo CONFIGURE_EXIT=%ERRORLEVEL%
) else (
  cmake --build %BDIR% --target nhllegacy
  echo BUILD_EXIT=%ERRORLEVEL%
)
