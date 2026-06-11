# Handoff prompt — faithful EDRAM fold (closed-SDK render-target-cache work)

> Paste this whole file as the opening prompt of a new session. It is self-contained.
> The companion memory note `[[beta-scene04-projection]]` (UPDATE 1–4) has the full archaeology;
> this prompt is the distilled, actionable brief.

---

## Mission

Make the beta owned-D3D12 backend render the scene_02 create-a-player **3D player model at the
correct (oracle) on-screen position** by driving the SDK's EDRAM render-target cache to perform the
Xenos **pitch<width fold + un-fold** faithfully — i.e. through the real EDRAM/resolve pipeline, NOT
the offscreen flat-RT path.

This is the architecturally-correct track. The offscreen path is ALREADY SHIPPED and renders a
complete, fully-textured player, but with a positional artifact (player splatted far-right + a left
wrap fragment) because a flat RT can't express the fold. Solving the faithful fold:
- fixes the position to match the oracle,
- preserves the EDRAM resolve surface as a clean injection/manipulation seam,
- **generalizes to all 3D** (scene_04 gameplay is the same blocker — the user's "player is a
  stepping stone to all 3D" intuition is confirmed correct).

## The exact technical problem

The create-player 3D model is drawn via **Xenos predicated tiling: two `PA_SC_WINDOW_OFFSET`
passes into ONE 640-pitch folded EDRAM surface**, then resolved (un-folded) to a 1280×720 texture.

- **Pass A** (beta edram-draw #271-277): `vte=0x43F surf_pitch=640 extent=(1280,720) win_off=(0,0)`,
  ndc_scale=(1,1,1). Renders the left 640 cols.
- **Pass B** (#280-285): `surf_pitch=640 extent=(640,720) win_off=(-640,0)`, SAME
  `VPORT(xs=640 xo=640 ys=-360 yo=360)`. GetHostViewportInfo squishes the -640 origin into
  ndc_scale=(2,1,1) ndc_offset=(-1,0,0). This is the fold's second half.
- The fold for pitch=640 is a **non-affine per-16px-tile-row remap** (the resolve walks tiles
  wrapping past pitch into the next tile rows). A plain D3D12 viewport/scissor CANNOT express it.
- **Resolves** (`copy_dest=(1280x720)` from a `surf_pitch=640` surface) un-fold the surface to
  1280×720 textures: #13→0x1A7D9000, #14→0x1AF09000, #15→0x1A7ED000, #16→0x1AF1D000.
- The on-screen player comes specifically from the **pass-B resolves 0x1A7ED000 / 0x1AF1D000**
  (proven by SKIP_RESOLVE bisection, see below). 0x1AF09000 (pass-A) is secondary.

Beta's own resolves of these fold surfaces come out **black / comb-garbage** — the geometry never
lands in the host-RT region the SDK resolve reads. THAT is the bug to fix.

## What is PROVEN (don't re-litigate)

1. **Geometry + textures render fine** — the offscreen path (`-Edram 0`) draws the whole frame incl.
   the textured player. The bug is 100% the EDRAM fold render + un-fold resolve.
2. **The fix is localized + the downstream composite already works** — `NHL_BETA_SKIP_RESOLVE="15,16"`
   (inject the trace's correct bytes into 0x1A7ED000/0x1AF1D000 instead of beta's black resolve) makes
   the player APPEAR at the correct oracle position. So: produce a correct un-folded pass-B surface and
   beta composites it for free.
3. **The SDK cache CAN do pitch<width** — the base/oracle path renders this exact player correctly
   using the SAME SDK `D3D12RenderTargetCache` at pitch=640. So beta is **mis-driving** the cache, not
   missing a capability.

## What FAILED (do NOT repeat without new information)

- `NHL_BETA_VP_VPORT` (derive x_max from VPORT xscale) + WIDE-RT (force surf_pitch 640→1280): render
  uses 1280-pitch host RT while the guest RESOLVE still reads pitch=640 → different cache keys →
  resolve reads empty.
- `NHL_BETA_WINOFF` block-stack placement (un-squish pass-B, translate to `y += wrap*img_h`): player
  still blank; beta resolve of 0x1A7ED000 → content≈541 vs target ≈25000 (dump is black except a thin
  bottom sliver).
- No-hack EDRAM baseline: content≈21.
- bypass-fold (custom flat-RT + custom resolve): rejected because pass-A and pass-B land the player at
  DIFFERENT positions the composite was authored against → would have to reproduce the non-affine
  un-fold anyway. More work than the faithful path.

## THE HARD CONSTRAINT that defines this session

**The SDK is closed — headers only.** `E:/Tools/rexglue-sdk/0.8.0/win-amd64/include/rex/graphics/`
has `render_target_cache.h`, `d3d12/render_target_cache.h`, `command_processor.h`,
`d3d12/command_processor.h` — but NO .cpp. The implementation is in the static lib. So "drive the
cache exactly as base does" cannot be done by reading base source. Strategies that work around this:

1. **It's Xenia-derived.** The SDK is a fork of Xenia's GPU stack. Read Xenia's open-source
   `d3d12_render_target_cache.cc` / `render_target_cache.cc` / `d3d12_command_processor.cc` (GitHub:
   xenia-project/xenia, `src/xenia/gpu/...`) to learn the EXACT register-derived state the native
   command processor feeds into `Update()` / `Resolve()` for a window-offset pitch<width surface, and
   how it computes host-RT extents + the tile-transfer for the fold. Then mirror that driving in beta's
   `RenderBetaOwnedDraw`. The header signatures in the rexglue SDK should match Xenia closely enough to
   map the logic over.
2. **ROV path (alternative).** `D3D12RenderTargetCache` has a ROV mode (`path=1` per the init log;
   beta currently runs `path=0` host-RT). In ROV the PS computes the per-pixel EDRAM tile address, so
   the fold is handled in-shader and the host-RT pitch mismatch disappears. Check whether the SDK lets
   beta select the ROV path and whether it Just Works for the fold. (Header:
   `d3d12/render_target_cache.h`.)
3. **Diff driving via instrumentation.** Run the BASE path (no takeover) with whatever SDK-side logging
   exists, capture how base calls into the cache for draws #271-285 + resolves #13-16, then make beta
   match. The base path is reachable via the oracle recipe below.

## Where the code is

- `renderer/core/nhl_command_processor.cpp` (~3250 lines):
  - `RenderBetaOwnedDraw` (line 960) — main owned-draw path. `edram = beta_edram_enabled_` (978).
    `use_rtcache = edram || (depth...)` (1055). Drives `beta_render_target_cache_->Update()` (~1087).
  - Viewport setup (1280-1392): `x_max=8192` for edram&&vport_xform (1296), scissor clamp to
    surface_pitch (1322-1325). `NHL_BETA_WINOFF` prototype (1338-1363, OFF). Non-edram res-mismatch
    branch (1372-1392).
  - `EDRAM_DIAG` (1414) logs vte/zfunc/edram_tile/surf_pitch/extent/win_off.
  - bind-diag PS texture logging (1811-1844).
  - `WriteBetaGpuDumps` (2341-2393) — the GPU-readback dumper (untile formula ~2369-2374).
  - IssueDraw takeover dispatch (2802-2875): em==kCopy→`BetaResolveEdram()`, else→`RenderBetaOwnedDraw()`.
  - `beta_edram_enabled_ = getenv("NHL_BETA_EDRAM")` (2666).
  - `BetaResolveEdram` (3088-3149): delegates to
    `beta_render_target_cache_->Resolve(*memory_, *beta_shared_memory_, *beta_texture_cache_, ...)`.
    `NHL_BETA_SKIP_RESOLVE` skips 1-based resolve indices (lets the texture cache upload trace data).
- `renderer/core/nhl_command_processor.h`: beta members incl. `beta_render_target_cache_` (202),
  `beta_shared_memory_` (201), `beta_offscreen_rt_` (236).
- SDK headers: `E:/Tools/rexglue-sdk/0.8.0/win-amd64/include/rex/graphics/{d3d12/,}render_target_cache.h`,
  `.../command_processor.h`.

## Build

VS2022 BuildTools vcvars64 + LLVM on PATH (see `[[rexglue-build-environment]]`). Build via
`cmd.exe /c "<absolute path>\_build_beta.bat"` from
`out/build/win-amd64-relwithdebinfo/`. Keep the build GREEN.

## Validation loop

- **Run (faithful EDRAM):** `& "e:\Repositories\nhl-legacy-recomp\run_edram.ps1" -Scene scene_02 -Frame 30 -Depth 1`
  (EDRAM on by default). Outputs `out/build/win-amd64-relwithdebinfo/beta_owned_draw.png`; logs in
  `.../logs/nhllegacy_*.log`.
- **Oracle (reference):** `& ".\run_oracle.ps1"` (or beta + `NHL_SHOT_FRAME=30`, NO takeover) →
  `live_frame.png`. Pre-captured: `out/build/.../oracle_editplayer_f30.png` (player = black silhouette
  centered x650-1050; that's correct position, textures are a separate trace gap).
- **GPU-readback (the key observability):** `NHL_BETA_GPUDUMP="0x1A7ED000,0x1AF1D000"` (+ `_FRAME`,
  `_W`,`_H`,`_ALPHA`,`_LINEAR`) dumps beta's GPU-side `beta_shared_memory_` at those addrs as
  `gpudump_<ADDR>.png`. Pass = the un-folded player surface (target content ≈25000 by the metric
  below); current beta = black.
- **Content metric (player present?):** count non-background pixels in player-isolated region
  x950-1275 using a COLOR-AGNOSTIC non-blue test: pixel is background if `(B>150 && R<90 && G<90)`.
  Injected/raw player can render GREEN (mis-swizzle), so a "dark"-pixel metric MISSES it — always use
  this or VIEW the PNG.
- **Win condition:** beta's own `gpudump_1A7ED000` / `gpudump_1AF1D000` show the un-folded player
  (content ≈ trace target), AND `beta_owned_draw.png` (EDRAM, no SKIP_RESOLVE) shows the player at the
  oracle position. Then confirm `NHL_BETA_SKIP_RESOLVE` is NO LONGER needed.

## Hard no-regression constraints (carry forward, non-negotiable)

- Menu (scene_00) + intro (scene_03) MUST stay byte-identical. The default/live path AND
  `NHL_BACKEND=beta` WITHOUT takeover MUST stay byte-identical.
- All new logic env-gated / takeover-EDRAM-only. 2D draws (vte=0x300, vport_xform=false,
  guest_w==rt_width) take the untouched 1:1 viewport branch — keep it that way.
- Depth support and texture injection must keep working. Keep the build green.
- The shipped offscreen path (the `-Edram 0` default-takeover route) must keep working as the fallback.

## Implementation constraint to remember

Beta records into a **deferred command list** that only executes at IssueSwap (end of frame), in
order. A resolve runs mid-list BEFORE the fold-pass renders execute → you cannot CPU-readback a
just-rendered RT mid-frame. The composite draws that sample the resolved dest are later in the SAME
list → the dest must be populated in `beta_shared_memory_` before they run. Any fix must live inside
this deferred-execution model (which is exactly why driving the SDK cache's own GPU-side
transfer/resolve — rather than a CPU hack — is the right shape).

## Suggested first moves

1. Pull Xenia's `d3d12_render_target_cache.cc` + `d3d12_command_processor.cc` and find how it handles a
   draw with `PA_SC_WINDOW_OFFSET != 0` and `surface_pitch < viewport width` — specifically what it
   passes to the host-RT allocation and the EDRAM tile transfer.
2. Add an `EDRAM_DIAG`-style dump of the EXACT args beta passes to `Update()` for draws #271-285, and
   reason about what differs from the Xenia logic.
3. Decide ROV-path vs host-RT-path-driving as the primary approach based on (1)/(2), then implement
   env-gated and iterate against the GPU-readback validation loop.

---

## SESSION ADDENDUM (2026-06-10) — DIAGNOSIS CORRECTED: this is NOT a fold/un-fold bug

The "faithful EDRAM fold" framing of this whole brief is **misleading**. With reliable observability the
real bug is much narrower. Read this before acting on the brief above.

**Tooling fix landed (kept, build green):** `WriteBetaGpuDumps` (nhl_command_processor.cpp ~2660) now
untiles with the SDK's exact `rex::graphics::texture_util::GetTiledOffset2D(x,y,W,2)` instead of the old
approximate macro-tile formula that SCRAMBLED every dump. Added `#include
<rex/graphics/pipeline/texture/util.h>`. Diagnostic-only (NHL_BETA_GPUDUMP-gated), no render-path change.
**The prior "beta resolves are black/comb-garbage" conclusion was an untile artifact — discard it.**

**What the reliable dumps show (scene_02 EditPlayer f30):**
- **The SDK resolve un-fold WORKS.** `SKIP_RESOLVE=15,16` (inject trace) → 0x1A7ED000 / 0x1AF1D000 are
  CLEAN, CONTIGUOUS ~640-wide players (silhouette / Ducks jersey) in the LEFT half of the 1280 dest, right
  half = background. **No comb. The 1280 is only the dest pitch; player content is ~640 wide.** The
  closed-form "non-affine per-16px comb" the Xenia research derives is real for the EDRAM addressing but
  MOOT for the output — the SDK already produces a normal image.
- **Beta's OWN resolves: the dark-red background/resolve-clear renders, the PLAYER MODEL is ABSENT.** Not
  black, not comb — just no 3D geometry.

**The actual bug:** the **vte=0x43F 3D player draws (the `win_off=(-640,0)` pass-B group) never land in
beta's EDRAM host RT during takeover**, even though the SAME draws render the player fine in the offscreen
path (`-Edram 0`). 2D background draws (vte=0x300) DO land. So it is a **3D-geometry-into-EDRAM render-target
problem**, NOT the fold, NOT the resolve, NOT projection.

**Ruled out this session (each with a reliable dump; player still absent):** depth-compare cull
(`NHL_BETA_DEPTH_FORCE_ALWAYS`), back-face cull (`NHL_BETA_NOCULL`), EDRAM reseed (`NHL_BETA_NO_RESEED`),
per-draw RT re-bind (`NHL_BETA_NO_RT_INVALIDATE`, temp gate, reverted), projection/ndc (beta's vpi matches
upstream Xenia exactly: pass-A `extent=1280 ndc=(1,1,1)`, pass-B `extent=640 ndc_scale=(2,1,1)
offset=(-1,0,0)`), draw-skip / ConfigurePipeline / Process errors (none; `Update()` returns
`bound_bits=0x3`).

**So the validation target changes:** stop trying to "reproduce the fold". The resolve already un-folds.
The goal is: **make the 3D player geometry appear in beta's EDRAM color/depth host RT** (then the existing
resolve composites it for free, exactly as it does for the injected trace bytes).

**Next step = WHITE-BOX (black-box is exhausted):** RenderDoc capture of one beta EDRAM-mode pass-B player
draw — where does its geometry go? Top suspects, ranked:
1. **Host-RT MSAA-sample mismatch** — the fold draws log `msaa=1` but resolve #16 is
   `copy_sample_select=4`; if beta's host RT for these draws is keyed at a different sample count than the
   resolve reads, the resolve dumps an empty RT. (Ties to the known "depth+MSAA not reconciled" gap.)
2. The 640-wide host RT clips pass-A's >640 geometry, AND pass-B doesn't land where expected.
3. Ownership-range / accumulated-RT desync from beta's per-draw Invalidate+Update churn.

Full detail in `[[beta-scene04-projection]]` UPDATE 5.

## SESSION ADDENDUM #2 (2026-06-10) — STOP DEBUGGING THE HOST-RT PATH. THE ROV PATH ALREADY RENDERS THE PLAYER.

The single most important finding: **`NHL_BETA_RT_PATH=rov` (the SDK's pixel-shader-interlock path) renders
the scene_02 player AT THE CORRECT ORACLE POSITION AND SHAPE** — full body + stick on the right
(x≈650-1050), matching `oracle_editplayer_f30.png`. The host-RT path (this whole brief's target) loses the
3D geometry ENTIRELY; ROV does not. So the host-RT "faithful fold" track is the WRONG bet — it's strictly
harder (player absent) than ROV (player present, one color defect left).

Verified this session (scene_02 f30, fixed untile):
- ROV `beta_owned_draw.png` = full CREATE PLAYER/EQUIPMENT UI + the 3D player at the correct position.
- ROV `gpudump_1AF1D000` (resolve #16) = a clean player silhouette in the left 640 of the dest (matches
  trace-correct shape/position). Right half has a magenta artifact (lower priority — likely not sampled by
  the composite).
- The ONLY remaining player defect is the known **green-fold additive color corruption** (see
  `[[rov-green-player-is-fold-color]]`): regional `(0,+127,+15)` additive bands split at x=640; the RED
  channel is byte-identical to the correct offscreen render, so SHAPE + MATERIALS are correct. Prior
  investigation (Codex+Claude) ruled out: missing materials, readback swizzle, ROV blend constant, EDRAM
  reseed, MSAA averaging, guest clear-color. Leading hypothesis: render-side EDRAM tile ownership/aliasing
  — the composite's left fold-band reuses EDRAM tiles holding earlier green/depth content that beta doesn't
  clear/own correctly (the host-RT path's flat RTV has no such aliasing). Possible depth-as-color aliasing
  of the depth tiles (base 736).

ROV is opt-in (`NHL_BETA_RT_PATH=rov`, nhl_command_processor.cpp:323) so it does not regress the default
menu/intro path. Caveat: ROV's 2D background showed a blue/cyan seam artifact, so making ROV the default
for 3D scenes (while keeping host-RT for validated 2D) is its own scoping task.

**REVISED RECOMMENDATION:** pursue 3D via the ROV path. The remaining work is the green-fold COLOR defect
(EDRAM tile clear/ownership for the fold band), not the host-RT geometry-placement problem. The closed-form
comb mapping and host-RT fold archaeology above are moot for ROV (ROV handles tiling in-shader).

## SESSION ADDENDUM #3 (2026-06-10) — EDRAM-readback tool BUILT (the gate every prior session needed)

`NHL_BETA_EDRAMDUMP="base:pitch:W:H,..."` (bare `1` => color base 0 + depth 736 defaults) reads beta's
LIVE `edram_buffer_` to the CPU via a self-contained compute copy (the SDK resource is private — reached
through `WriteEdramUintPow2SRVDescriptor`) and writes untiled `edramdump_b<base>.png`. Post-frame GPU-idle,
diagnostic-only, build green (needs d3dcompiler.lib+d3d12.lib via #pragma). Code: ReadbackBetaEdramRegion
(~nhl_command_processor.cpp:2700). **Usage:** end-of-frame EDRAM is cleared (resolves clear after copy), so
skip the clearing resolve to catch content — e.g. `NHL_BETA_SKIP_RESOLVE="19,20,21,22,23,24"` (resolve #19 =
frontbuffer copy+clear) + `NHL_BETA_EDRAMDUMP="0:16:1280:720"` shows the un-cleared 1280-pitch frontbuffer.

**First direct result:** the green player is **green in the raw frontbuffer EDRAM itself** — render-side,
confirmed by direct readback, not a resolve/swizzle artifact (raw EDRAM is R/B-swapped vs the resolve's
endian; swap-back gives the true blue/cyan bg, player stays green). **Next:** backward-trace with the tool
(truncate via NHL_BETA_MAX_DRAW) to see whether the player is already green at its base-0-pitch-8 source or
whether the texture-less EDRAM-direct composite (draws #560-581) injects it; `(0,127,15)`≈depth keeps
depth-tile (base 736) aliasing as the lead. Full detail in `[[rov-green-player-is-fold-color]]`.
