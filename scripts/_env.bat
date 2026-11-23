@echo off
REM Shared build environment for all scripts/ drivers. Every location can be
REM overridden by setting the variable before calling a script; the defaults
REM assume the layout created by scripts\setup_sdk.ps1 (SDK vendored under
REM third_party\rexglue-sdk inside this repo).
REM
REM Variables:
REM   REPO_ROOT            this repository (derived from this file's location)
REM   REXGLUE_SDK_SRC      ReXGlue SDK source tree (pinned + patched)
REM   REXGLUE_SDK_BUILD    SDK build dir   (win-amd64-ffx, Ninja Multi-Config)
REM   REXGLUE_SDK_INSTALL  SDK install prefix the game links against
REM   REXGLUE_FFX_PREBUILT prebuilt amd_fidelityfx_vk.dll/.lib (VS-generator build)
REM   VULKAN_SDK           LunarG Vulkan SDK
REM   VCVARS64             VS2022 vcvars64.bat (auto-discovered if unset)

for %%i in ("%~dp0..") do set "REPO_ROOT=%%~fi"
if not defined REXGLUE_SDK_SRC set "REXGLUE_SDK_SRC=%REPO_ROOT%\third_party\rexglue-sdk"
set "REXGLUE_SDK_BUILD=%REXGLUE_SDK_SRC%\out\build\win-amd64-ffx"
set "REXGLUE_SDK_INSTALL=%REXGLUE_SDK_SRC%\out\install\win-amd64-ffx"
set "REXGLUE_FFX_PREBUILT=%REXGLUE_SDK_SRC%\out\ffx-prebuilt\vk"

REM Vulkan SDK: honor an existing VULKAN_SDK env var, else pick the newest
REM installed under C:\VulkanSDK.
if not defined VULKAN_SDK (
  for /d %%v in ("C:\VulkanSDK\*") do set "VULKAN_SDK=%%~fv"
)
if not defined VULKAN_SDK (
  echo [env] ERROR: Vulkan SDK not found ^(install LunarG Vulkan SDK or set VULKAN_SDK^)
  exit /b 1
)

REM MSVC environment: BuildTools first, then Community/Professional/Enterprise,
REM both Program Files flavors.
if not defined VCVARS64 (
  for %%e in (BuildTools Community Professional Enterprise) do (
    if not defined VCVARS64 if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=C:\Program Files (x86)\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
    if not defined VCVARS64 if exist "C:\Program Files\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS64=C:\Program Files\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
  )
)
REM Fall back to VS 2026 (v18) when no 2022 install is present.
if not defined VCVARS64 (
  for %%e in (BuildTools Community Professional Enterprise) do (
    if not defined VCVARS64 if exist "C:\Program Files\Microsoft Visual Studio\18\%%e\VC\Auxiliary\Build\vcvars64.bat" (
      set "VCVARS64=C:\Program Files\Microsoft Visual Studio\18\%%e\VC\Auxiliary\Build\vcvars64.bat"
      if not defined FFX_VS_GENERATOR set "FFX_VS_GENERATOR=Visual Studio 18 2026"
    )
  )
)
if not defined FFX_VS_GENERATOR set "FFX_VS_GENERATOR=Visual Studio 17 2022"
if not defined VCVARS64 (
  echo [env] ERROR: Visual Studio 2022 vcvars64.bat not found ^(install VS2022 Build Tools with the C++ workload, or set VCVARS64^)
  exit /b 1
)
call "%VCVARS64%" >nul
if errorlevel 1 exit /b 1

REM Prefer a standalone LLVM (clang 20+) when present; vcvars puts the VS-bundled
REM clang on PATH otherwise.
if exist "C:\Program Files\LLVM\bin\clang.exe" set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"
exit /b 0
