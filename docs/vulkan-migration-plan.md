# Migration plan — carrying the high-cut/beta work onto the SDK Vulkan backend

Date: 2026-06-16. Follows [vulkan-rov-backend-spike-findings.md](vulkan-rov-backend-spike-findings.md)
(spike = GO; gameplay renders on the Vulkan **fsi** path). Answers: *what of the completed
beta/high-cut work carries over, and how?*

## The one-paragraph answer

The SDK Vulkan backend runs **none** of our rendering code — it has its own shader translator,
texture cache, EDRAM, resolve and compositing. So the completed work splits three ways: most of it
(~37 pieces) is **SUBSUMED** — the SDK does it natively, which is exactly *why* gameplay renders, and
we delete that code; the **asset/file layer ports for free** (it changes the bytes the game loads, not
the draws); and a short list of **title-specific correctness fixes** doesn't port as code but maps to
small **patches in the rexglue source we now build** — guided by our prior diagnoses and the NHL12
team's notes. Net: we trade thousands of lines of fragile bridge code for a handful of targeted SDK
patches.

---

## Bucket 1 — SUBSUMED (delete; the SDK does it natively)

The bulk of high-cut (25 pieces) and beta (12 of 23) were re-implementing faithful rendering that the
SDK provides: the Xenos→SPIR-V translator, plume device/window/swapchain, depth/stencil, indexed draw,
all texture-format untiling (incl. BC5/cube/k_8), float/bool/system/blend/scissor/color-mask/y-flip
state, EDRAM resolve, per-surface RTs, the live-feed bridge, the by-ID resource cache, the offline
replay + capture tooling. **None of this is "lost"** — it proved the rendering model was achievable and
is the reason the SDK path works. It stops being our maintenance burden.

## Bucket 2 — PORTS FREE (already live in the VK build; VFS/file layer)

| Piece | Hook layer | Status |
|---|---|---|
| `UnionDevice` (loose `_compiled` over `cache:`) | VFS | ports free — already compiled into the VK build |
| `LooseTreeDevice` + runtime `.dds`→`.rx2` synth | VFS (file `Open`) | ports free |
| `injection_registry` **registration + sidecar** (`RegisterRx2`, `ScanDirectory`, `Write/ParseSidecar`) | VFS / file | ports free |
| DB/roster work (nhl-database-studio) | data | renderer-independent |

**Caveat:** the injection **live-correlation** half (`CorrelateTexturesForInjection`, called from the
D3D12 `IssueDraw`) is GPU-layer and does *not* port — but it's a *capture/authoring* tool, not needed
for normal rendering. If we want auto address→asset mapping on Vulkan later, re-add a one-call hook in
`NhlVkCommandProcessor::IssueDraw` (we already have that override). Texture *mods themselves* work now.

## Bucket 3 — TITLE FIXES → patch the rexglue source (the real "bring it over")

We build rexglue from source (`E:\Tools\rexglue-sdk\src`), so title-specific defects get fixed in the
SDK's own renderer — the NHL12-team approach. Several of our hard-won fixes turn out to be **already
correct in the SDK** (verify-only): Xenos component swizzle, `color_exp_bias` tone-mapping, per-sampler
POINT/CLAMP filtering (those were *our-backend* bugs, not SDK bugs). The visible gameplay defects map to
a short patch list:

| # | Defect | Root cause (confirmed by reading SDK source) | Patch site | NHL12 ref | Risk |
|---|---|---|---|---|---|
| **D1 ✅ PORTED** | Equipment shading green/speckle | `k_DXN` (BC5 normal) had **no signed host pair** → guest-signed normals sampled the UNORM view, never reconstructed to [-1,1] | DONE in `E:\Tools\rexglue-sdk\src\src\graphics\vulkan\texture_cache.cpp`: gave `k_DXN` a `VK_FORMAT_BC5_SNORM_BLOCK` signed pair + `unsigned_signed_compatible=true`, removed `k_DXN` from `kD3D12SignedUnsupportedFormats` (19→18), added a runtime BC5_SNORM-support guard | `re/equipment-recolor/README.md` §4 "B1 DXN normal" (#1 cause); `nhl12_renderer_system_notes.md` L405-417 | **low** |
| **D2 ✅ PORTED** | Cubemap reflections wrong/missing (electric blue / multicolor on metallic equipment) | Stock Xenia had no "cube-reflection sanitizer": cube RGB unclamped + cube **alpha not forced to 1.0** | DONE in `pipeline/shader/spirv_translator_fetch.cpp` (tfetch `result[]` before `StoreResult`, ~L2050): for `kCube` — clamp RGB [0,1] → collapse to min channel → cap 0.25, force alpha=1.0. Bumped `spirv_translator.h` `kVersion` 12→13. Hardcoded ON (no cvar yet; would need folding into the shader cache key) | `nhl12_renderer_system_notes.md` §"Metallic Reflection / Cube Fetch Patch" L509-553; our [[highcut-c5f-jersey-number-sampler-fix]] | med-high |
| **D3** | Jersey numbers/names missing | SDK sampler/filter is **already correct**; likely gated by the same cube-alpha/decal composition as D2. Re-diagnose after D1/D2 (no takeover/stale-RAM confound on native path) | re-investigate; `spirv_translator_fetch.cpp` `QueryTextureLod` (~L2221) explicit-LOD | system-notes L289-303, L442; [[highcut-c5g-jersey-number-residency]] | high |
| **D4** | Weird lighting/shading | Most likely **downstream of D1+D2** (bad normals + garbage cube/specular feed lighting). Secondary: shader **modification key** not encoding every mode → stale cached SPIR-V/PSO | revisit after D1/D2; `vulkan/pipeline_cache.cpp` modification version | `graphics/shaders-materials.md` §1, §5 | med |

### Recommended first patch: **D1 (signed BC5/DXN normal)**
Lowest-risk, highest-value, self-contained: add the `VK_FORMAT_BC5_SNORM_BLOCK` host pair for `k_DXN` in
`vulkan/texture_cache.cpp` (mirrors the NHL12 working renderer's "BC5_UNORM + BC5_SNORM SRVs"). The
translator's per-component sign-switch is already correct, so no shader-math change. The NHL12 RE ranks
DXN sign as the **#1** equipment cause (explains *equipment-only*, *green = BC5 R,G*, *worst on dark*),
and correct normals also de-garbage the inputs to D4's lighting. Bump the texture-cache/shader
**modification version** so stale cached SPIR-V/PSOs don't mask the fix. Then D2 (cube sanitizer, behind
a flag), then re-diagnose D3/D4.

> Note: `[[nhl12-decomp-bc5-normal-leads]]` records BC5-normal as "ruled out" — but that was for the
> *high-cut* path (whose equipment issue was capture-residency). On the *SDK Vulkan* path it IS the
> cause: the SDK source genuinely lacks the signed DXN pair. Same lead, different path, now relevant.

---

## Sequencing

1. **Consolidate** the spike (fsi default — done; commit the Vulkan scaffolding + this plan).
2. **D1** signed-BC5 SDK patch → rebuild SDK → verify equipment in live gameplay.
3. **D2** cube sanitizer (flagged) → verify reflections + recheck jersey numbers.
4. **D3/D4** re-diagnose on the native path with D1/D2 in place.
5. Port the injection **live-correlation** hook into `NhlVkCommandProcessor::IssueDraw` only if/when
   auto asset-mapping is wanted (mods already work via the VFS layer).
