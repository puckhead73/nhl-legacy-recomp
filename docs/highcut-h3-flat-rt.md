# H-3 — draws into flat, logical-sized RTs (fork decided; scope characterized)

> Milestone H-3 of the hybrid high cut: render the (PM4-decoded) guest draws into
> **flat, logical-sized** render targets and validate the **fold is gone** on a 3D
> scene. This session **decided the draw-execution fork** and **characterized the real
> work with live evidence** — it did not yet produce a fold-free 3D render (that is a
> larger build than a single milestone step; see "Status" below).

## Fork decision: (a) SDK-D3D12 into flat RTs — DECIDED

The kickoff left a fork open: **(a)** keep decoding draws with rexglue's SDK-D3D12 (the
beta command processor) and render them into flat logical-sized RTs, vs **(b)**
reimplement draw execution on plume.

**Decision: (a).** Rationale (evidence below): we already own a `D3D12CommandProcessor`
subclass (`renderer/core/nhl_command_processor.cpp`, the "beta" backend) that decodes
the inlined PM4 draws, translates every Xenos shader to DXBC (verified: 106/106 on
scene_02), and renders into a **flat offscreen RTV** for the 2D path at parity. (b) would
re-derive all of that on plume. So (a) reuses the mature decode/translate/bind machinery
and localizes the new work to render-target *sizing and pass structure*.

## The real work is the MULTI-PASS RTT chain, not a viewport tweak (key finding)

The naïve H-3 idea was "the fold is `guest_w = surface_pitch` (640) instead of the
logical width (1280); drive the viewport from the logical width and the 3D scene lands
flat." **Ruled out by live evidence** — the offscreen flat-RTV path renders scene_02 to
an **empty** frame regardless:

| run (scene_02 takeover replay) | result |
|---|---|
| offscreen, default | empty (alpha 0 everywhere) |
| offscreen + `NHL_BETA_VP_FULLRT` (force full 1280 viewport) | empty |
| offscreen + `NHL_BETA_DEPTH` + `VP_FULLRT` | empty |
| EDRAM path (`NHL_BETA_EDRAM`) | empty (this trace) |

Why: **scene_02 is a 19-resolve multi-pass RTT scene** (`[nhl-gpu] frame 0 swap:
draws=600 copies(resolves)=19`). The create-player model is built across ~19 intermediate
render-target textures with a `Resolve` between passes; a late composite samples them. A
**single offscreen RTV cannot represent that** — draws targeting different intermediate
surfaces all collapse into one RT (or are culled), so nothing coherent lands. Viewport
width is irrelevant when the *pass structure* isn't modeled.

So fork (a) "render draws into flat RTs" for 3D means: **own the multi-pass RTT chain in
flat host RTs** — allocate each intermediate render target / resolve-dest texture at its
**logical** size (from the H-1 D3D9 resource graph + the guest viewport/resolve regs),
bind the right flat RT per pass, and honor each PM4 `Resolve` as a host copy between flat
RTs. The fold never forms because every RT is flat at logical size; the EDRAM tile model
(the only thing that produces the 640-pitch fold) is never used.

### Why we can't just make the SDK RT-cache render flat
`D3D12RenderTargetCache` is the SDK's EDRAM emulator and is **`final`** (cannot be
subclassed) and sizes its host RTs from the guest `surface_pitch` (the 640 that folds).
We can drive it (the beta EDRAM path does) but cannot make it allocate flat logical RTs —
that is the closed-SDK wall the pivot investigation documented. Hence fork (a)'s flat-RT
work is **our own** pass/RT orchestration over the reused decode+translate+primitive
machinery, not a tweak to the SDK cache.

## Validation harness caveat (important for next session)

Trace-replay of 3D scenes is a **confounded** validation target for "fold gone":
- multi-pass RTT (above) means the single offscreen path can't render them at all, and
- captured 3D traces have **texture/data gaps** (`[[gameplay-trace-missing-textures]]`) —
  some textures were never written by the `.xtr`, so even a correct renderer shows black.

So validating the flat-RT chain needs either a **live** takeover (the game actually
producing the 3D frame) or a **self-contained** capture (`NHL_CAPTURE_FULL`) of a 3D
scene. Establish that harness first, or the "after" image is unreadable.

## Status

- ✅ Fork **(a)** decided, with evidence (we own the decode/translate/flat-RTV machinery).
- ✅ Real work precisely characterized: a **flat-RT multi-pass RTT chain** sized from the
  H-1 logical graph — needed for *either* fork, so the decision de-risks it.
- ✅ Easy "viewport-width" fix ruled out live (offscreen path is single-pass; 3D is 19-pass).
- ❌ A fold-free 3D render is **not** produced this session — it is the multi-session build
  above, and the trace-replay harness must first be made readable for 3D (live or
  full-capture).

## Validation harness — ESTABLISHED (oracle readable; "harness first" step)

The readable oracle is in place and proves the fold is **not** in the scene:

