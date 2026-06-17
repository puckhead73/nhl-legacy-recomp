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
- **Perf toolchain** — Release+LTO+PGO+native build is the #1 lever; supersampling ~free.

## Superseded / reference-only

The **high-cut plume path** (`gpu/hooks/`, `NHL_HIGHCUT*`) and the **beta owned-D3D12 takeover**
(`NHL_BACKEND=beta`) are **no longer the development surface**. They proved the rendering model and
remain useful as a *correctness* ground truth for A/B comparison, but new work targets the Vulkan
backend, not these.

## Build & run

```
_build_vk.bat build          # rebuild the nhllegacy Vulkan target (recomp only)
_build_vk.bat configure      # one-time / after CMake changes
```
Run on the Vulkan path: `NHL_VK_BACKEND=1` (defaults to `fsi`). Diagnostic driver: `_vknet.ps1`.
SDK rebuild (only when patching rexglue source): `E:\Tools\rexglue-sdk\src\_build_vk.bat`.

## Active / open

Tracked in [vulkan-migration-plan.md](vulkan-migration-plan.md): jersey numbers (D3), lighting/gamma
(D4), Phase-B enhancement hooks, and fps verification under dense gameplay.
