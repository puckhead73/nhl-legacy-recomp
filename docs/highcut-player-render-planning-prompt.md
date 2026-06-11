# Planning session — make the beta backend render 3D geometry (the create-player model)

> Paste everything below the line as the opening prompt of a new **planning** session. It
> is self-contained. The deliverable is a **plan/strategy**, not a finished fix: a ranked,
> step-by-step investigation+implementation plan with the diagnostic and parity oracle for
> each step. Do not write the fix in this session — design it.

---

You are planning the next campaign for `e:\Repositories\nhl-legacy-recomp` — a static
recompilation (PPC→x86, BSD-3 "rexglue"/Xenia-derived runtime) of Xbox 360 **NHL Legacy**,
where we are building a **hybrid high-cut renderer**. The goal of THIS planning session:
**produce a plan to make the "beta" owned-D3D12 backend render 3D geometry faithfully**, so
the create-player **player model appears** (and, by extension, 3D gameplay scenes render).
Everything else around the player already works; the player geometry is the wall.

## Where we are (precise, all proven this/last session — do not re-derive)

The hybrid splits by interception level. Resources/present are hooked at the out-of-line
D3D9 level (H-1 logical resource graph, H-2 plume present, both DONE + committed). The
per-draw path is **inlined → PM4-only**, so draws are decoded by our owned command
processor `NhlD3D12CommandProcessor` (subclass of the SDK `D3D12CommandProcessor`) in
[renderer/core/nhl_command_processor.cpp](../renderer/core/nhl_command_processor.cpp). It
runs under `NHL_BACKEND=beta NHL_BETA_TAKEOVER=1` and reconstructs each draw from public
SDK pieces (`RenderBetaOwnedDraw`).

**The fold is solved.** "H-3 route (a)" (`NHL_BETA_FLAT`, gated) was built and validated: on
each guest resolve we copy our flat offscreen RT into a host texture keyed by
`draw_util::GetResolveInfo().copy_dest_base`, and substitute that host texture when a later
draw samples the address — so composites read our flat host RTs instead of a guest-RAM
untile of the resolved surface (which was misplaced = the "fold"). Full history +
diagnostics: [docs/highcut-h3-flat-rt.md](highcut-h3-flat-rt.md). Result on scene_02
frame 150: the mispositioned/wrapped player is GONE, the **2D UI renders perfectly**, but
the **player model does not appear**.

**The remaining problem is precisely localized (the planning target):**
- **2D draws render at parity** (menu = 45 dB vs the oracle; the create-player UI is
  pixel-clean). 2D draws have the viewport transform *disabled* (`PA_CL_VTE_CNTL` without
  `vport_x/y_scale_ena`, e.g. `vte=0x300`).
- **3D draws render to NOTHING.** The player's geometry draws (`vte=0x43F`, viewport
  transform enabled, depth-tested) produce no visible output in beta's flat scratch RT.
- Proven by toggles: `NHL_BETA_FLAT_FAKE` (bind solid red for the resolved player texture)
  turns the **whole background red** → the substitution fires and the player composite is a
  full-screen quad sampling the resolved texture; the captured texture is simply **empty**.
  `NHL_BETA_FLAT_KEEPFIRST` (don't overwrite the dest on its 2nd resolve) → still empty, so
  it is not a double-resolve overwrite.
- The player **exists in the trace's guest RAM** (the OLD guest-RAM untile path rendered it,
  mispositioned), so this is **beta's render of 3D geometry, not missing data**.

⇒ **The wall is Tier-1 3D-geometry rendering parity:** beta's reconstruction of the per-3D-
draw state — the WVP/float-constant upload, vertex fetch/format, depth setup, clip/guard-
band/perspective-divide flags, and texture/sampler bindings — does not produce visible
geometry for viewport-transformed draws, even though the same machinery renders 2D draws
correctly. This is the "reproduce the CP's binding & constant computation exactly" risk
called out in [docs/tier1-backend-build-order.md](tier1-backend-build-order.md) §4.1 / Phase
4–5, now isolated to the 3D-draw subset.

## What the plan must figure out (rank these; the plan picks the order + the oracle per step)

The crux: **why do beta's 3D (viewport-transform) draws rasterize nothing, while 2D draws are
pixel-perfect?** Candidate failure modes to design diagnostics around:
1. **WVP / float constants.** A 3D VS multiplies fetched positions by a model-view-projection
   matrix read from float constants (`0x4000+`). A wrong/zero/garbage upload collapses verts
   to a point or pushes them off-screen/behind the near plane. (`NHL_BETA_BIND_DIAG` already
   dumps the VS's non-zero float4s — but for the 3D player draws specifically, not the 2D
   composite.) *Likely #1 suspect.*
2. **Vertex fetch/format for 3D.** The player uses different vertex formats than the 2D quad
   (`NHL_BETA_VTX_DUMP` decoded the composite quad cleanly; do the same for the player's
   geometry draws — positions sane and in model space?).
3. **Perspective-divide / clip flags** (`kSysFlag_XYDividedByW/ZDividedByW/WNotReciprocal`
   from `PA_CL_VTE_CNTL`; user clip planes). Wrong here = the "¼-width"/off-screen symptom.