- **Oracle = base/SDK path, whole-stream replay.** Replaying the *entire* scene_02 stream
  through the base path (textures warm across frames) renders the create-player EQUIPMENT
  screen with the **3D player model correctly positioned** — upright, center-right, **no
  fold / no wrap / no mis-projection** (`replay_scene02_full.png`, fully textured Bruins
  jersey). A single-frame base replay (`replay_frame.png`) shows the same geometry as a
  black **silhouette** (early frame, textures not yet uploaded) — still correctly placed.
- **Conclusion:** the faithful SDK path renders this 3D frame correctly. The empty beta
  output is therefore a **beta-backend** gap (no multi-pass RTT into flat RTs), not a
  scene/fold property — exactly what fork (a)'s flat-RTT build must close.
- **Harness recipe:**
  - *Oracle:* `NHL_REPLAY_XTR=scene_02\…_stream.xtr` (no `NHL_BACKEND`) → whole-stream
    replay → `replay_scene02_full.png` (warm, textured, correctly placed). Reference image.
  - *Test (to fill once the build lands):* same trace through `NHL_BACKEND=beta
    NHL_BETA_TAKEOVER=1` with the flat-RTT path → compare the player's placement/shape to
    the oracle. "Fold gone" = player lands like the oracle (not folded/wrapped/empty).
  - Note: the beta side must replay the **whole stream** (warm textures) and capture a
    **late** frame to be comparable — today's single-frame takeover would be cold even if
    geometry were correct. Wiring beta's streaming-replay + late-frame capture is the
    first build sub-step.
- Composite `beta_owned_draw.png`/`replay_frame.png` over **black** before judging
  (RTV-alpha artifact, `[[beta-png-alpha-artifact]]`).

## Recommended next step (harness done → build)

1. ✅ Readable 3D validation harness established (above): oracle = base whole-stream replay
   of scene_02 (`replay_scene02_full.png`), player correctly placed.
2. Wire beta's **streaming replay + late-frame capture** so its output is comparable to the
   oracle (whole-stream warm textures, not the cold single-frame takeover).
3. Build the **flat multi-pass RTT** orchestration in the beta CP: per guest render pass,
   allocate/bind a flat host RT at the logical size (H-1 graph + viewport/resolve regs);
   on each PM4 `Resolve`, copy flat-RT → flat resolve-dest texture; composite samples the
   flat textures. Validate the player lands like the oracle (fold absent).

## Build phase — progress (this session)

**Sub-step 1 DONE — fast readable harness.** The beta takeover ran a ~4 s/frame
device-removal poll on *every* frame, so fast-forwarding a streaming trace to a late
capture frame took ~12 min (reached only frame 7 in 40 s). Gated it to the capture frame
(`beta_takeover_rendered_>0 && !beta_capture_done_`); streaming replay now reaches frame
168 in seconds (commit "make beta streaming replay fast"). With it, replaying scene_02 to
`NHL_BETA_CAPTURE_FRAME=150` through the EDRAM path renders the **EQUIPMENT screen UI
correctly** — **but the 3D player model is missing** (right side empty; oracle has the
Bruins player). This is the precise, fast, readable test for the flat-RTT work.

**Diagnosis of the missing player (EDRAM_DIAG, capture frame = 581 owned draws):**

| draws | surf_pitch | extent | reading |
|---|---|---|---|
| 199 | 640 | 640×360 | half-res 3D player passes |
| **27** | **640** | **1280×720** | **wide-into-narrow fold draws** (1280 content into a 640-pitch surface) |
| 199 | 1280 | 1280×720 | full-res UI / composite |
| 61/27/26… | 320/480/800 | … | intermediate RTT surfaces (thumbnails, shadow/reflection) |

Resolves fire and report `ok=true` (player surfaces resolve to guest textures, e.g.
`surf_pitch=640 -> copy_dest=(640×360)`). So the player **is** drawn and resolved, yet
never reaches the composite. The gap is the **fold** on the 27 wide draws (the SDK RT
cache stores 1280-wide into a 640-pitch host RT) **and/or** a resolve→sample address
mismatch the composite reads from — the closed-SDK EDRAM-sequencing problem.

**Implementation target (next):** intercept the 27 `extent=1280 / pitch=640` wide draws
(and the player's 640×360 passes) and render them into **our own flat host RTs at the
logical extent**; honor their `Resolve` as a flat host copy into the texture the composite
samples (matched by guest address). Validate the player appears like the oracle. This is
the focused flat-RTT work; it is deep (prior sessions' "green wall" / EDRAM-sequencing
divergence live here), so it is scoped as its own effort, not a one-shot change.

## Reproduce (the diagnostic runs above)

```
# offscreen flat-RTV (empty for the 19-resolve scene_02):
NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_REPLAY_XTR=...\gpu_trace\scene_02\454109EC_stream.xtr \
  nhllegacy.exe --game_data_root "H:\…\NHL Legacy - Vanilla"   # -> beta_owned_draw.png (alpha 0)
# add NHL_BETA_VP_FULLRT=1 / NHL_BETA_DEPTH=1 / NHL_BETA_EDRAM=1 to reproduce the table.
# beta_owned_draw.png keeps RTV alpha — composite over BLACK before judging ([[beta-png-alpha-artifact]]).
```
