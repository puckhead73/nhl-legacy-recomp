# Pivot plan — divorce from EDRAM/FSI: exclusive plume renderer (D3D9-hook frame graph + PM4 geometry tap)

> **Status:** PLAN (decided to pursue 2026-06-22). Reverses the 2026-06-16 "SDK Vulkan FSI is the
> baseline" decision ([current-status.md](current-status.md), [vulkan-migration-plan.md](vulkan-migration-plan.md))
> in favor of a renderer we fully own, with **no EDRAM, no fold, no fragment-shader-interlock**.
>
> **North star:** plume is the *sole* renderer and the *sole* presenter. rexglue does CPU recomp +
> headless PM4 geometry decode only — it never produces or shows a pixel.

---

## 0. The non-negotiable target: *exclusively* plume

A prior misunderstanding had plume running **alongside** rexglue's EDRAM render (both active,
plume in its own window). That coexistence was **bring-up scaffolding only** — every Path C
milestone was built additive + env-gated so it could be verified without disturbing the working
game. **It is not the goal.**

The goal is **exclusive plume**:

| Component | Endstate |
|---|---|
| rexglue CPU recomp (PPC→native game logic) | **KEPT** — this is the game; nothing replaces it |
| rexglue PM4 **decode front-end** (parse, primitive processor, register decode, Xenos→SPIR-V) | **KEPT, but headless** — used only to extract geometry/state from inlined draws; renders nothing |
| rexglue GPU **render backend** (EDRAM emulation, FSI/host-RT, render-target cache, resolve) | **OFF** — deleted from the live path |
| rexglue **present** (SDK presenter / VdSwap path) | **OFF** — plume presents |
| **plume** (our Vulkan renderer) | **THE renderer + THE present** — only source of pixels |

If at the end of this pivot rexglue's GPU backend is still drawing anything, the pivot is not done.

---

## 1. The governing constraint (verified, immovable)

NHL Legacy's **per-draw `DrawIndexedPrimitive` packet emission is INLINED** into the recompiled
code — confirmed by the live M1 runtime tap, not a static guess
([phase0-d3d9-hookability.md](phase0-d3d9-hookability.md) §"M1 UPDATE",
[highcut-h1-resource-graph.md](highcut-h1-resource-graph.md)). EA's compiler inlined the draw path
into raw command-buffer (PM4) stores ~15 years ago; that function boundary does not exist in the
binary.

**Consequences:**

- **PM4 cannot be eliminated for this title.** There is no higher-level draw call to intercept.
  For NHL Legacy, *the draws are the PM4 stream*. No recompiler change recovers what the original
  compiler inlined away.
- **An Unleashed/XenonRecomp-style toolchain swap is a trap here.** That approach works for titles
  whose draws are out-of-line D3D9 calls (Sonic Unleashed); it carries **no EDRAM/PM4 model**
  ([[unleashed-recompiled-no-edram]]), so for this title's inlined draws it would have *nothing to
  hook for geometry*. rexglue's PM4 decode is what makes this title tractable. **Keep rexglue's
  CPU recomp + PM4 decode.**

**But PM4 is not what causes the pain.** EDRAM, the fold, and FSI come from *modeling the
render-target/resolve memory as Xenos EDRAM* — not from the draws. So the pivot does not delete
PM4; it **demotes** PM4 from "the EDRAM substrate" to "a thin geometry channel."

---

## 2. Why the divorce is reachable: the D3D9 frame graph is fold-free

H-1 proved that **every render-target/resolve/present signal is available out-of-line at the D3D9
hook level, at TRUE logical size — never the 640-pitch fold**
([highcut-h1-resource-graph.md](highcut-h1-resource-graph.md)). The fold is purely an artifact of
the PM4/EDRAM *emulation* one layer below; at the D3D9 interception level it does not exist.

| signal | D3D9 hook | logical value |
|---|---|---|
| present surface | `sub_827F1C88` | 1280×720 |
| viewport extent | `sub_827E6480` | 1280×720 |
| resolve dest | `sub_827EF8E0` (count-exact = Resolve) | 1280×720 |
| sampled textures | `sub_827E5938` | true sizes |

So the architecture is a **hybrid high cut**:

```
                D3D9 HOOKS (out-of-line, logical, fold-free)
  resource-create ─┐
  SetViewport ─────┤→  flat plume RTs @ logical size   ┐
  Resolve ─────────┤→  host-copy RT→texture            ├─→  PLUME (sole renderer + present)
  Present ─────────┘→  drive plume swapchain           │
                                                        │
                PM4 GEOMETRY TAP (inlined draws only)   │
  decode verts/indices/shaders/constants ──────────────┘  (no EDRAM semantics; just triangles)
```

EDRAM never exists. The fold never happens. FSI is never needed. PM4 is reduced to a payload of
"here are some triangles, with this shader and these constants, into the currently-bound logical
RT."

---

## 3. What is already built (do not rebuild)

| Piece | Where | State |
|---|---|---|
| plume in-process device + swapchain, driven by guest Present | `gpu/hooks/plume_present.cpp` (H-2) | built |
| Xenos ucode → SPIR-V translator (ported, spirv-val-clean) | `gpu/spirv/*` (Path C P-1…P-3) | built |
| Flat-RT render of real decoded draws: geometry, textures, depth/stencil, blend, multi-draw frame | Path C C-3…C-5c, `plume_present.cpp` | built (menu ~90% faithful; 3D depth build-clean, runtime-verify pending) |
| **D3D9 logical resource graph** (resource/RT/resolve/present at logical size) | `gpu/hooks/d3d9_resources.cpp` (H-1, `NHL_HIGHCUT`) | built, runs live |
| PM4 draw decode (verts/indices/constants/viewport/topology) | `renderer/core/nhl_command_processor.cpp::RenderBetaOwnedDraw` | built |

