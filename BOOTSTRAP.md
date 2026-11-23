# Bootstrapping this fork from a clean machine

This fork removes the original developer's machine-specific paths (`E:\Tools\...`,
`E:\Repositories\...`, `H:\...`) and makes the ReXGlue SDK dependency reproducible
from public source. Everything below was verified against the public SDK repo on
2026-07-06.

## What you need

| Piece | Where | Notes |
|---|---|---|
| ReXGlue SDK source | `scripts/setup_sdk.ps1` fetches it | Public: <https://github.com/rexglue/rexglue-sdk> (BSD-3). Pinned to `bd9b5191` (= v0.8.1.31-dev, nightly-20260603) — the exact snapshot this port was developed against. |
| Title patch for the SDK | `sdk/rexglue-vulkan-nhl-legacy-bd9b519.patch` | Applied by `setup_sdk.ps1`. Regenerated from the original `docs/rexglue-vulkan-nhl-legacy.patch` to apply cleanly to the public `bd9b5191` tree (one include-list hunk fixed). Contains all title-specific SDK fixes **including the `NHL_VK_READBACK_MODE` perf override**. |
| Vanilla `default.xex` | you provide | Any folder with the vanilla xex works, e.g. the `game\` dir of an existing builder install (the builder hash-validates it). Set `game_root`/`file_path` in `nhllegacy_manifest.toml`. |
| VS2022 Build Tools | <https://aka.ms/vs/17/release/vs_BuildTools.exe> | "Desktop development with C++" workload. |
| LLVM/Clang 20+ | <https://releases.llvm.org> | Standalone LLVM preferred (`C:\Program Files\LLVM`); `scripts/_env.bat` picks it up automatically. |
| CMake 3.25+ / Ninja | winget or installers | Must be on PATH. |
| Vulkan SDK | <https://vulkan.lunarg.com> | Dev used 1.4.350.0. `_env.bat` auto-detects any version under `C:\VulkanSDK`. |

All build scripts now source `scripts/_env.bat`, which discovers vcvars64 /
Vulkan SDK / LLVM and defines `REPO_ROOT`, `REXGLUE_SDK_SRC`, `REXGLUE_SDK_BUILD`,
`REXGLUE_SDK_INSTALL`, `REXGLUE_FFX_PREBUILT`. Override any of them as env vars
to relocate things.

## Build order (first time)

Run everything from the repo root in a normal `cmd`/PowerShell (the scripts set
up the MSVC environment themselves):

```bat
:: 1. Fetch + pin + patch the SDK, and the vendored high-cut deps.
powershell -ExecutionPolicy Bypass -File scripts\setup_sdk.ps1

:: 2. Configure the SDK. The FIRST run also downloads FidelityFX into
::    _deps/ and may fail at the FidelityFX step - that is expected: the
::    upstream FFX CMake needs the Visual Studio generator, not clang+Ninja.
scripts\_ffx_sdk_configure.bat

:: 3. Build the FidelityFX DLL once with the VS generator and stage it into
::    the prebuilt dir the SDK configure looks for.
scripts\_ffx_dll_build.bat

:: 4. Re-configure (now imports the prebuilt FFX) + build + install the SDK.
scripts\_ffx_sdk_configure.bat
scripts\_ffx_sdk_build_install.bat

:: 5. Point nhllegacy_manifest.toml at your vanilla default.xex, then run
::    codegen to emit generated/ (~181 recompiled .cpp files + the CMake
::    boilerplate the game configure include()s - so this MUST run before
::    the first _game_ffx_build.bat).
third_party\rexglue-sdk\out\install\win-amd64-ffx\bin\rexglue.exe codegen nhllegacy_manifest.toml

:: 6. Configure + build the game.
scripts\_game_ffx_build.bat
:: (after manifest/XEX changes, refresh with:
::  cmake --build out\build\win-amd64-vk-ffx --target nhllegacy_codegen)
```

Dev-build smoke test (Vulkan path is the only supported renderer):

```bat
set NHL_VK_BACKEND=1
out\build\win-amd64-vk-ffx\nhllegacy.exe --game_data_root "<your install>\game"
```

**Movies show green blocks?** The VP6 host-decode bridge needs `ffmpeg.exe`
(on PATH or beside the exe; `release\fetch_ffmpeg.ps1` fetches one) and the
movies as loose files under `<game>\_compiled\`. A raw disc extract (e.g. via
extract-xiso) only has the `.big` archives, and the builder's QuickBMS unpacker
comes from a private repo, so pull just the movies out with:

```bat
python tools\extract_big_movies.py "<game>" "<game>\_compiled"
```

(Add `--dry-run` to list them first: 54 `.vp6`/`.sub` files, ~700 MB.)

For an optimized (shipping-grade, non-PGO) build: `scripts\_build_vk_opt.bat
configure` then `scripts\_build_vk_opt.bat`. PGO flow: `_build_vk_pgogen.bat` →
play a session → `llvm-profdata merge -output=pgo\nhllegacy.profdata *.profraw` →
`_build_vk_pgo.bat`.

## Known deltas vs the original dev tree

- **Solved:** the original SDK tree's unexplained `-dirty` state on the
  `libmspack`/`o1heap` submodules is Windows git checking out their git
  *symlinks* as plain text files (libmspack's `cabextract/mspack/*.{c,h}` point
  at `../../libmspack/mspack/`); the build fails compiling a "file" that
  contains only a path. `setup_sdk.ps1` now materializes the symlink targets
  automatically — the resulting working-tree diff is the same class of change
  the dev tree carried.
- **Color grade is a no-op in rebuilt runtimes:** the SDK-side `present_grade_*`
  compute pass (overlay "Lighting / Color Grade") was never captured in the
  title patch. `src/present_grade_compat.cpp` defines the cvars so the game
  links; the overlay section works but has no visual effect until the pass is
  reimplemented in the SDK's Vulkan present path (shader already in-repo:
  `renderer/shaders/nhl_grade.comp`).
- `docs/rexglue-vulkan-nhl-legacy.patch` is the original (historical) patch; the
  canonical one that applies to public `bd9b5191` is under `sdk/`.
- The optional `tdb-rx2-ffi` static lib (runtime `.dds` → `.rx2` texture
  override) comes from the private `nhl-database-studio` sibling repo. Without
  it the build prints a warning and disables loose-`.dds` overrides — everything
  else works. Set `-DRX2FFI_LIB=<path>` if you have it.
- PGO profiles are machine-generated; the first fork builds are non-PGO
  (`_build_vk_opt.bat`), which is ~the v0.2.0 shipping config minus PGO.
