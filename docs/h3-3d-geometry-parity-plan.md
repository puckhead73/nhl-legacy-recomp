# H-3 — beta 3D-geometry parity: make the create-player model appear (campaign plan)

> **Deliverable of this session: a strategy, not a fix.** No renderer code changed.
> **Target:** the beta owned-D3D12 backend (`NHL_BACKEND=beta NHL_BETA_TAKEOVER=1`,
> route-(a) flat path `NHL_BETA_FLAT=1`) must render the scene_02 create-player **3D player
> model** so it matches the oracle `replay_scene02_full.png`.

## The wall (don't re-derive — see [[highcut-h3-flat-rt]])

Route (a) is built and the **fold is gone**: on each guest resolve we copy the flat
offscreen RT into a host texture keyed by `GetResolveInfo().copy_dest_base` and substitute
it when a draw samples that address. On scene_02 f150 the mispositioned/wrapped player is
gone and the 2D UI is pixel-perfect — but the player is **absent**. Proven:

- `NHL_BETA_FLAT_FAKE` (solid red for the resolved texture) → whole background red ⇒
  substitution + composite quad (`#430`, full-screen, UV[0,1], CLAMP) are **correct**; the
  captured texture (`0x1AF09000`) is simply **empty**.
- `NHL_BETA_FLAT_KEEPFIRST` → still empty ⇒ not a double-resolve overwrite; **both** resolves
  of `0x1AF09000` are empty.
- ⇒ The player's **3D geometry rasterizes nothing into any captured scratch**, even though
  the player exists in guest RAM (the old untile path showed it, mispositioned).

So this is **Tier-1 binding/constant parity isolated to the 3D-draw subset** (vte=0x43F,
viewport transform on). 2D draws (vte=0x300) are at parity; 3D draws rasterize nothing.

## Architectural facts that constrain the plan (read before designing tests)

