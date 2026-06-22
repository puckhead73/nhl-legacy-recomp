# Kickoff — BUILD: graphics enhancements on the Vulkan-fsi backend

**Use this as the opening prompt for a fresh session. The renderer foundation is DONE and validated;
this session builds enhancements (internal-res, upscaling, HD textures, framerate, custom draws) on
top of it. Read this whole doc first, then `docs/vulkan-migration-plan.md` and the auto-memory
`sdk-vulkan-rov-backend-option`.**

---

## 0. TL;DR

NHL Legacy now renders on the rexglue SDK's **native Vulkan ROV/EDRAM backend** (the `fsi` /
fragment-shader-interlock path) at **parity with the old high-cut renderer's correctness** — lighting,
jersey numbers/names, equipment all verified in live gameplay — at **66–84 fps dense gameplay** vs the
high-cut path's ~3 fps. This replaces the entire high-cut plume perf treadmill with a faithful + fast +
portable foundation, with far less code. **Committed on branch `vulkan-fsi-backend` (`1184bcf`).**

This foundation makes enhancements *cheap* — most are built-in SDK features (internal-res supersampling
and FidelityFX are cvars/build-flags), and the file-layer texture-mod pipeline ports for free. This
session: turn those on, measure, and make them look good.

---

## 1. What we're building on (the foundation)

**Opt-in, non-destructive.** Everything is gated on env `NHL_VK_BACKEND`; with it unset the default
D3D12 build is byte-for-byte unchanged. Two moving parts:

1. **A Vulkan-enabled rexglue SDK** built from source (the public stock zips ship
   `REXGLUE_USE_VULKAN OFF`). Lives in a **separate clone**: `E:\Tools\rexglue-sdk\src` (pinned to
   commit `bd9b519` = our linked `0.8.1.31-dev.gbd9b519`). Our source patches to it are captured in
   [rexglue-vulkan-nhl-legacy.patch](rexglue-vulkan-nhl-legacy.patch).
2. **A thin subclass in our repo** ([renderer/core/nhl_vk_backend.cpp](../renderer/core/nhl_vk_backend.cpp)):
   `NhlVkGraphicsSystem` / `NhlVkCommandProcessor` forward to the SDK's native backend and tap
   `IssueSwap`/`IssueDraw` for a `[nhl-vk-fps]` report (fps + draws/frame). Selected in
   [src/nhllegacy_app.h](../src/nhllegacy_app.h) `OnPreSetup` under the `NHL_VK_BACKEND` gate (which
   sets `render_target_path_vulkan=fsi`, `readback_resolve=full`, etc.).

**The four fixes that got us to parity** (diffed against our own high-cut code — the proven-correct
reference — NOT the NHL12 docs, which are a different game and did not map):
- **numbers/names**: SDK SPIR-V translator read texture `exp_adjust` from fetch-constant **word 4**
  (= `lod_bias`) instead of **word 3**; the jersey font atlas (`lod_bias -0.75`) was scaled by ~2⁻¹² →
  black. SDK patch in `spirv_translator_fetch.cpp`.
- **goalie equipment**: `readback_resolve=full` (cvar) so EDRAM resolve results reach guest RAM.
- **equipment normals**: signed `BC5_SNORM` host view for `k_DXN`. SDK patch in
  `vulkan/texture_cache.cpp`.
- **cartoonish lighting** cleared once `exp_adjust` was fixed.

`fsi` is mandatory: the Vulkan backend only honors `render_target_path_vulkan=="fsi"` (the real per-tile
EDRAM path); any other value (incl. `"rov"`) silently falls back to host-RTs and renders the in-game 3D
scene black.

---

## 2. Build / run

**SDK (only when changing the SDK patch):** in `E:\Tools\rexglue-sdk\src`, run `_configure_vk.bat`
once, then `_build_vk.bat` to build+install (RelWithDebInfo) to `out/install/win-amd64`. Then copy
`out/install/win-amd64/bin/rexruntimerd.dll` into our VK build dir.

**Our app:** `_build_vk.bat configure` once, then `_build_vk.bat build` (builds the `win-amd64-vk`
build dir against the Vulkan SDK with `-DNHLLEGACY_VULKAN_BACKEND=ON`). The default
`_build_beta.bat` / `win-amd64-relwithdebinfo` D3D12 build is untouched.

**Run:** `NHL_VK_BACKEND=1` (fsi is the default; no `NHL_BACKEND`/`NHL_HIGHCUT_*`). Drivers:
`_vktest.ps1` (smoke), `_vkfps.ps1` (fps log; pass `-NoVsync` for uncapped), `_vkwindowshot.ps1`
(window screenshot), `_vklongshot.ps1` (multi-capture through the attract-demo into gameplay). The
attract demo reaches on-ice gameplay ~70–90 s after launch. Logs:
`out/build/win-amd64-vk/logs/nhllegacy_*.log`; grep `[nhl-vk-fps]`.

**Reproduce the SDK from scratch:** clone `github.com/rexglue/rexglue-sdk`, checkout `bd9b519`,
`git submodule update --init --recursive`, materialize the 15 `libmspack` symlink stubs (Windows
`core.symlinks=false` writes them as text — copy each symlink target over the stub), apply
`docs/rexglue-vulkan-nhl-legacy.patch`, then configure with `-DREXGLUE_USE_VULKAN=ON`. (A scripted
bootstrap for this is a nice-to-have — see §5.)

