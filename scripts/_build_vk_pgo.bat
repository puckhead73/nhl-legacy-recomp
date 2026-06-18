@echo off
REM PGO STAGE 2 (use): final optimized app build with -fprofile-use, guided by the
REM gameplay profile captured from the instrumented build (Stage 1). Release base
REM (-O3 -DNDEBUG) + -march=x86-64-v3 + ThinLTO, linking the Release+ThinLTO FFX SDK
REM (rexruntime.dll, no "rd" suffix). ThinLTO is per-binary: this whole-program-opts
REM the exe incl. the ~13.4M-line recompiled guest code (the guest-CPU hot path).
REM Expects the merged profile at pgo\nhllegacy.profdata (produced by:
REM llvm-profdata merge -output=... *.profraw). Re-capture the profile after any
REM base/flag change (Stage 1 -> play a session -> merge). Separate build dir;
REM dev/opt/pgogen builds untouched.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "VKSDK=E:/Tools/rexglue-sdk/src/out/install/win-amd64-ffx"
set "BDIR=e:/Repositories/nhl-legacy-recomp/out/build/win-amd64-vk-pgo"
set "PROFDATA=e:/Repositories/nhl-legacy-recomp/pgo/nhllegacy.profdata"
if "%1"=="configure" (
  cmake -S e:/Repositories/nhl-legacy-recomp -B %BDIR% -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v3 -flto=thin -fprofile-use=%PROFDATA% -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" ^
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=x86-64-v3 -flto=thin -fprofile-use=%PROFDATA% -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" ^
    -DCMAKE_EXE_LINKER_FLAGS="-flto=thin -fuse-ld=lld" ^
    -DCMAKE_PREFIX_PATH=%VKSDK% ^
    -DNHLLEGACY_VULKAN_BACKEND=ON ^
    -DNHLLEGACY_BUILD_PACKAGER=ON -DNHLLEGACY_BUILD_TRACE_TOOLS=OFF
  echo CONFIGURE_EXIT=%ERRORLEVEL%
) else (
  REM Build both the port and the installer so they link the SAME Vulkan runtime
  REM DLL (the ThinLTO rexruntimerd.dll), required for the Vulkan-primary release.
  cmake --build %BDIR% --target nhllegacy
  cmake --build %BDIR% --target nhl-legacy-builder
  echo BUILD_EXIT=%ERRORLEVEL%
)
