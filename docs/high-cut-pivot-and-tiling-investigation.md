# Pivot: from low-cut (rexglue/EDRAM) toward high-cut (own the D3D9→D3D12 translation)

> Written 2026-06-10 at the end of a long ROV "green player" investigation. Part 1 summarizes WHY the
> project is considering an architectural pivot. Part 2 is a self-contained prompt to start the first
> investigation step in a new session: **how does NHL Legacy do its tiling?** That single fact decides
> whether the high cut actually buys us anything.

---

## Part 1 — The pivot, summarized

### Where we were
Rendering the scene_02 create-a-player 3D model had three candidate paths in the beta backend, all partial:
- **Host-RT** (SDK path=0) — background renders, 3D player geometry clipped out by the fold scissor-clamp.
- **ROV** (SDK path=1) — player at the CORRECT position + shape, but tinted GREEN.
- **Offscreen** (beta flat RTV, the shipped default) — FULLY TEXTURED player, but mispositioned (far-right + wrap).

### What this session established (the terminus)
1. **The console renders this frame via ROV**, not host-RT (RenderDoc-proven: the base command processor's
   player draws have no bound RT + Forced Sample Count 4 = pixel-shader-interlock). So ROV is the
   architecturally-correct path; host-RT is not even what the hardware does.
2. **The ROV "green" is render-side EDRAM**, and after exhaustive elimination — RenderDoc-confirmed
   viewport / VS+PS shader hashes / rasterizer state / forced-sample-count all match the console, AND a
   field-by-field diff of beta's ROV system constants vs upstream Xenia's `UpdateSystemConstantValues`
   showed **every constant matches** — the green is **NOT any per-draw parameter**.
3. ⇒ The green is a **closed-SDK EDRAM-state / sequencing divergence**: the depth EDRAM that beta's ROV
   pixel shaders READ differs from the base command processor's, because of how the shared EDRAM buffer is
   sequenced (clears / interlock barriers / ownership / draw order) across beta's *reconstructed* draw
   stream vs. the native one. Closing it needs the SDK's render-target-cache source (or a faithful reimpl
   of that protocol). Full evidence trail in `[[rov-green-player-is-fold-color]]`.