---

## 3. Enhancement roadmap (ranked; pick and build)

All of these are now cheap because the SDK provides the machinery. Current relevant settings in
`OnPreSetup`: `draw_resolution_scale_x/y = 1` (native 720p internal), `window 1920x1080`, `vsync = true`
(caps 60; `NHL_VK_NO_VSYNC=1` frees it), `readback_resolve=full`.

**A. Internal-resolution supersampling (RECOMMENDED FIRST — cheapest, biggest visual win).**
The SDK renders the guest 1280×720 internally; `draw_resolution_scale_x/y` is an integer multiplier
(2 → 1440p, 3 → 2160p internal, downsampled to the window). It's a cvar — flip it (gate a higher value
under `NHL_VK_BACKEND`, or env-drive it) and measure fps on the 4080. This is the headline "remaster"
look (crisp edges, no shimmer) for near-zero code. Watch VRAM + the `texCache`/EDRAM scaling cost;
confirm resolves/readback still behave at scale.

**B. FidelityFX (CAS sharpening / FSR upscaling).** `REXGLUE_ENABLE_FIDELITYFX=ON` is a *build* option
on the SDK (default OFF; needs a Vulkan/D3D12 backend). Rebuild the SDK with it on, then drive its
cvars. Pairs well with A (render high, FSR/CAS to present) or as a cheaper alternative to brute-force
supersampling.

**C. HD texture replacement (the mod pipeline — already ports free).** The loose-file `.dds`→`.rx2`
override (`UnionDevice`, `LooseTreeDevice`, `injection_registry` registration/sidecar) hooks at the VFS
layer, so edited textures already feed the Vulkan backend with no GPU-side work — it's compiled into
the VK build today. Build an authoring loop: dump → upscale/replace → drop into the loose tree → see it
in-game. (The *live address→asset correlation* half hooked `IssueDraw` on D3D12 and would need a small
re-add in `NhlVkCommandProcessor::IssueDraw` — but plain mods don't need it.)

**D. Framerate unlock / pacing.** `vsync` is forced on (console pacing). Expose an uncapped/high-refresh
mode cleanly (we have `NHL_VK_NO_VSYNC`); reconcile with the FE/Flash timeline that historically ran
fast when unthrottled (see the `OnPreSetup` vsync comment). Gameplay already has fps headroom.

**E. MSAA / anti-aliasing.** `native_2x_msaa` and the SDK's MSAA path are available; relevant once
internal-res is chosen (supersampling may make MSAA redundant — measure).

**F. Custom draws / overlays.** `NhlVkCommandProcessor::IssueDraw`/`IssueSwap` are already overridden
(currently just the fps tap) — the seam for injected geometry, debug HUDs, or post effects.

---

## 4. Constraints / gotchas

- **`fsi` only** (§1). Don't change `render_target_path_vulkan`.
- **SDK lives outside git.** Any SDK change must be re-captured into
  `docs/rexglue-vulkan-nhl-legacy.patch` (`git diff` in the clone) or it's lost. Bump
  `spirv_translator.h` `kVersion` whenever you change shader translation (invalidates cached SPIR-V).
- **DLL copy step.** After rebuilding the SDK, copy `rexruntimerd.dll` into `out/build/win-amd64-vk`
  (the post-build copy only fires on a relink).
- **Verify visually in gameplay**, not menus/title — the attract demo reaches the ice ~70–90 s in, and
  headless screenshots often miss it. The user watching the live window is the fastest oracle.
- **NHL12 reference (`docs/nhl12-decomp-reference/`) is a DIFFERENT game** — useful for intuition, not
  literal answers. Port from our own high-cut code when matching correctness.
- **Known separate, unfixed:** the VP6 intro video shows green/red corruption. It's a video-decode
  issue (`docs/nhl12-decomp-reference/nhl12_vp6_green_video_fix_plan.md`), NOT a 3D-render issue, and
  out of scope for enhancements.

---

## 5. Suggested first moves

1. **Scripted SDK bootstrap** (optional but valuable): a script that clones `bd9b519`, materializes the
   libmspack symlinks, applies the patch, configures `-DREXGLUE_USE_VULKAN=ON`, builds+installs, and
   copies the DLL — so the Vulkan runtime regenerates from scratch and the patch is provably complete.
2. **Internal-res A/B**: set `draw_resolution_scale_x/y = 2` (and try 3) under the VK gate, capture the
   same gameplay scene at 1×/2×/3×, log `[nhl-vk-fps]` for each, and eyeball sharpness. Decide the
   default. This is the fastest, most visible win and proves the enhancement thesis.
3. From there, FidelityFX (B) and the HD-texture authoring loop (C) are the natural follow-ups.

## 6. References

- [vulkan-rov-backend-spike-findings.md](vulkan-rov-backend-spike-findings.md) — the GO/parity writeup +
  reproduce steps.
- [vulkan-migration-plan.md](vulkan-migration-plan.md) — what of high-cut ports / is subsumed / is an
  SDK patch (the inventory).
- [rexglue-vulkan-nhl-legacy.patch](rexglue-vulkan-nhl-legacy.patch) — the SDK source fixes (base
  `bd9b519`).
- auto-memory `sdk-vulkan-rov-backend-option` — full chronological log of the spike → parity.
- Branch `vulkan-fsi-backend` @ `1184bcf` — the committed foundation.
