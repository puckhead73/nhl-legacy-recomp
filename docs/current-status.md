# Current status — the primary baseline

**Decided 2026-06-16.** The **SDK-native Vulkan backend on the `fsi` render-target path** is the
project's primary renderer and build target. **All future development builds off this.** It renders
NHL Legacy at high-cut correctness with real-time framerates (~30–84 fps gameplay vs the high-cut
path's ~3 fps).

This document is the authoritative pointer to "what is current." Older design docs in `docs/` are
historical context for how we got here.

## The renderer

- **Backend:** SDK native `vulkan::VulkanGraphicsSystem`, subclassed as `NhlVkCommandProcessor`
  ([renderer/core/nhl_vk_backend.cpp](../renderer/core/nhl_vk_backend.cpp) / `.h`).
- **Render-target path:** `fsi` (`Path::kPixelShaderInterlock` — the per-tile EDRAM/ROV-equivalent).
  Selected by `NHL_VK_BACKEND=1`; `NHL_VK_RT_PATH` defaults to `fsi`. **Do not use `rov`/`rtv`** —
  they fall through to `kHostRenderTargets` and render the in-game 3D scene black.
- **rexglue SDK:** built from source with `REXGLUE_USE_VULKAN=ON` at `E:\Tools\rexglue-sdk\src`
  (the published win-amd64 zips ship Vulkan OFF). Title-specific correctness fixes live as patches
  in that source tree — see [rexglue-vulkan-nhl-legacy.patch](rexglue-vulkan-nhl-legacy.patch).

## What's in the baseline

- **Net transparency fix** — the FSI path doesn't cull on alpha-to-coverage, so the rink net
  rendered opaque; emulated via alpha-test in `nhl_vk_backend.cpp` (commit `f667ccd`).
- **SDK source patches** — exp_adjust word-3 (jersey font), `readback_resolve=full` (equipment),
  signed BC5/DXN normal maps.
- **In-game enhancements overlay** — ImGui settings/perf HUD/supersampling on the Vulkan path.
- **Color-grade post-process** — live "Lighting / Color Grade" overlay section (exposure/contrast/
  saturation/brightness/white-balance/filmic tone-map) as a compute pass after the swap gamma-apply;
  `present_grade_*` cvars, identity-default no-op (commit `768c484`).
- **Perf toolchain** — Release+LTO+PGO+native build is the #1 lever; supersampling ~free.

## Superseded / reference-only

The **high-cut plume path** (`gpu/hooks/`, `NHL_HIGHCUT*`) and the **beta owned-D3D12 takeover**
(`NHL_BACKEND=beta`) are **no longer the development surface**. They proved the rendering model and
remain useful as a *correctness* ground truth for A/B comparison, but new work targets the Vulkan
backend, not these.

## Build & run

**THE canonical build is `-vk-ffx` (FidelityFX on).** Decided 2026-06-17. There is ONE game
source tree (`E:\Repositories\nhl-legacy-recomp`, `master`) and ONE SDK source tree
(`E:\Tools\rexglue-sdk\src`); the `-vk` vs `-vk-ffx` split is only two CMake output dirs + two SDK
install prefixes, NOT divergent source. FFX is opt-in at runtime (default `present_effect=bilinear`
≡ non-FFX), so `-vk-ffx` is a strict superset of the old plain `-vk` build, which is **retired**.
Every dev instance must build `-vk-ffx` so no compiled binary goes stale.

```
_ffx_sdk_build_install.bat   # SDK -> out/install/win-amd64-ffx  (only when patching rexglue source)
_game_ffx_build.bat          # game -> out/build/win-amd64-vk-ffx
```
Canonical exe: `out\build\win-amd64-vk-ffx\nhllegacy.exe`. Run on the Vulkan path with
`NHL_VK_BACKEND=1` (defaults to `fsi`). Diagnostic driver: `_vknet.ps1`.

- `_build_vk.bat` is now a **deprecation shim** that redirects to `_game_ffx_build.bat`.
- Optimized/PGO release builds (`_build_vk_opt.bat`, `_build_vk_pgo.bat`, `_build_vk_pgogen.bat`)
  link the **FFX** SDK install (`E:/Tools/rexglue-sdk/src/out/install/win-amd64-ffx`).
- **Consolidation complete (2026-06-17):** the stale non-FFX build dirs
  (`out/build/win-amd64-vk{,-opt,-pgo,-pgogen}`) and the non-FFX SDK install
  (`.../src/out/install/win-amd64`) have been deleted. The only Vulkan build dir is
  `win-amd64-vk-ffx` and the only SDK install is `win-amd64-ffx`; everything links FFX.

## Release

Packaged by [release/package.ps1](../release/package.ps1) (`-Version X.Y.Z`): defaults to the
prebuilt `win-amd64-vk-ffx` dir (auto `-SkipBuild`, since vk has no cmake preset). **v0.1.0** is
tagged and pushed to `origin`. See the [release-pipeline memory] / `out/release/RELEASE-NOTES-*.md`.

## Active / open

Tracked in [vulkan-migration-plan.md](vulkan-migration-plan.md): jersey numbers (D3), lighting/gamma
(D4), Phase-B enhancement hooks, and fps verification under dense gameplay.