The two halves — **the flat plume renderer (Path C)** and **the fold-free D3D9 frame graph
(H-1)** — exist but were **never fused**. Path C sizes its RTs from PM4-inferred EDRAM state; H-1
observes the true logical graph but drives nothing. The pivot is the fusion + the finish + the
takeover.

> Caveat: `gpu/hooks/d3d9_resources.cpp` and `gpu/hooks/d3d9_tap.cpp` override the same guest
> symbols, so only one is in `NHLLEGACY_SOURCES` at a time. The fused path needs the resource hooks
> live concurrently with the CP decode — reconcile the source-set/weak-alias arrangement in F-1.

---

## 4. Milestones

Each milestone has a binary exit criterion. F-1…F-3 build the fused renderer; F-4 makes it
exclusive; F-5 reaches parity; F-6 is the gating performance proof.

### F-1 — Flat RTs sized from the D3D9 graph (kills the fold structurally)
Replace Path C's PM4-inferred surface sizing with H-1's D3D9-hooked logical sizes. Each guest
color/depth surface (observed via `CreateRenderTarget`/`CreateTexture` + the bound viewport) → one
flat plume RT at logical size. Reconcile the `d3d9_resources.cpp` ↔ CP-decode source arrangement so
both run together.
**Exit:** a live frame's RTs are all created at logical size from D3D9 hooks (no 640-pitch anywhere
in the plume path); the existing menu render still composites correctly into them.

### F-2 — Resolve = D3D9-hooked host-copy (render-to-texture)
Use the count-exact `sub_827EF8E0` Resolve hook to drive host-copy of a flat plume RT → a plume
texture, keyed by the D3D9 resolve dest address. This is what makes shadow maps, reflections, and
post-process composite correctly without EDRAM resolve.
**Exit:** a 3D scene's shadow-map / reflection / post chain composites correctly from
D3D9-hooked resolves; no SDK EDRAM resolve involved.

### F-3 — Draw-tap → flat render
Wire the inlined-draw PM4 decode (`RenderBetaOwnedDraw`) to emit into the F-1 flat RT bound at draw
time (attribute each inlined draw to the current logical viewport/RT from the D3D9 graph). Live, not
disk-replay.
**Exit:** a full live frame (menu, then a 3D scene) renders end-to-end into the D3D9-sized flat RTs
from the live draw tap — still alongside rexglue, but plume's output is a faithful full frame.

### F-4 — TAKEOVER: make plume exclusive  ← *the goal*
Suppress rexglue's GPU render backend and present so plume is the only output:
- rexglue GPU backend produces nothing (no EDRAM, no FSI, no SDK render-target cache, no SDK
  present). Investigate running rexglue's CP decode **headless** (decode without a render-target
  backend attached) — or, if that proves entangled, decode the tapped PM4 ourselves.
- plume presents to the real game window (not a secondary window).
**Exit:** with rexglue's render backend disabled, the game is fully playable and visually faithful
with **plume as the sole renderer and presenter**. No coexistence.

### F-5 — Parity pass
Sweep correctness across all scenes (menu, intro 3D, create-player, gameplay, replay). The FSI
baseline reached parity with 4 small fixes; the owned path needs its own sweep (lighting/gamma,
jersey numbers/names, equipment normals + cube reflections, alpha-to-coverage net, MSAA).
**Exit:** user-verified parity with the retired FSI baseline across all scenes.

### F-6 — Performance  ← *the gating risk*
Path C was ~3 fps, but that was **bring-up artifacts** (CPU untile, disk-replay, dual-GPU
coexistence) — F-3/F-4 remove all three. Real-time *owned* plume is nonetheless **unproven**. Levers:
GPU-compute untile, descriptor/buffer pooling, dynamic-texture-by-address reuse, removing the
coexistence overhead, MT command recording.
**Exit:** real-time framerate (target: within range of the retired FSI baseline's 66–84 fps) in
dense gameplay. **This is the make-or-break; if it cannot be met, the pivot fails and FSI stands.**

---

## 5. Risks / open decisions

- **F-6 perf is a genuine unknown.** Recommend an early de-risking spike (a representative dense
  frame through the fused live path, measured) before committing the full F-2…F-5 build-out. If
  plume can't approach real-time, the whole pivot is moot and the FSI baseline should be kept.
- **Headless rexglue CP decode (F-4).** Whether the SDK CP front-end can run with no render-target
  backend attached is unverified. Fallback: parse the tapped PM4 ourselves (more work, fully
  decouples from the SDK GPU).
- **Loss of the FSI "free" wins.** This re-opens everything [vulkan-migration-plan.md](vulkan-migration-plan.md)
  marked SUBSUMED. The owned path must re-earn each via the F-5 parity sweep.
- **Hook source-set conflict (F-1).** `d3d9_resources.cpp` vs `d3d9_tap.cpp` override the same
  symbols; the fused build needs them coexisting — resolve the weak-alias/source arrangement.

## 6. What rexglue is reduced to (the endstate, restated)

1. CPU recomp — the game's logic (PPC→native). Untouched.
2. A **headless PM4 geometry decoder** for the inlined draws — produces no pixels.

Everything that sizes a render target, resolves, draws a triangle, or presents a frame is **plume,
exclusively**.
