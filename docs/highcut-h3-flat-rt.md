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

## Demonstration: flat RT renders the player; EDRAM folds it away (the thesis, shown)

Three captures of the **same** scene_02 frame 150 (composite over black):

| capture | path | result |
|---|---|---|
| `replay_scene02_full.png` | base/SDK oracle | UI + player, correct (center-right, textured) |
| `h3_edram_f150_black.png` | beta **EDRAM** (folds) | UI correct, **player MISSING** (folded/sequenced away) |
| `h3_offscreen_f150_black.png` | beta **flat** offscreen RTV | UI + **player TEXTURED** (Ducks jersey), but mispositioned (far-right + left wrap) |

**This is the answer to "are we still hitting the fold?": no.** The EDRAM path (the low
cut we delete) renders the player away. The **flat** path renders the player **textured**
— the EDRAM fold-to-black does not happen on a flat RT. The high cut escapes the fold.

The flat path's residual is **positioning**, and it is NOT the fold — it is that the
offscreen path uses a **single** RT and piles all ~19 multi-pass surfaces into it, so the
player's intermediate render + the composite don't line up. Confirmed: a viewport-only fix
(route the 27 wide `vte=0x43F` draws through a native-viewport flat branch) changed **0**
visible pixels — the visible player comes from the 640×360 passes + composite, which a
single RT can't separate. So the remaining work is genuinely the **multi-pass flat RT
manager**, not a fold fix:

1. Map each guest color surface (`RB_COLOR_INFO.color_base`) → its own **flat host RT** at
   logical size (viewport extent), instead of one shared offscreen RT.
2. Bind the per-surface flat RT per draw; render with the native guest viewport.
3. On each PM4 `Resolve`, copy the source flat RT → a flat host **texture**; map the guest
   resolve-dest address → that host texture.
4. When a draw samples a resolved guest address, bind the mapped host texture (skip the
   tiled guest-memory round-trip).
5. Present/capture the frontbuffer surface's flat RT.

None of this models EDRAM tiles, so the fold cannot form — it is the genuine high-cut
renderer, scoped as its own (multi-session) build.

## REFRAME: the missing player is a mispositioned composite blit, not the fold or multi-pass

`NHL_BETA_MAX_DRAW` bisection of scene_02 frame 150 (flat offscreen path) localizes the
visible content precisely:

| MAX_DRAW | result |
|---|---|
| 250 | black (early passes render then get cleared — they're render-to-texture) |
| 430 | solid blue (the background fill) |
| 470 | **full player (right) + left sliver**, UI header + partial equipment list |
| 560 / 600 | full UI + same mispositioned player |

So the visible frame is painted by the **late** draws (~430→600): blue background → UI →
**a single composite blit of the player at draws ~430–470**, sampling the player's
already-resolved texture (present in guest RAM from the trace; the texture cache uploads
it — that's why the player is fully textured). The early render-to-texture passes are
cleared away, as expected.

**This reframes the remaining work entirely.** The player is *not* lost to the EDRAM fold
(it renders, textured) and the fix is *not* a multi-pass RT manager. It is a **single
composite draw** whose placement is wrong: the player lands shifted right and wraps at
x=1280 (full player at x≈960–1280 + a sliver wrapping to x≈0–80), vs the oracle's
center-right placement. That signature (right-shift + wrap-at-RT-width) points at the
composite blit's viewport/UV mapping or a WRAP sampler address mode on a quad whose UVs
run past 1.0 — a localized, single-draw fix.

**Next step (focused):** inspect the ~430–470 composite draw's vertices/UVs + sampler
(RenderDoc on the takeover frame, or dump the draw's VB/fetch-constants) and correct its
placement so the player lands like the oracle. No multi-pass machinery needed for this
scene — the texture is already there; only the blit is misplaced.

## Isolated to ONE draw: the player composite samples a resolved RTT texture beta untiles wrong

Drilling into the ~430–470 composite blit (`NHL_BETA_DEPTH_DIAG` + `NHL_BETA_BIND_DIAG`):

- **All composite draws 430–472 have a correct, standard viewport**: `VPORT(xs=640 xo=640
  ys=-360 yo=360)`, `ndc_scale=(1,1,1)`, full 1280×720. So the misplacement is **not** the
  viewport.
- **Draw #430 is the player composite**: it samples a **1280×720** texture at
  `0x1AF09000` (fmt=6, **tiled=1, pitch=40 → 1280px**, resident) and draws it full-screen.
  The texture is a correct, un-folded, 1280-wide player RTT — pitch == width, no 640 fold.
- **Ground-truth dump of `0x1AF09000`** (`NHL_DUMP_ADDRS`, base path) shows the player
  content **split to the left+right edges** under the approximate untile — the same
  far-right+wrap signature, in the texture's untiled layout.
- **Decisive:** the **base/SDK path renders this exact texture correctly** (player
  center-right, `replay_scene02_full.png`), while **beta renders it split**. Same texture,
  same fetch constant — so **beta's untile/sample of this resolved RTT texture diverges
  from the base**.

**Conclusion — the residual is texture/binding PARITY, not the fold.** The high cut already
escaped the fold (flat RT renders the player textured). What's left is the documented hard
core of the owned backend (build-order doc §4.1): "reproduce the CP's binding and
constant computation exactly." Specifically, beta's `D3D12TextureCache` untile / sampler /
UV handling for this **resolved-render-target texture** must match the base byte-for-byte
(the UI's ordinary tiled textures already match; the resolved RTT has a different
tiling/addressing the beta path doesn't yet reproduce). That is iterative renderer-parity
work (diff beta's untiled texture vs the base for `0x1AF09000`, find the addressing
divergence, fix), not a one-line change — and it generalizes to all 3D scenes (gameplay
included), so it is worth doing as a parity pass rather than per-scene.

## Route (a) BUILT — resolve→host-texture substitution works (fold-untile eliminated)

The high-cut multi-pass flat path (`NHL_BETA_FLAT`, route a) is implemented and the core
mechanism is validated:
- **`BetaFlatResolve()`** — on each guest resolve (kCopy), copy our flat offscreen RT (the
  just-rendered pass) into a host texture keyed by the resolve's **physical** dest address.
  The dest must come from `draw_util::GetResolveInfo().copy_dest_base` (NOT
  `RB_COPY_DEST_BASE<<12` — Xenos uses tiled resolve addressing); with that, the captured
  dests include `0x1AF09000`, exactly the texture the player composite samples.
- **SRV substitution** (`write_tex_table`) — when a draw samples a captured resolve dest,
  bind our host texture instead of the guest-RAM untile.
- **Flat 3D viewport** — 3D passes render with their native viewport into the flat RT.

**Result on scene_02 frame 150:** the **mispositioned far-right/wrapped player is GONE** and
the UI renders perfectly — the substitution replaced the bad resolved-RTT untile, proving
the mechanism. **Remaining:** the player's flat render into the scratch comes up empty
(center-right blank) — its multi-pass render needs work: with depth ON it's depth-culled
(single depth buffer accumulates across passes → needs per-pass depth); with depth OFF more
content renders but green-tinted (the known EDRAM green-additive artifact) and the player
still doesn't resolve. So route (a)'s substitution is done; the **flat multi-pass render of
the player (per-pass depth + green + the multi-level RTT chain)** is the next layer. Gated
by `NHL_BETA_FLAT`; default build unchanged.

**Per-pass depth (added):** `BetaFlatResolve` now clears the depth target at each resolve
(pass boundary) so each pass z-tests from the far plane (the single depth target otherwise
accumulates and culls later passes; color is NOT cleared — the composite scratch is what we
dump). Result: the green tint is gone and the UI stays perfect — but **the player is still
missing**, so it is not (only) depth. Key clue from `NHL_BETA_FLAT_DIAG`: **`0x1AF09000` is
resolved TWICE (resolves #13 and #16)**, so the capture is overwritten by the 2nd resolve,
whose scratch may not hold the player; the player is built through a **multi-level RTT
chain** (its geometry → intermediate resolves → composited into `0x1AF09000`). Next focused
step: dump the captured `0x1AF09000` host texture (and the intermediates) to see at which
chain level the player content is lost (double-resolve overwrite vs an empty geometry pass),
then fix that level (keep both resolves, or accumulate rather than overwrite).

**Localized further (decisive):**
- `NHL_BETA_FLAT_FAKE` (substitute solid RED for the resolved texture) turns the **whole
  background red** → the substitution fires and the player composite is a **full-screen quad**
  sampling `0x1AF09000`. So substitution + composite are correct; the captured texture is
  just **empty**.
- `NHL_BETA_FLAT_KEEPFIRST` (don't overwrite a dest on its 2nd resolve) → **still no player**,
  so BOTH captures of `0x1AF09000` are empty — not a double-resolve overwrite.
- ⇒ The player's actual **3D geometry doesn't render into ANY captured scratch.** Beta
  renders the player's geometry to nothing (even though the player exists in the trace's
  guest RAM — that's why the *old* untile path showed it, mispositioned). So the residual is
  **beta's 3D-geometry rendering parity** (the player's VS/PS reconstruction, vertex/texture/
  constant bindings for the 3D draws producing visible output) — the deep Tier-1 core, a
  **separate, larger problem from the fold**, which route (a) has already solved.

**Bottom line:** route (a) (the high-cut flat resolve→host-texture substitution) is built,
gated (`NHL_BETA_FLAT`), and **eliminates the fold-untile** (the mispositioned player is gone;
the UI is perfect). Making the player *appear* now depends on beta rendering 3D geometry
faithfully — Tier-1 3D parity — not on the fold. Diagnostic toggles: `NHL_BETA_FLAT_FAKE`,
`NHL_BETA_FLAT_KEEPFIRST`, `NHL_BETA_FLAT_CLEAR`, `NHL_BETA_FLAT_DIAG`.

## ROOT CAUSE CONFIRMED (in-engine diagnostic, no RenderDoc): the resolved-RTT untile

A new in-engine diagnostic (`NHL_BETA_VTX_DUMP=<draw>`, in `RenderBetaOwnedDraw`) decodes a
draw's vertex attributes with their **real** per-attribute offsets+formats (the generic
`pos@0` diag read garbage) plus the bound texture's clamp mode. For draw #430 (player
composite) it shows the blit is **provably perfect**:

| vertex | position (px) | UV |
|---|---|---|
| v0 | (0, 0)     | (0, 0) |
| v1 | (0, 720)   | (0, 1) |
| v2 | (1280,720) | (1, 1) |
| v3 | (1280, 0)  | (1, 0) |

…a full-screen quad, **UV [0,1]×[0,1] exactly**, **CLAMP** sampler (`clamp_x/y=2`, not wrap),
sampling `0x1AF09000`. A full-screen quad + [0,1] UVs + clamp reproduces the texture
**verbatim** — so the player rendering shifted+wrapped means **beta's reconstruction of the
texture `0x1AF09000` is itself shifted.** It is **not** the geometry, the UVs, or the
sampler.

**Root cause:** the texture is a **resolved render target**. The base path renders it
correctly because it keeps/binds the resolved *host* texture (no guest round-trip). Beta's
flat/offscreen path has no resolve, so it reads the resolved surface back from **guest RAM
and untiles it as a plain texture** — but a resolved surface's guest-RAM layout does not
match a plain-texture untile per its fetch constant, so it comes out shifted.

**Fix (two routes, both high-cut-aligned):**
- (a) **Multi-pass flat:** render the player pass into a flat host RT, resolve it (host copy)
  to a host texture, and bind THAT for the composite — no guest-RAM untile. This is the
  flat multi-pass path; it makes the composite correct because it samples a real host RT.
- (b) **Fix the resolved-surface untile:** reconstruct the resolved tiling correctly when
  reading `0x1AF09000` from guest RAM (match how the resolve stored it, not the plain
  texture untile). Narrower, but tricky.

Route (a) is the cleaner high-cut answer and generalizes to all 3D (gameplay). Both are
real renderer work, but the unknown is now **gone** — it's the resolved-RTT texture, full
stop, proven by the in-engine vertex/UV/sampler decode.

## (superseded) Earlier note: log-based debugging exhausted → use the frame debugger

The misplacement is fully isolated to draw **#430** (player composite): correct full-frame
viewport, samples a correct 1280×720 player RTT texture (`0x1AF09000`), yet renders the
player shifted+wrapped while the base renders it correctly. The remaining unknown is which
of these diverges from the base for this one draw:
- the **texture untile/addressing** for a *resolved render-target* texture (in the
  offscreen/flat path the RT cache isn't tracking `0x1AF09000` as a resolved surface, so
  the texture cache may untile it as a plain 2D texture), or
- the composite quad's **UVs / sampler address mode** (a U-offset past 1.0 with WRAP
  reproduces the right-shift + left-sliver exactly).

**Log-based debugging has hit its limit:** the generic per-draw vertex diag assumes
`pos@0 float3` and reads ~0 for these 2D composite quads (different vertex/UV layout), so
it can't show the quad's UVs. The efficient next step is the **frame debugger**:
`NHL_BETA_RENDERDOC=1` brackets the takeover frame (StartFrameCapture→EndFrameCapture); open
the `.rdc` and inspect draw #430's **bound SRV (is `0x1AF09000` untiled correctly?), its
UVs, and the sampler address mode** vs the base. That directly shows the divergence in one
look, where logs can't. Then fix in the untile path (track resolved-surface ownership in
the flat path) or the sampler/UV. The fix generalizes to all 3D scenes (gameplay included).

This is the documented deep Tier-1 parity core (build-order §4.1) and is multi-session
renderer work — isolated precisely here, to resume with the frame debugger.

## 2026-06-11 — diagnostic campaign executed: hypotheses ranked, fix isolated to RTT-chain + color

Ran the `docs/h3-3d-geometry-parity-plan.md` investigation against a live frame-100 capture
(the `CAPTURE_FRAME=150` in older notes is STALE — this `454109EC_stream.xtr` is only **109
frames** (0–108); frame 108 is a black transition. Capture a late valid frame, e.g.
**`NHL_BETA_CAPTURE_FRAME=100`**, which renders the EQUIPMENT UI correctly with the **player
missing** — the wall reproduced).

**New diagnostic added (gated, default-off):** `NHL_BETA_FLAT_DUMP=1` writes every guest
resolve's captured host texture to `flatresolve_<seq>_<dest>.png` at capture finalize
(`BetaFlatResolve` records a readback copy; `WriteBetaFlatResolveDumps` writes the PNGs). This
is the decisive per-resolve "what landed where" view the plan called for.

**Hypotheses ruled out (with evidence):**
- **H1 WVP / float-constant upload — KILLED.** `NHL_BETA_BIND_DIAG` on the player draws
  (e.g. owned-draw #27, vte=0x43F, ext 640×360): **`dyn=false`** (static addressing, so the
  sparse `pack_floats` is the correct CBV layout), the dumped float4s are a **sane view matrix
  (c[0..3]) + projection (c[4..7]) + combined WVP (c[12..15])**, and the vertex fetch
  (fc=95, 0x19249000, stride 32) reads real guest data (nz=806/1024). Geometry *setup* is
  correct. (The prime suspect from the plan does not apply to these draws.)
- **H4 depth / reversed-Z — KILLED.** `NHL_BETA_DEPTH_FORCE_ALWAYS` changes nothing; depth
  clears to `1.0` (correct far plane for the player's zfunc=3 LEQUAL).

**What the 19 resolve dumps show (the real residual):**
- 10 of 19 resolve dests are **fully empty** (uniform 0,0,0).
- The player's content-bearing surface **`0x1A6E9000`** (resolved 4×) DOES contain the player,
  localized to the **top-left 640×360 viewport** — but with **R and G channels pinned at 255**
  (per-channel min R=255, G=255; only blue varies). The player rasterizes; its **color is
  saturated/wrong**, not absent.
- The surface the player composite actually **samples — `0x1AF09000`** (resolves #13 empty,
  #16) — comes out **perfectly uniform `(0,127,15)`**, the documented **depth-as-color
  green-fold** value ([[rov-green-player-is-fold-color]], [[beta-depth-buffer-status]]). So the
  composite reads a **different dest than where the player landed**, and that dest holds the
  depth-green fold, not the player.

**Conclusion (evidence-backed):** the residual is NOT geometry/WVP/depth. It is the
**multi-level RTT chain + color-target correctness** in the single shared offscreen RT:
(1) the player's color writes saturate R/G on its real surface (`0x1A6E9000`), and (2) the
composite's source (`0x1AF09000`) resolves to the depth-green fold instead of the player
surface — a resolve-dest/surface mapping + color/depth-target confusion. This is the
[[beta-scene04-projection]] / green-fold core: beta mis-drives the RT cache for the player's
multi-pass surfaces. **Fix = the per-surface flat-RT manager** (each guest `RB_COLOR_INFO.
color_base` → its own flat host RT; resolve copies the *correct source surface region*, not
the whole shared RT; separate color vs depth), plus running down the R/G-saturation on the
player color write. Multi-session, as the plan flagged (worst-case branch). Diagnostic toggles
to resume: `NHL_BETA_FLAT_DUMP`, `NHL_BETA_BIND_DIAG`, `NHL_BETA_EDRAM_DIAG`,
`NHL_BETA_DEPTH_FORCE_ALWAYS`.

## Reproduce (the diagnostic runs above)

```
# Per-resolve host-texture dump at a VALID late frame (109-frame trace; 150 is stale):
NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_BETA_DEPTH=1 NHL_BETA_FLAT=1 NHL_BETA_FLAT_DUMP=1 \
  NHL_BETA_CAPTURE_FRAME=100 NHL_REPLAY_XTR=...\gpu_trace\scene_02\454109EC_stream.xtr \
  nhllegacy.exe --game_data_root "H:\…\NHL Legacy - Vanilla"   # -> flatresolve_*.png + beta_owned_draw.png
# Judge flatresolve_*.png by FORCING ALPHA OPAQUE (they keep RTV alpha); 0x1AF09000 is the
# player composite source, 0x1A6E9000 is the player's real (R/G-saturated) surface.

# offscreen flat-RTV (empty for the 19-resolve scene_02):
NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_REPLAY_XTR=...\gpu_trace\scene_02\454109EC_stream.xtr \
  nhllegacy.exe --game_data_root "H:\…\NHL Legacy - Vanilla"   # -> beta_owned_draw.png (alpha 0)
# add NHL_BETA_VP_FULLRT=1 / NHL_BETA_DEPTH=1 / NHL_BETA_EDRAM=1 to reproduce the table.
# beta_owned_draw.png keeps RTV alpha — composite over BLACK before judging ([[beta-png-alpha-artifact]]).
```

## 2026-06-11 (cont.) — SOLVED: the player renders at parity (three root causes, all fixed)

The "player missing" wall is **down**. `NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_BETA_DEPTH=1
NHL_BETA_FLAT=1 NHL_BETA_CAPTURE_FRAME=100` on scene_02 now produces the oracle frame
(equipment UI + blue stage + black player silhouette, ~40 dB vs the cross-resolution
oracle). Three independent defects stacked on this one scene; each was isolated with an
in-engine experiment and fixed in `renderer/core/nhl_command_processor.cpp`:

1. **Pixel-shader float constants read from the WRONG BANK (the big one — affects every
   scene).** Xenos splits the 512-entry float-constant file per stage: VS = c0..255 at reg
   `0x4000+`, **PS = file entries 256..511 at reg `0x4400+`** (ucode "c0" in a PS is file
   entry 256; Xenia reads the pixel pack from `SHADER_CONSTANT_256_X`). `pack_floats` fed
   BOTH stages from `0x4000` — every PS got the VERTEX bank. The create-player post-grade
   PS (`out = lerp(player + grain*c1, c2.rgb, c2.w)`, grain remap `grain*c255.y + c255.x`)
   then saw a view-matrix row as its green tint and 0.5 as its remap → the **uniform
   (0,127,15) green** that the on-screen player quads sampled (this is also the long-
   standing "green fold color" value — it was never EDRAM fold, it was the PS reading VS
   constants). Fix: `pack_floats(sh, pixel_stage)` reads PS floats from `0x4400`.
   A/B toggle: `NHL_BETA_PS_BANK_OLD=1`. scene_03 f30 A/B shows the same content minus a
   gray wash (improvement, no regression; its black quads pre-date this and are the known
   trace-data texture gap).

2. **The skinning bone-matrix VS texture loads as ZERO through the SDK texture cache (beta
   path only).** The player VS is texture-skinned: it fetches 6 texels from VS texture
   **tf16** (384×160 linear `k_32_32_32_32_FLOAT` endian=2 at `0x1C7AE000` — written
   mid-frame by the trace between draws #26/#27, one write per even frame, full 0xF0000
   arena). Binding resolves (host_swizzle 0x688 ✓), guest RAM + our shmem buffer hold the
   data at draw time (probes), no debug-layer errors — yet the sampled content is zero, so
   the skin matrix = 0 → every vertex collapses → **zero pixels rasterized** (proven by a
   purple-clear + SKIP_RANGE experiment, then by substituting a CPU-built SRV which made
   the player appear). The base path loads the same texture fine, so it's a beta-usage
   interaction inside the prebuilt SDK we cannot see into. Fix (ours): **VS-texture
   CPU-upload path** — any VERTEX-stage binding of a linear `k_32_32_32_32_FLOAT` 2D
   texture binds a CPU-built `R32G32B32A32_FLOAT` Texture2DArray filled from guest RAM
   (k8in32 swap), re-uploaded when a trace write covers its range (`EnsureBetaVsTexSub`,
   map keyed by guest base). Default ON; `NHL_BETA_NO_VSTEX_CPU=1` disables. The blue
   STAGE backdrop also came back with this (its RTT pass also skins via a 384×64 palette
   at `0x1C6EE000`).

3. **Bool/loop constant upload was 16 dwords; the block is 40** (8 bool + 32 loop;
   the translator's b1 cbuffer is declared 160 B). OOB CBV reads return 0 → any shader
   driving a loop from a high loop-constant runs 0 iterations. Not the killer for this
   scene (the player VS has no loops) but a real latent bug — fixed to `40 * 4`.

Plus one flat-model correction: **`BetaFlatResolve` no longer snapshots the color RT for
`is_depth=1` resolves** (a depth resolve to a dest that also receives color would
overwrite the good color capture with depth-as-color; the dest now keeps its latest COLOR
capture, and the depth resolve still acts as a pass boundary for the per-pass depth
clear). Restore old behavior: `NHL_BETA_FLAT_DEPTH_COPY=1`.

**Corrections to the earlier 2026-06-11 section** (analysis made with the pre-fix
diagnostics): the "R/G pinned at 255" and "composite reads the wrong dest" conclusions
were artifacts of the PS-bank bug (wrong PS constants saturate/tint everything); the
resolve-dest mapping was actually CORRECT all along — capture#8 (0x1AF09000) holds the
black player + alpha mask, the composite samples it, grades it (badly, bug 1), and the
later green capture (#10) was the graded output, not depth-fold. Also `fmt=38` is
`k_32_32_32_32_FLOAT` (16 B/texel), so the bone data rows 3–5.8 were exactly where the
draws sample (earlier "rows 6–11 vs sampled 3.5–5.5" used the wrong texel size). No
per-surface flat-RT manager was needed — the snapshot-at-resolve model held up.

**New/changed diagnostics:** `NHL_BETA_UCODE_DRAW=<idx>` (choose which textured draw's
ucode lands in `beta_textured_ucode.txt`), VS-texture identity in `NHL_BETA_BIND_DIAG`
(`VS-tex#` lines: fc, base, fmt, host_swizzle, raw fc dwords, full-extent guest-RAM scan),
`NHL_BETA_GPUDUMP` now also works in the flat/offscreen path (recorded at finalize),
`NHL_LOG_LEVEL=debug|trace` (SDK logger levels at runtime), `WATCH` logs frame + row
fingerprints, shmem buffer named `beta_shared_memory` for debug-layer messages.

**Status:** scene_02 f100 at oracle parity. Next: re-run scene_04/gameplay with the PS-bank
fix (suspect it cures more than this scene); textured (non-silhouette) player needs a live
F9 capture with warm textures (trace-data gap, capture-side).