### The realization that motivates the pivot
The beta "owned-D3D12-backend" is an attempt to get **high-cut renderer control** (own PSOs, reconstruct
draws, manipulate the pipeline) **from a low-cut foundation** (rexglue's PM4/Xenos EDRAM model). The green
wall is exactly the seam where that ambition meets the hardware model it can't escape. The friction isn't a
one-off bug — it's the **ceiling of the low cut** for a project that wants to *manipulate and enhance* the
renderer.

### The two intercept levels (the core mental model)
- **Low cut (rexglue / PM4 / Xenos):** run the game's recompiled Xbox D3D9 runtime, which emits real Xenos
  command packets; a Xenia-derived layer decodes them. You hold **EDRAM tiles, sample masks, resolve
  rects** — the predicated-tiling fold is already baked in. Faithful, mature, but every enhancement
  negotiates with the 360 hardware model.
- **High cut (own the D3D9→D3D12 translation, "Unleashed Recompiled"-style):** don't run the Xbox D3D9
  runtime; reimplement the D3D9 API on the host. You hold **meshes, materials, render targets, shaders,
  passes**. EDRAM never appears (it only existed for the 360's 10 MB framebuffer, which the host lacks), so
  the fold disappears. Enhancements (4K/internal-res, ultrawide, modern AA, texture/shader mods, framerate)
  become "change a number / swap a resource."

### The tradeoff (eyes open)
The high cut's freedom is real and is why the modern recomps (Unleashed, the N64 ones) use it. The cost:
you **reproduce the Xenos tricks yourself** (predicated tiling, MSAA-resolve-as-effect, depth-as-texture),
you **reimplement a chunk of the quirky 360 D3D9 API**, and you **lose rexglue's maturity** — so
time-to-parity goes up even as the enhancement ceiling does.

### What carries over to a high cut (so the rexglue work isn't wasted)
- The **Xenon CPU recompilation** (PPC→x86) — 1:1 reusable, the biggest chunk.
- The **Xenos shader translator** (ucode → DXBC) — still needed; the game's shaders are Xenos microcode at
  either boundary.
- This session's **render-pipeline map** (the multi-pass fold→resolve→composite chain) — it's the spec for
  what a high-cut renderer must produce.
- Texture/asset understanding.
- NOT reusable: the PM4-level EDRAM/resolve/tiling machinery — that's exactly what the high cut deletes.

### The decision lens
- North star = **enhancements / mods / a definitively better version** → the high cut is the aligned
  architecture; the green wall is a *signal* that the low cut's ceiling is reached.
- North star = **faithful, playable, soonest** → rexglue gets there faster; request the RT-cache source to
  close the green.

### Two unknowns that decide whether the high cut delivers (→ Part 2)
1. **Does NHL Legacy use the OFFICIAL D3D9 predicated-tiling API (`BeginTiling`/`EndTiling`) or do tiling
   MANUALLY** (game-side window-offset passes + explicit Resolve)? Official → the high cut renders full-res
   and the fold genuinely vanishes. Manual → the tiling lives in *game* logic, so the high cut must
   recognize and collapse the pattern itself (the win is smaller / conditional).
2. **How much of the 360 D3D9 API surface does this game actually touch?** Scopes the reimplementation
   effort.

These are answerable by looking at the game's D3D9 call sites — a far better next step than another EDRAM
experiment. Part 2 is the prompt to do it.

---

## Part 2 — New-session prompt: investigate NHL Legacy's tiling

> Paste everything below this line as the opening prompt of a new session.

---

We are evaluating an architectural pivot for `e:\Repositories\nhl-legacy-recomp` — from the current
**low-cut** GPU approach (the rexglue / Xenia-derived runtime that emulates the Xenos GPU at the PM4 level
and models EDRAM) toward a **high-cut** approach (reimplement the Xbox 360 D3D9 API → D3D12 directly,
"Unleashed Recompiled"-style, no EDRAM model). Full background is in
`docs/high-cut-pivot-and-tiling-investigation.md` Part 1 and the memories `[[rov-green-player-is-fold-color]]`,
`[[beta-scene04-projection]]`, `[[unleashed-recompiled-no-edram]]`, `[[trace-capture-replay-constraint]]`.

The pivot's payoff hinges on ONE technical question, and your job this session is to answer it with
evidence — **do NOT change rendering code; this is investigation only.**

## The decisive question
**Does NHL Legacy 360 perform its wide-render-target tiling via the OFFICIAL Xbox 360 D3D9 predicated-tiling
API (`IDirect3DDevice9::BeginTiling` / `EndTiling`), or does the game do it MANUALLY** (its own loop that
sets `PA_SC_WINDOW_OFFSET` / window offsets, re-renders the scene per tile, and calls `Resolve` itself)?

Why it matters: at the high cut you don't run the Xbox D3D9 runtime, so:
- **If OFFICIAL** (`BeginTiling`/`EndTiling`): your D3D9 reimpl can simply IGNORE tiling and render the whole
  frame full-res to a host RT. The 640-pitch fold (the thing that caused the entire "green player" saga)
  **vanishes for free**. Big win, clean.
- **If MANUAL**: the tiling is in game code (the game itself issues the two window-offset passes + resolves
  you see in the trace). A D3D9 reimpl still receives those calls, so you must DETECT and COLLAPSE the
  pattern (un-fold it) yourself — the win is conditional and more work.

## What we already know (from the prior session, all verified)
- The create-player player (scene_02, frame 30) is rendered with **two `PA_SC_WINDOW_OFFSET` passes**
  (window_x_offset = 0 and = -640) into a **640-pitch** EDRAM surface, then **resolved** to a 1280×720
  texture, then composited. This is the classic predicated-tiling signature for a 1280-wide RT that exceeds
  the EDRAM tile budget at that format/MSAA. (Seen at the PM4 level in the `.xtr` trace.)
- The trace files are PM4-level: `out/build/win-amd64-relwithdebinfo/gpu_trace/scene_*/454109EC_stream.xtr`.
- The console/base path renders this via ROV; it is faithful. The fold only exists because of the 360's
  10 MB EDRAM.

## Concrete starting points (pick the cheapest that yields a definitive answer)
1. **Static, in the recompiled game / the XEX.** The Xbox 360 D3D9 runtime is statically linked into the
   game (XDK). `BeginTiling`/`EndTiling` are known XDK `D3DDevice` methods. Find them in the recomp's
   sources / symbol map (or disassemble the original XEX at `H:\Emulators\games\XBOX\NHL Legacy - Vanilla`)
   and check whether they are CALLED, and from where (engine render loop vs. never). If `BeginTiling` is
   called around the player/3D render → OFFICIAL. If it's never called and the window-offset passes come
   from game render code → MANUAL.
2. **Identify the player render call site.** Trace how the create-player 3D model render is set up in the
   game code: does it call `BeginTiling`/`EndTiling`, or does it manually loop + set window offset + render
   + `Resolve`? The latter is MANUAL.
3. **PM4 signature.** Official predicated tiling emits a recognizable runtime setup (the D3D9 tiling loop +
   predication packets); manual emits raw game-driven register writes. The `.xtr` reader / the existing
   trace tooling (`[[trace-capture-replay-constraint]]`, `nhl-trace-replay`) can dump the packet stream
   around the window-offset passes — look for predication/tiling-runtime packets vs. plain register writes.

## Also scope (secondary, but do it while you're in there)
**Enumerate the 360 D3D9 API surface this game actually uses** — from the XEX import table / the recomp's
D3D9 call sites. A rough count + the notable/quirky calls (tiling, resolves, EDRAM RT formats, custom
shaders, predication, GPU constants) is enough to estimate the high-cut reimplementation effort. Note the
reference ecosystem for the high cut: the Unleashed-Recompiled toolchain (XenonRecomp for the CPU,
XenosRecomp for shaders) is the closest existing prior art for a D3D9-level 360 recomp — worth checking how
they handle (or sidestep) predicated tiling.

## Deliverable
A short written verdict: **OFFICIAL vs MANUAL tiling** (with the evidence — call site or PM4 signature), and
a one-paragraph read on whether the high cut would make the fold vanish for this game. Plus the rough D3D9
surface scope. Update `docs/high-cut-pivot-and-tiling-investigation.md` and add/append a memory note. Do not
modify renderer code.

---

## Part 3 — VERDICT (2026-06-10, investigation complete; PM4-trace evidence)

### Answer: **NEITHER. The game does no predicated tiling at all.**
The question assumed the wide-RT "fold" is produced by a tiling loop (official `BeginTiling`/`EndTiling` or a
manual game-side window-offset loop). **That premise is false.** Direct decode of the captured PM4 stream
proves there is *no tiling construct of any kind* — neither the official runtime path nor a manual one. The
"fold" is not tiling; it is a single-pass **wide-into-narrow EDRAM render** (content wider than the EDRAM
`surface_pitch`), resolved out in one copy. This is the *best-case* outcome for the high cut (see below).

### Evidence (all from `out/build/.../gpu_trace/{scene_00,scene_02,scene_04}/454109EC_stream.xtr`, decoded with `tools/tiling_probe.py` + `tools/frame_dump.py`; reproducer commands at the bottom)

1. **No GPU binning machinery — anywhere.** Across all scenes: **0** `DRAW_INDX_BIN` (0x34) / `DRAW_INDX_2_BIN`
   (0x35), and **0** `SET_BIN_MASK` (0x50) / `SET_BIN_SELECT` (0x51) / `SET_BIN_BASE_OFFSET` (0x4B). The Xbox 360
   D3D9 runtime implements predicated tiling with these binning draws + bin state; their total absence rules out
   the official binning-predicated-tiling path. Every draw is plain `DRAW_INDX`/`DRAW_INDX_2`.
2. **`PA_SC_WINDOW_OFFSET` (ctx reg 0x2080) is ALWAYS 0.** Written 32809× in scene_02, 4452× in scene_00, 38401×
   in scene_04 — **every single write is 0x00000000**; a nonzero window offset never appears in any of the 5
   scenes. `PA_SC_WINDOW_SCISSOR` is wide-open (TL=0, BR=0x20002000 = 8192×8192). There is no per-tile window
   shifting in the command stream at all. (Decoder validated against `RB_SURFACE_INFO` surface_pitch, which
   correctly reports the expected 320/640/800/1280 spread, and against `RB_MODECONTROL` kCopy resolves.)
3. **No hidden register-load path.** **0** `LOAD_CONSTANT_CONTEXT` (0x2E) packets in scene_02 — so no register
   value is loaded from a memory block behind the decoder's back. The window-offset register is set *only* by
   type-0 / `SET_CONSTANT` writes, all captured, all 0.
4. **The "player" is a single pass, not two tiles.** scene_02 frame 30's create-player 3D model = **one 75-draw
   render pass with a 1280-wide viewport** (`PA_CL_VPORT_XSCALE`=640 → width 1280) **into a `surface_pitch`=640,
   2×MSAA EDRAM surface, then ONE whole-surface `kCopy` resolve to 1280×720.** Window offset 0; geometry issued
   once. The earlier "two passes, win=0 and win=-640" model was the multi-RTT chain (the player is rendered into
   several intermediate textures, each a full 1280-wide single pass + its own resolve) mis-read as tile passes.
5. **Resolves are per-surface, not per-tile.** One `kCopy` per render target. Official `EndTiling` emits one
   resolve *per tile*; that pattern is absent.
6. **Generalises to real 3D gameplay.** scene_04 (the full broadcast arena frame, 90 frames) shows the identical
   signature: 0 BIN draws, 0 bin state, window offset always 0, the same pitch-640-with-1280-viewport pattern.

### What the "-640 window offset" in the prior sessions actually was
`[[beta-scene04-projection]]`'s "DEFINITIVE" two-pass predicated-tiling model and the entire
`[[rov-green-player-is-fold-color]]` green saga rest on a `win_off=(-640,0)` reading. That value was printed by
the **`NHL_BETA_WINOFF` prototype branch** (`nhl_command_processor.cpp:1385-1409`, env-gated, off by default)
reading `register_file_->Get<reg::PA_SC_WINDOW_OFFSET>()` during `NHL_BETA_EDRAM` replay. **The guest stream
never writes a nonzero window offset (proven above), so the -640 did not come from the game.** The most
consistent explanation: it originates *inside the closed rexglue/Xenia EDRAM emulation* — to reproduce a
wider-than-pitch render on a host GPU that has no EDRAM address-wrap, the SDK's render-target cache splits the
wide draw into window-offset sub-passes, and the beta draw hook observed that SDK-synthesized state. (Not
directly instrumented — the SDK ships headers only — but it is the only path left once type-0/SET_CONSTANT/
LOAD_CONSTANT_CONTEXT are all shown to write 0.) **Net: the fold is an artifact of *emulating* EDRAM, not a
property of the game.**

### Does the high cut make the fold vanish? — YES, more cleanly than the "official" case
There is no `BeginTiling` to stub and no manual loop to collapse. A D3D9→D3D12 reimpl intercepts, for the
player: `CreateRenderTarget` → `SetRenderTarget` → `SetViewport`(1280-wide) → `DrawIndexedPrimitive`×75 →
`Resolve`/`StretchRect` → 1280×720 texture. It allocates a **full-size host render target** and rasterises
1280-wide natively. **The fold never forms, because the fold exists only inside the EDRAM tile-addressing model,
which the high cut deletes** (same reason Unleashed-Recompiled has zero edram/tile/pitch code — see
`[[unleashed-recompiled-no-edram]]`). Supporting inference: D3D9 requires the viewport to fit within the bound
render target, so the game's *logical* D3D9 RT is ≥1280 wide; `surface_pitch`=640 is purely an EDRAM-side
packing detail (2×MSAA). The single residual (a minor reimpl design choice, **not** the closed-SDK fold
problem): size host RTs to the logical/viewport/resolve extent (1280), not to the GPU `surface_pitch` (640).
Confirming the exact `CreateRenderTarget` width is the one thing PM4 alone can't show — but it does not gate the
conclusion.

### D3D9 surface scope (secondary)
- **Topology = exactly the Unleashed-Recompiled model.** D3D9 is statically linked into the XEX (no D3D9
  imports; XEX strings carry no D3D9 symbols) and drives the **Vd\* video-driver kernel** (`VdInitializeRingBuffer`,
  `VdSwap`, `VdGetSystemCommandBuffer`, `VdRetrainEDRAM`, `VdSetGraphicsInterruptCallback`, `VdQueryVideoMode`…,
  all present in `generated/default/`) → PM4 ring buffer. rexglue intercepts at the ring (low cut). The high cut
  hooks the statically-linked D3D9 functions (Unleashed's `GUEST_FUNCTION_HOOK` pattern; XenonRecomp CPU +
  XenosRecomp shaders are the direct prior art and **carry over 1:1** — our Xenon recomp + Xenos shader
  translator already exist).
- **PM4-feature → D3D9-call surface the reimpl must cover** (from the opcode/register inventory):
  `DRAW_INDX`/`DRAW_INDX_2`→Draw[Indexed]Primitive[UP]; `SET_CONSTANT`(context)→SetRenderState/SetViewport/
  SetScissorRect/depth-stencil-blend-raster state; `SET_CONSTANT`(ALU)/`LOAD_ALU_CONSTANT`→Set{Vertex,Pixel}
  ShaderConstantF; `IM_LOAD_IMMEDIATE`→Set{Vertex,Pixel}Shader (Xenos ucode — already handled by our shader
  translator); texture fetch constants→SetTexture/SetSamplerState/CreateTexture; `RB_SURFACE/COLOR/DEPTH_INFO`→
  CreateRenderTarget/SetRenderTarget/SetDepthStencilSurface; `kCopy`+`RB_COPY_*`→Resolve/StretchRect (EDRAM→tex);
  `EVENT_WRITE*`→queries/fences/Present sync.
- **Explicitly ABSENT — the reimpl can skip these entirely:** `BeginTiling`/`EndTiling`, predication / predicated
  tiling, GPU binning. (The per-packet predicate bit is set on ~100% of `DRAW_INDX` but no predication condition
  is ever established — it is the runtime's benign default, not tiling.) MSAA 1×/2×/4× present (→ modern MSAA or
  supersample); single render target bound per pass (per `[[frame-feature-inventory]]`).
- A precise entry-point **count** needs static recovery of the D3D9 call sites in the recomp (`nhllegacy_functions.toml`
  is only scanner-miss patches — no D3D9 symbols); deferred. But the feature set is the **standard core** (~a
  couple dozen entry points), and Unleashed's `video.cpp` covers ~the same surface — so the reimpl scope is
  "known and bounded," not open-ended.

### Bottom line for the pivot decision
The single fact the whole pivot was gated on resolves **in favor of the high cut, decisively**: the game emits
no tiling, so the fold — the root of the ROV green, the scene_04 mis-projection, and the host-RT geometry-vanish
(all the same EDRAM-fold defect) — is an artifact the high cut **never produces**, rather than a bug it must
fix. The low cut will keep fighting it (it must emulate EDRAM tile-addressing faithfully through a closed,
headers-only SDK). The carry-over (Xenon CPU recomp + Xenos shader translator) is intact and is the bulk of the
high-cut work, and the D3D9 surface to reimplement is the standard core minus the exotic tiling/predication the
game never touches.

### Reproduce
```
python tools/tiling_probe.py out/build/win-amd64-relwithdebinfo/gpu_trace/scene_02/454109EC_stream.xtr
#   -> window-offset histogram (all 0), BIN-draw total (0), predicate-bit usage
python tools/frame_dump.py  out/build/win-amd64-relwithdebinfo/gpu_trace/scene_02/454109EC_stream.xtr 30
#   -> in-order frame structure: single-pass renders (pitch 640, 1280-wide vport) + per-surface resolves
# Cross-scene (scene_00 menu, scene_04 gameplay): run tiling_probe.py on each scene's 454109EC_stream.xtr.
```
No renderer code was modified (only read-only `tools/*.py` analysis scripts were added).