1. **One shared offscreen RT for all passes.** `beta_offscreen_rt_` is the only color
   target; `BetaFlatResolve()` ([nhl_command_processor.cpp:1004](../renderer/core/nhl_command_processor.cpp#L1004))
   copies the *whole* RT into a per-dest host texture at each guest resolve. Color is **never
   cleared by us** between passes (only the guest's own background/clear draws clear it);
   **depth is cleared per-pass** at each resolve. ⇒ A raw-geometry scratch dump is only
   readable in a window *after* the player's geometry pass and *before* the next guest clear.
2. **`pack_floats` ignores dynamic float addressing.**
   [nhl_command_processor.cpp:1849](../renderer/core/nhl_command_processor.cpp#L1849) packs
   only the constants named in `float_bitmap`, in ascending storage-index order — the
   **static-addressing** layout. It **never branches on `float_dynamic_addressing`**, yet the
   diag at line 2354 already prints `dyn=`. The DXBC translator picks the CBV layout *from
   that flag at translation time*: a dynamic-addressing shader expects the **full contiguous
   `[0, float_count)` array at natural indices**, not the sparse pack. **This is the prime
   suspect** (see Hypothesis 1).
3. **Flat 3D viewport branch.** For vport_xform draws the flat path
   ([nhl_command_processor.cpp:1507](../renderer/core/nhl_command_processor.cpp#L1507)) uses
   `vpi.xy_extent/xy_offset` from an un-clamped `x_max=8192`, scissor-clamped to
   `beta_rt_width_/height_` (1280×720). The half-res player passes (640×360 surface, vte on)
   compute their own viewport here.
4. **Single capture frame.** `NHL_BETA_CAPTURE_FRAME=150`; all owned rendering is deferred to
   that frame, IssueSwap dumps the offscreen RT → `beta_owned_draw.png` (composite over BLACK,
   [[beta-png-alpha-artifact]]).

## Hypothesis ranking

| # | Hypothesis | Why this rank | Decisive cheap test |
|---|---|---|---|
| **1** | **Float-constant upload wrong for 3D shaders** — `pack_floats` feeds the sparse static layout to a VS the translator built for **dynamic addressing** (skinned player = bone-matrix-indexed WVP). Garbage matrices → degenerate clip positions → zero raster. | Exactly fits "2D parity, 3D nothing": menu VS = static addressing (sparse pack correct); skinned player VS = dynamic (sparse pack wrong). Code path **provably unhandled** (fact 2). | `NHL_BETA_BIND_DIAG` already prints `dyn=` for the player VS — one look. |
| 2 | **Vertex fetch/format for 3D** — wrong fetch-constant base/stride/endian/format for the player's vertex layout → fetched positions garbage/zero. | The fetch path is shared with 2D, but the player's vertex layout (skinned: blend weights/indices, packed normals) differs from 2D quads; a wrong stride collapses verts. | `NHL_BETA_BIND_DIAG` vertex dump (line 2316) — sane model coords vs zeros/garbage. |
| 3 | **Perspective-divide / clip flags** (PA_CL_VTE_CNTL, PA_CL_CLIP_CNTL) wrong for 3D → geometry clipped out or W-divide misapplied. | `sys_flags` already maps the vte fmt bits + user clip planes; partial coverage, but a 3D-specific combo (e.g. clip_disable, w0_fmt) could still kill it. | `NHL_BETA_DEPTH_DIAG` logs vte + ndc_scale/offset; compare a 3D draw's clip flags vs base. |
| 4 | **Depth / reversed-Z culling** — per-pass depth clear added, but zfunc/reversed-Z means the player fails the test. | Per-pass clear already landed and the player is *still* missing, so depth is **likely not the (sole) cause** — but cheap to rule out outright. | `NHL_BETA_DEPTH_FORCE_ALWAYS` (line 1150): if the player appears, it was depth. |
| 5 | **Multi-level RTT chain / shared-RT bleed** — player built across intermediate resolves; a later pass samples an intermediate that the single shared RT + no-color-clear corrupts, or the intermediate's own geometry pass is the empty one. | Real, but downstream of 1–4: if the *first* geometry pass already rasterizes nothing (Step 0), the chain is moot. Investigate only if Step 0 says geometry DID rasterize. | Per-resolve-dest PNG dump (small build step) — find which chain level goes empty. |
| 6 | **3D texture/sampler bindings** wrong → player untextured. | Lowest: a missing texture yields an **untextured** player, not an **invisible** one. Can't explain zero raster. | Deferred; only relevant once geometry rasterizes. |

## Investigation order (front-load the single most decisive test)

### Step 0 — Does the raw player geometry rasterize ANYTHING? (bifurcates the whole problem)
The one test that splits "geometry doesn't rasterize" (Steps 1–4) from "geometry rasterizes
but is lost in the resolve/composite chain" (Step 5), using **no new code**.

- **Diagnostic:** `NHL_BETA_EDRAM_DIAG` to classify draws (the 199 half-res `640×360` 3D
  passes vs the `1280×720` UI/composite), then `NHL_BETA_MAX_DRAW=N` bisection to cap
  rendering at the boundary **right after the player's geometry pass and before the next
  guest clear/resolve**; IssueSwap dumps the raw offscreen scratch.
- **Confirm/kill:** scratch shows a recognizable player silhouette ⇒ **geometry rasterizes;
  jump to Step 5** (loss is in the resolve/composite chain). Scratch blank across every
  geometry-pass boundary ⇒ **geometry rasterizes nothing; proceed to Step 1**.
- **Oracle:** `replay_scene02_full.png` (player center-right). Single-frame base
  `replay_frame.png` shows the same geometry as a silhouette — that is the shape to look for.
- **Caveat (fact 1):** the shared RT is guest-cleared between passes, so pick the MAX_DRAW
  boundary from the EDRAM_DIAG draw classification. **If clean MAX_DRAW windows can't isolate
  it, the first build sub-step is a per-resolve-dest PNG dump** (write each
  `beta_flat_resolves_` entry to disk) — cheap, diagnostic-only, and it directly answers
  "which chain level is empty" for Step 5 too.

### Step 1 — Float-constant / WVP parity (prime suspect; do first if Step 0 says "empty")
- **Diagnostic:** `NHL_BETA_BIND_DIAG` on the player's VS draws. Read **`dyn=`**
  (`float_dynamic_addressing`), `float_count`, the `float_bitmap`, and the dumped first
  non-zero `c[i]` float4 rows (the WVP). Also check the vertex-index sys constants
  (`endian/offset/min/max`, line 2348) — a tiny `max`/wrong `offset` collapses all SV_VertexIDs
  to one guest vertex (verts coincide → no raster).
- **Confirm/kill:**
  - `dyn=1` on the player VS ⇒ **Hypothesis 1 confirmed** — the sparse pack is the wrong CBV
    layout. (Menu VS will read `dyn=0`, explaining why 2D is at parity.)
  - `dyn=0` but the dumped `c[i]` WVP rows are zero/garbage where base shows 4 non-trivial
    rows ⇒ a packing-order or source-index bug in the static path; still a `pack_floats` fix.
  - WVP rows sane and `dyn=0` ⇒ kill Hypothesis 1, go to Step 2.
- **Likely fix location:** `pack_floats`
  ([nhl_command_processor.cpp:1849](../renderer/core/nhl_command_processor.cpp#L1849)) — add a
  `float_dynamic_addressing` branch that uploads the **full contiguous `[0, float_count*4)`
  range at natural indices** (mirroring base `UpdateBindings`), keeping the sparse pack only
  for static shaders. VS and PS branch independently.
- **Oracle:** byte-diff the uploaded VS float CBV against an α-path capture for the same draw
  (build-order §4 mitigation: "diff our generated CBV bytes against an α capture before
  trusting pixels"), then visual vs `replay_scene02_full.png`.

### Step 2 — Vertex fetch/format for 3D
- **Diagnostic:** `NHL_BETA_BIND_DIAG` vertex dump (line 2316) on the player draws — fetch
  `addr`, `size`, `stride_words`, `endian`, guest-RAM `nz` non-zero count, and the decoded
  first vertices' positions/attribs.
- **Confirm/kill:** positions decode to sane model coords ⇒ fetch is fine (kill). Zeros/garbage
  or `nz=0` ⇒ wrong fetch base/stride/residency.
- **Likely fix location:** the fetch-constant upload (`va_fetch`,
  [nhl_command_processor.cpp:1838](../renderer/core/nhl_command_processor.cpp#L1838)) and the
  shared-memory residency range (`NHL_BETA_SHMEM_MB`, default 512 MB — verify the player's
  vertex addresses fall inside it).
- **Oracle:** same as Step 1.

### Step 3 — Perspective-divide / clip flags
- **Diagnostic:** `NHL_BETA_DEPTH_DIAG` (vte, ndc_scale/offset, VPORT regs, z-range) on a 3D
  draw; compare vte fmt bits + PA_CL_CLIP_CNTL (ucp_ena, clip_disable) against the base for the
  same draw.
- **Confirm/kill:** a 3D-only flag combo (e.g. `vtx_w0_fmt`/`clip_disable`) mishandled in
  `sys_flags` ([nhl_command_processor.cpp:1606](../renderer/core/nhl_command_processor.cpp#L1606)).
- **Likely fix location:** the `sys_flags`/clip-plane block (lines 1606–1646).

### Step 4 — Depth / reversed-Z (cheap rule-out, can run in parallel with Step 1)
- **Diagnostic:** `NHL_BETA_DEPTH_FORCE_ALWAYS` (line 1150).
- **Confirm/kill:** player appears ⇒ it was a depth/zfunc/reversed-Z cull → fix the per-pass
  depth clear value (`beta_depth_clear_`) / zfunc mapping. No change ⇒ depth is not the cause
  (expected, given per-pass clear already landed).

### Step 5 — Multi-level RTT chain (only if Step 0 says geometry DID rasterize)
- **Diagnostic:** per-resolve-dest PNG dump (the Step-0 build sub-step) — walk the chain and
  find the first dest that is empty; correlate which draw samples it (`NHL_INJECT_CORRELATE`
  style) vs which pass writes it.
- **Likely fix:** the single shared offscreen RT cannot isolate intermediate surfaces — if a
  pass samples an intermediate while rendering into the same RT, give each guest color surface
  (`RB_COLOR_INFO.color_base`) its own flat host RT (the "flat multi-pass RT manager" already
  scoped in [[highcut-h3-flat-rt]]). This is the larger build; only take it if Step 0 proves
  geometry rasterizes and the loss is downstream.

## Likely fix locations in `RenderBetaOwnedDraw` (ranked by probability)

1. **`pack_floats` dynamic-addressing branch** (line 1849) — **most probable single fix.**
2. Fetch-constant / shared-memory residency for 3D vertex layouts (lines 1838, 1893).
3. `sys_flags` clip/vte combination for vport_xform draws (line 1606).
4. Per-pass depth clear value / zfunc (lines 1079, 1144) — low probability.
5. (Structural) per-surface flat RTs instead of one shared offscreen RT (Step 5 only).

## Done criterion

- **Primary:** beta `NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_BETA_DEPTH=1 NHL_BETA_FLAT=1
  NHL_BETA_CAPTURE_FRAME=150` of the scene_02 stream renders the player **center-right,
  upright, textured Bruins jersey**, matching `replay_scene02_full.png` (composite over black).
  Intermediate gate: the VS float CBV byte-matches the α capture for the player draws.
- **Generalization (required, don't skip):** prove on a **second 3D scene** — scene_04 arena.
  Per [[gameplay-trace-missing-textures]], trace replay of 3D scenes has texture/data gaps, so
  use a **self-contained `NHL_CAPTURE_FULL` capture** of scene_04 (or a live takeover), not a
  cold single-frame replay, or the "after" image is unreadable. The fix is parity work that
  must hold for all 3D (gameplay included), not a scene_02 special-case.

## Scope / risk

- This is the **documented deep Tier-1 3D-parity core** (build-order §4.1: "reproduce the CP's
  binding and constant computation exactly"). Plausibly **multi-session**.
- **Best case (likely):** Step 1 confirms dynamic float addressing; the `pack_floats` branch is
  a localized, high-confidence fix that lands the player and generalizes immediately. This is
  why Step 1 is front-loaded right after the Step-0 bifurcation.
- **Worst case:** Step 0 shows geometry rasterizes but is lost in the chain (Step 5) → the
  larger per-surface flat-RT manager, a separate build.
- **Risk of silent wrong-pixels:** binding/constant mismatches don't crash — always gate on the
  **CBV byte-diff vs the α oracle** before trusting a pixel result (build-order §4 mitigation).
- RenderDoc is impractical (~158× slowdown, never reaches f150) — the plan is **all in-engine
  diagnostics**, which already exist for every step except the per-resolve-dest PNG dump.
- All work stays gated behind `NHL_BETA_FLAT`; the validated 2D menu/intro path
  (`guest_w == beta_rt_width_`) is untouched, so parity regressions are contained to the 3D
  subset.
