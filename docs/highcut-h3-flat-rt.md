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

## Recommended next step

1. Stand up a **readable 3D validation harness**: a live beta takeover of a 3D scene, or a
   `NHL_CAPTURE_FULL` self-contained 3D capture, so output is judgeable.
2. Build the **flat multi-pass RTT** orchestration in the beta CP: per guest render pass,
   allocate/bind a flat host RT at the logical size (H-1 graph + viewport/resolve regs);
   on each PM4 `Resolve`, copy flat-RT → flat resolve-dest texture; composite samples the
   flat textures. Validate the fold is absent vs the EDRAM path's folded output.

## Reproduce (the diagnostic runs above)

```
# offscreen flat-RTV (empty for the 19-resolve scene_02):
NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_REPLAY_XTR=...\gpu_trace\scene_02\454109EC_stream.xtr \
  nhllegacy.exe --game_data_root "H:\…\NHL Legacy - Vanilla"   # -> beta_owned_draw.png (alpha 0)
# add NHL_BETA_VP_FULLRT=1 / NHL_BETA_DEPTH=1 / NHL_BETA_EDRAM=1 to reproduce the table.
# beta_owned_draw.png keeps RTV alpha — composite over BLACK before judging ([[beta-png-alpha-artifact]]).
```