4. **Depth.** 3D draws are z-tested. Per-pass depth clear was added, but the depth target
   setup / `zfunc` / near-far / reversed-Z could still cull everything. Isolate with
   `NHL_BETA_DEPTH_FORCE_ALWAYS` / `NHL_BETA_DEPTH_FORCE_NEVER`.
5. **The player is a MULTI-LEVEL RTT chain.** The player model may render into intermediate
   resolves first, then composite into `0x1AF09000`. The plan must first establish **does
   the player's RAW geometry pass render at all** (find those draws by bisection, dump the
   scratch right after them) — separating "geometry renders to nothing" from "the chain
   loses it."
6. **Texture/sampler bindings for 3D.** The player skin/jersey may itself be a resolved-RTT
   texture (the substitution should cover it) or a guest texture; a wrong bind → black model
   (but black geometry should still occlude/clear — distinguish "renders black" from "renders
   nothing").

## Tooling the plan should lean on (all exist, no new harness needed)

- **Validation oracle:** base-path whole-stream replay → `replay_scene02_full.png` (the
  create-player EQUIPMENT screen with the correctly-rendered player). The beta target is
  scene_02 frame 150.
- **Fast harness:** the per-frame device-removal poll is gated to the capture frame, so a
  streaming replay reaches frame 150 in seconds.
- **Per-draw diagnostics (env-gated, in `RenderBetaOwnedDraw`):** `NHL_BETA_VTX_DUMP=<idx>`
  (vertex attrs/UV/sampler with real offsets), `NHL_BETA_BIND_DIAG` (bindings, textures,
  vfetch, WVP float4s), `NHL_BETA_DEPTH_DIAG` (viewport/VPORT/ndc), `NHL_BETA_EDRAM_DIAG`
  (per-draw surf_pitch/extent/vte/z), `NHL_BETA_MAX_DRAW=N` (bisect which draws paint what),
  `NHL_BETA_DEPTH_FORCE_ALWAYS/NEVER`, `NHL_BETA_FLAT_FAKE/KEEPFIRST/CLEAR/DIAG`.
- **A capture isolation move the plan should specify early:** use `NHL_BETA_MAX_DRAW` +
  scratch dump to find the player's raw geometry draws and confirm whether they rasterize
  anything into the flat RT *before* worrying about the resolve/composite chain.
- RenderDoc is **impractical** here (≈158× slowdown; never reaches frame 150) — plan
  in-engine diagnostics, as above.

## Reproduce (the current state)

```
_build_beta.bat
# route (a), player still empty (UI perfect):
NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_BETA_DEPTH=1 NHL_BETA_FLAT=1 NHL_BETA_CAPTURE_FRAME=150 \
  NHL_REPLAY_XTR=out\build\win-amd64-relwithdebinfo\gpu_trace\scene_02\454109EC_stream.xtr \
  nhllegacy.exe --game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"
# -> beta_owned_draw.png ; composite over BLACK before judging (RTV-alpha artifact).
# add NHL_BETA_FLAT_FAKE=1 (background goes red) to confirm substitution+composite.
```

## Read first

- [docs/highcut-h3-flat-rt.md](highcut-h3-flat-rt.md) — the full H-3 arc + every diagnostic
  already run (don't repeat them).
- [docs/tier1-backend-build-order.md](tier1-backend-build-order.md) — §4 (binding & system-
  constant parity is risk #1) and Phase 4–5 (the owned-draw bring-up spine + parity oracles).
- `renderer/core/nhl_command_processor.cpp` — `RenderBetaOwnedDraw` (the 3D draw path:
  viewport, system constants `UpdateSystemConstantValues`-subset, bindings, the
  `DrawIndexedInstanced`), `BetaFlatResolve`, and the `write_tex_table` substitution.
- Memories: `[[highcut-h3-flat-rt]]`, `[[tier1-backend-architecture]]`,
  `[[beta-scene04-projection]]`, `[[rov-green-player-is-fold-color]]`,
  `[[gameplay-trace-missing-textures]]`.

## Deliverable of this planning session

A written plan (`docs/...`) that:
1. **Ranks the hypotheses** above with rationale, and picks an **investigation order** that
   front-loads the cheapest decisive test (recommended start: bisect to the player's raw
   geometry draws, `NHL_BETA_VTX_DUMP` + `NHL_BETA_BIND_DIAG` them, and dump the scratch to
   prove whether geometry rasterizes at all).
2. For each step: the **exact diagnostic** to run, the **expected signal** that confirms/kills
   the hypothesis, and the **parity oracle** (vs `replay_scene02_full.png` and/or the
   `NHL_BETA_*_DIAG` numbers vs the base CP's `ReadRegisterValue`).
3. Identifies the **likely fix locations** in `RenderBetaOwnedDraw` (constant upload, vertex
   fetch, depth/clip flags, bindings) per hypothesis, and the order to attempt fixes.
4. States the **done criterion** (the create-player model appears in `beta_owned_draw.png`
   matching the oracle within the existing tolerance) and a **generalization check** (scene_04
   gameplay — note `[[gameplay-trace-missing-textures]]`: use a full/self-contained capture so
   missing-texture gaps don't masquerade as render bugs).
5. Calls out scope/risk: this is the deep Tier-1 3D-parity work; estimate the steps and flag
   where it might need the device vtable / a self-contained 3D capture / multiple sessions.

Do **not** modify renderer code in this session — produce the plan.
