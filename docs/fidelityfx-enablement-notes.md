# FidelityFX (FSR/CAS) enablement — WORKING (prebuilt-libs approach)

**Status (2026-06-17):** ENABLED. The overlay FSR/CAS controls are live in the
`win-amd64-vk-ffx` game build. The in-tree FidelityFX build is incompatible with
the SDK's clang/Ninja toolchain (see "blocker" below), so FidelityFX is built once
with the VS generator and **imported as a prebuilt DLL**. The known-good
`win-amd64-vk` game build + `win-amd64` SDK install are untouched.

## Working build chain (reproducible)

Scratch batch files in the repo root drive each step (Vulkan SDK 1.4.350 + VS2022
BuildTools + LLVM required):

1. `_ffx_dll_build.bat` — builds `amd_fidelityfx_vk.dll` + `.lib` with the **VS 2022
   generator** (`-G "Visual Studio 17 2022" -A x64 -DFFX_API_BACKEND=VK_X64`). Output
   stashed to `E:\Tools\rexglue-sdk\src\out\ffx-prebuilt\vk\`.
2. `_ffx_sdk_configure.bat` — configures the SDK (clang/Ninja-MC) with
   `-DREXGLUE_ENABLE_FIDELITYFX=ON -DREXGLUE_FIDELITYFX_BACKEND=vk
   -DREXGLUE_FIDELITYFX_PREBUILT_DIR=...\out\ffx-prebuilt\vk`, install prefix
   `out\install\win-amd64-ffx`.
3. `_ffx_sdk_build_install.bat` — builds + installs the SDK (RelWithDebInfo).
4. `_game_ffx_build.bat` — configures+builds the game in `out\build\win-amd64-vk-ffx`
   with `CMAKE_PREFIX_PATH` → the FFX SDK prefix. Then copy
   `amd_fidelityfx_vk.dll` next to `nhllegacy.exe` (DELAYLOAD'd at runtime).

## SDK source patches required (in E:\Tools\rexglue-sdk\src)

- `cmake/rexglue_fidelityfx.cmake` — new `REXGLUE_FIDELITYFX_PREBUILT_DIR` option:
  when set, imports the prebuilt `amd_fidelityfx_<backend>` as an IMPORTED SHARED
  target instead of `add_subdirectory(ffx-api)` (the clang/Ninja blocker).
- `cmake/rexglue_install.cmake` — install the IMPORTED FFX DLL/lib as files
  (can't `install(TARGETS)` an imported target; it's DELAYLOAD'd, only the DLL ships).
- `src/system/CMakeLists.txt` — export `REX_HAS_FIDELITYFX_SDK=1` **PUBLIC** on
  `rexruntime` (it was PRIVATE on `rexui`). REQUIRED: presenter.h changes
  Presenter/GuestOutputPaintConfig layout under that define, so the game must
  compile with the same define as the SDK binary, or the ABI disagrees. Also drives
  the overlay's FSR/CAS guard.

## Still to verify (needs a live run)

That FSR/CAS actually render — set the overlay "Effect" to cas/fsr, restart, observe.
The build links and the DLL loads; visual confirmation is user-side.

---

## (Historical) the in-tree-build blocker

## What's done (repo side, shipped)

- `renderer/core/nhl_overlay.cpp` — "Upscaling & Sharpening (FidelityFX)" section:
  effect combo (bilinear/cas/fsr/fsr2/fsr3) + CAS/FSR sharpness sliders. Guarded by
  `#if defined(REX_HAS_FIDELITYFX_SDK)`; `#else` shows a "rebuild the SDK" note.
- `renderer/core/nhl_settings.h` — `ffx_effect` / `ffx_cas_sharpness` / `ffx_fsr_sharpness`
  persisted to `nhl_enhancements.ini`.
- `src/nhllegacy_app.h` — `OnPreSetup` applies the persisted FFX cvars at launch
  (they're restart-required SDK cvars).

The SDK present-time scaler is entirely cvar-driven (`present_effect`,
`present_cas_additional_sharpness`, `present_fsr_sharpness_reduction`,
`present_fsr_quality_mode`), so once the define is present, no further code is needed.

## The blocker — FidelityFX-SDK assumes MSVC + Visual Studio generator

Our SDK + game build with **clang + Ninja Multi-Config**. The fetched FidelityFX-SDK
(`github.com/rexglue/FidelityFX-SDK`, pinned) build scripts assume MSVC + the VS
generator and fail under Ninja/clang:

1. `sdk/toolchain.cmake:60` force-sets `CMAKE_GENERATOR_PLATFORM=x64` → Ninja errors
   "generator does not support platform specification".
2. Calls `vswhere.exe` (present at `C:\Program Files (x86)\Microsoft Visual Studio\Installer\`
   but not on PATH).
3. `sdk/libs/pix/CMakeLists.txt` resolves PIX libs via `${CMAKE_GENERATOR_PLATFORM}`
   (empty under Ninja → broken path).
4. `toolchain.cmake:46` injects MSVC-only flags (`/W3 /GR- /fp:fast /GS /Gy`) that
   plain `clang++` rejects.

These are independent failure points; making it build is a porting effort, not a flag.

## Configure recipe that got furthest (for whoever resumes)

Prereqs: VS2022 BuildTools vcvars64, LLVM on PATH, `VULKAN_SDK=C:\VulkanSDK\1.4.350.0`
(its `find_package(Vulkan)` needs it; the main SDK uses vendored volk and doesn't).

```
cmake -S E:\Tools\rexglue-sdk\src -B E:\Tools\rexglue-sdk\src\out\build\win-amd64-ffx ^
  -G "Ninja Multi-Config" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
  "-DCMAKE_C_FLAGS=-march=x86-64-v3" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3" -DCMAKE_CXX_STANDARD=23 ^
  "-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo" ^
  "-DCMAKE_INSTALL_PREFIX=E:\Tools\rexglue-sdk\src\out\install\win-amd64-ffx" ^
  -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=ON ^
  -DREXGLUE_ENABLE_FIDELITYFX=ON -DREXGLUE_FIDELITYFX_BACKEND=vk ^
  -DREXGLUE_ENABLE_TRACY=ON -DREXGLUE_ENABLE_PERF_COUNTERS=ON
```

After a working install: build+install the **RelWithDebInfo** config (the game build
links the `rd` SDK variant), then repoint the game's `win-amd64-vk` build
`CMAKE_PREFIX_PATH` → `...\out\install\win-amd64-ffx` and rebuild. The `REX_HAS_FIDELITYFX_SDK`
interface compile-def then propagates and the overlay section lights up.

## Possible paths to unblock (pick one)

- **Patch the FidelityFX fetch** in `E:\Tools\rexglue-sdk\src\cmake\rexglue_fidelityfx.cmake`:
  after populate, guard `toolchain.cmake`'s platform/flags blocks to VS-only and fix the
  PIX path. Fragile (re-fetch wipes the tree unless patched via the glue).
- **Build FidelityFX with a VS generator** in a separate tree and import its static libs +
  DLLs as prebuilt, bypassing the in-tree FetchContent build.
- **Wait for a newer rexglue SDK** where the FidelityFX glue is Ninja/clang-clean.

Partial build dir (FidelityFX already cloned, ~695 MB) left at
`E:\Tools\rexglue-sdk\src\out\build\win-amd64-ffx` to avoid re-cloning.
