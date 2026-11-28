@echo off
call "%~dp0_env.bat" || exit /b 1
set "FFXAPI=%REXGLUE_SDK_BUILD%\_deps\fidelityfx-src\ffx-api"
REM Build OUT-OF-TREE in a SHORT path: the FFX shader-permutation rule files
REM nest ~145 chars below the build dir, and MSBuild hard-fails past MAX_PATH
REM (260). Under third_party\rexglue-sdk\out\...\_deps the in-tree build_vk
REM blows the limit; %USERPROFILE% keeps it well under.
if not defined FFX_BUILD_DIR set "FFX_BUILD_DIR=%USERPROFILE%\_ffxbuild\vk"
cmake -S "%FFXAPI%" -B "%FFX_BUILD_DIR%" -G "%FFX_VS_GENERATOR%" -A x64 ^
  -DFFX_API_BACKEND=VK_X64 -DFFX_API_AUTO_COMPILE_SHADERS=ON
if errorlevel 1 exit /b 1
cmake --build "%FFX_BUILD_DIR%" --config Release --parallel
if errorlevel 1 exit /b 1

REM Stage the DLL + import lib where _ffx_sdk_configure.bat's
REM REXGLUE_FIDELITYFX_PREBUILT_DIR expects them, so the SDK reconfigure picks
REM up the prebuilt import path instead of the (clang/Ninja-incompatible)
REM in-tree FidelityFX build.
if not exist "%REXGLUE_FFX_PREBUILT%" mkdir "%REXGLUE_FFX_PREBUILT%"
REM The FFX CMake emits the DLL to <ffx-api>\bin (source-relative), not the
REM build dir - search both.
set "FFX_FOUND="
for /r "%FFXAPI%\bin" %%f in (amd_fidelityfx_vk.dll) do (
  if exist "%%f" if not defined FFX_FOUND (
    set "FFX_FOUND=1"
    copy /y "%%f" "%REXGLUE_FFX_PREBUILT%\" >nul
    if exist "%%~dpnf.lib" copy /y "%%~dpnf.lib" "%REXGLUE_FFX_PREBUILT%\" >nul
    echo staged %%f -^> %REXGLUE_FFX_PREBUILT%
  )
)
for /r "%FFX_BUILD_DIR%" %%f in (amd_fidelityfx_vk.dll) do (
  if exist "%%f" if not defined FFX_FOUND (
    set "FFX_FOUND=1"
    copy /y "%%f" "%REXGLUE_FFX_PREBUILT%\" >nul
    if exist "%%~dpnf.lib" copy /y "%%~dpnf.lib" "%REXGLUE_FFX_PREBUILT%\" >nul
    echo staged %%f -^> %REXGLUE_FFX_PREBUILT%
  )
)
if not defined FFX_FOUND (
  echo ERROR: amd_fidelityfx_vk.dll not found under %FFX_BUILD_DIR%
  exit /b 1
)
if not exist "%REXGLUE_FFX_PREBUILT%\amd_fidelityfx_vk.lib" (
  echo WARNING: amd_fidelityfx_vk.lib not found next to the DLL - locate it under
  echo   %FFX_BUILD_DIR% and copy it to %REXGLUE_FFX_PREBUILT% manually.
)
