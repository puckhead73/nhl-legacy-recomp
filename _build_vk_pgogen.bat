@echo off
REM PGO STAGE 1 (instrument): app build with -fprofile-generate to capture a
REM gameplay profile of the guest recomp. Links the (now ThinLTO) SDK. The
REM instrumented exe runs SLOWER (counter overhead) — that's expected; it only
REM needs to play a representative session, then Exit Game (which flushes the
REM profile via __llvm_profile_write_file, gated by NHL_PGO_INSTRUMENT).
REM Separate build dir; dev/opt builds untouched.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "VKSDK=E:/Tools/rexglue-sdk/src/out/install/win-amd64"
set "BDIR=e:/Repositories/nhl-legacy-recomp/out/build/win-amd64-vk-pgogen"
if "%1"=="configure" (
  cmake -S e:/Repositories/nhl-legacy-recomp -B %BDIR% -G Ninja ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O3 -DNDEBUG -march=native -fprofile-generate" ^
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O3 -DNDEBUG -march=native -fprofile-generate -DNHL_PGO_INSTRUMENT" ^
    -DCMAKE_EXE_LINKER_FLAGS="-fprofile-generate" ^
    -DCMAKE_PREFIX_PATH=%VKSDK% ^
    -DNHLLEGACY_VULKAN_BACKEND=ON ^
    -DNHLLEGACY_BUILD_PACKAGER=OFF -DNHLLEGACY_BUILD_TRACE_TOOLS=OFF
  echo CONFIGURE_EXIT=%ERRORLEVEL%
) else (
  cmake --build %BDIR% --target nhllegacy
  echo BUILD_EXIT=%ERRORLEVEL%
)
