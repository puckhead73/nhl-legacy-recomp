@echo off
REM PGO STAGE 2 (use): final optimized app build with -fprofile-use, guided by the
REM gameplay profile captured from the instrumented build (Stage 1). On top of
REM -O3 -march=native, and links the ThinLTO SDK. Expects the merged profile at
REM pgo\nhllegacy.profdata (produced by: llvm-profdata merge -output=... *.profraw).
REM Separate build dir; dev/opt/pgogen builds untouched.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "VKSDK=E:/Tools/rexglue-sdk/src/out/install/win-amd64"
set "BDIR=e:/Repositories/nhl-legacy-recomp/out/build/win-amd64-vk-pgo"
set "PROFDATA=e:/Repositories/nhl-legacy-recomp/pgo/nhllegacy.profdata"
if "%1"=="configure" (
  cmake -S e:/Repositories/nhl-legacy-recomp -B %BDIR% -G Ninja ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O3 -DNDEBUG -march=native -fprofile-use=%PROFDATA% -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" ^
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O3 -DNDEBUG -march=native -fprofile-use=%PROFDATA% -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" ^
    -DCMAKE_PREFIX_PATH=%VKSDK% ^
    -DNHLLEGACY_VULKAN_BACKEND=ON ^
    -DNHLLEGACY_BUILD_PACKAGER=OFF -DNHLLEGACY_BUILD_TRACE_TOOLS=OFF
  echo CONFIGURE_EXIT=%ERRORLEVEL%
) else (
  cmake --build %BDIR% --target nhllegacy
  echo BUILD_EXIT=%ERRORLEVEL%
)
