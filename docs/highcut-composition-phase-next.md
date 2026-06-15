# High-cut Composition Phase — Resolve = host copy (real shadows / reflections / RTT / HUD)

> **Mission:** complete the **multi-surface composition** (the deferred C-5d.3 "guest Resolve = host
> copy" piece). Right now the high-cut replay renders ONE guest surface (the broadcast view) to the
> swapchain and isolates every other surface into its own offscreen RT — but it does **not composite
> them back**. The resolved surfaces (shadow maps, reflection cubes, render-to-texture, HUD/scoreboard,
> picture-in-picture) are the game's **lighting + UI inputs**; until they're bound where later draws
> sample them, the main pass reads stubs → the whole deferred-lighting bucket. Wire them and the
> equipment lights correctly, the goalie's glove returns, the HUD appears.
>
> Prior context: `docs/highcut-c-plume-renderer-plan.md` (roadmap; this is C-5d.3 + the lighting-input
> completeness it unlocks), `docs/highcut-c5g-jersey-numbers-next.md` (the toolchain). Memory:
> `[[highcut-c5h-surface-selection]]`, `[[highcut-c5g-jersey-number-residency]]`, `[[highcut-c-plume-renderer]]`,
> `[[gameplay-trace-missing-textures]]`.

---

## 1. What's DONE — the DIRECT render path is complete (do not redo)

The single-surface broadcast view renders correctly. Everything feeding a draw **directly** is right:
- **Geometry/skinning/strips/cull/depth/stencil** — players, rink, net, boards all render (C-5c/e).
- **Camera / surface selection** — auto-picks the full-res (largest-viewport) main view, even in
  instant replay's 9-surface frames (`[[highcut-c5h-surface-selection]]`).
- **All texture FORMATS untiled** — DXT1/2_3/4_5 (BC1/2/3), k_DXN (BC5 normals), k_8_8_8_8, k_8,
  k_32_32_32_32_FLOAT (bone palette), and **k_16 (R16 data/mask) as of C-5h**. The stub census
  (`tools/_stub_census.py`) shows ZERO unsupported-format magenta on the gameplay path.
- **Texel values correct** — the exp_adjust fix (numbers/names render: "BACKES 42"), per-sampler
  filter/clamp, swizzle, color_write_mask.

So this phase is NOT about the direct path. It's about the SECOND-ORDER surfaces.

## 2. What's STILL stubbed (this phase's whole target)

`_stub_census.py` on any gameplay capture shows exactly two things left, both deferred to HERE:

| Stub | Count | What it really is |
|---|---|---|
| **cube reflections** (BC3-bound-as-cube) | ~146 | env reflection cubes — we emit a NEUTRAL dark 6-face stub (`untileBindings` cube branch). The game uses the reflection AND **the cube sample ALPHA as a material factor** (per the sibling NHL12 decomp doc). |
| **depth/shadow maps** (`k_24_8`) | ~40 | resolved DEPTH surfaces the main pass samples for self-shadowing. Stubbed NEUTRAL-white (far/no-occlusion) as of C-5h — benign but not real. |

Plus the secondary COLOR surfaces (reflection RTT, HUD/scoreboard, picture-in-picture) that currently
render into isolated offscreen RTs and are **never composited onto the swapchain**.

## 3. The architecture you're completing (already 80% built)

- **Surface buckets** (`plume_present.cpp` `LoadC5Frames`, ~line 1133): every draw is keyed by its
  guest surface `(depth_base, pitch, msaa)`. The PRIMARY (largest-viewport) renders to the swapchain;
  every other surface renders into its **own offscreen color+depth+stencil RT** (`c.c5surfaces`,
  cached per `SurfaceKey`, C-5d.2 — DONE).
- **The resolve graph** (`highcut_resolves.bin` sidecar + the decoder prints it): the CP records every
  guest EDRAM **Resolve** as `(after_draw, dest_addr, is_depth, src_depth_base, src_pitch, src_msaa)`.
  `python tools/highcut_packet_decode.py <build_dir>` cross-references each resolve **dest_addr** with
  the draws that later **sample** that address (via each texture binding's `fetch_base_addr`, packet
  v7+). Example from a goalie capture:
  `after draw 143 depth resolve dest=0x1C4A2000 src(depth=0 pitch=800 msaa=2) -> sampled by draws [553,554,555,556]`
  — i.e. the 768² shadow-map surface resolves to 0x1C4A2000, and the player draws sample it (that's
  the stubbed `slot15` depth map).

**The missing piece = bind the source surface's offscreen RT as the texture for draws that sample its
resolve dest_addr.** That's "Resolve = host copy": instead of untiling stale/stubbed guest memory at
`dest_addr`, bind the plume RT we already rendered for the source `SurfaceKey`.

## 4. Mechanism to implement

1. At load, build a map `resolve_dest_addr -> source SurfaceKey` from `highcut_resolves.bin`
   (`after_draw` orders it; a dest can be re-resolved — last-writer-before-sample wins).
2. In `BuildRenderableDraw` / the per-draw texture bind, for each texture binding whose
   `fetch_base_addr` matches a `resolve_dest_addr`, DON'T use the untiled blob — instead bind the
   color (or depth, if `is_depth`) attachment of that source surface's offscreen RT
   (`c.c5surfaces[srcKey]`), with the right view (depth→a depth-readable SRV; cube→6-face).
3. Render order: a surface must be fully rendered+"resolved" before a later draw samples it. The
   draws are already replayed in capture order; ensure offscreen surfaces render before the primary
   draws that sample them (today the split renders offscreen RTs first, then primary — verify the
   ordering vs `after_draw`).
4. **Cube reflections:** either untile the real 6 faces from guest memory (the cube branch currently
   emits neutral), or bind a resolved reflection surface if the game renders reflections to an RTT.
   Keep the **cube alpha = 1.0** safety (NHL12 proved zeroing it makes reflective meshes transparent).
5. **HUD/scoreboard / PiP:** these are secondary COLOR surfaces. After the primary draws, COMPOSITE
   their offscreen RTs onto the swapchain (a fullscreen blit/quad, respecting their blend) so the UI
   appears. Identify them by surface (e.g. the 1280×720 `pitch=1280` surface, or small overlay vps).

## 5. Why it matters — the deferred artifacts ALL trace to this (proven)

Everything we deferred during the gameplay audit is a missing-lighting-input symptom, not a separate
bug (`[[highcut-c5h-surface-selection]]` has the proofs):
- **Dark equipment / black goalie face / helmet "spotlight"** = missing AMBIENT/indirect (cube +
  resolved env). Direct light works; fill light doesn't → only reflective/direct-lit parts show.
- **Invisible trapper glove that DELETES the goalie behind it** (draw 556): the glove renders correctly
  but near-black (dark-equipment), and its output **alpha is derived from its own color**
  (`out.w = (color * sysconst[23]).z`), so dark → alpha≈0 → blends invisible BUT still writes depth →
  the body behind fails LEqual → background shows through. Fix the lighting inputs (color≠0) and the
  glove returns. (Confirmed via `_c5probe -Draw 556 -Ssa "%6102" [-Alpha]`.)
- **Green/multicolor equipment speckle** = specular from the incomplete lighting env (RULED OUT: cube
  stub, BC5 normal, depth map — none individually fix it; it's the lit result, not a texture).

So when the resolved inputs are composited, expect the equipment to light, the glove to reappear, and
the speckle to settle — without touching any shading math (we run the GAME's shaders; we only feed
inputs).

## 6. Suggested order (each independently verifiable)

1. **Depth/shadow map first** (simplest, the resolve graph already maps it): bind the resolved depth
   surface where players sample it (the `k_24_8` slot). Verify self-shadowing appears and the goalie
   gains contact shadows. Low risk — one surface, depth-only.
2. **Reflection cube** (real faces or resolved RTT). Verify helmet/pad reflections + the glove's alpha
   recovers (its color stops being ~0). Keep cube alpha = 1.0.
3. **HUD/scoreboard + PiP composite** onto the swapchain. Verify the scoreboard/clock appears (it's a
   secondary color surface today rendered to an offscreen RT and dropped).
4. Re-run the gameplay audit (goalie + an open-ice + a stoppage view) and confirm the deferred bucket
   is cleared. Then → **C-6 takeover** (plume as the sole renderer).

## 7. Tooling (all built + verified)

- **Capture:** `_c5dump.ps1` (beta-live + F10 at the view you want; overwrites `highcut_frame_*.bin`).
- **Render + probe:** `_c5probe.ps1 -Draw N [-Full] [-PrimaryPitch P -PrimaryDepth D] [-Ssa %ID]
  [-Cap K -After %ID] [-Scalar %ID] [-Alpha] [-Step]` — extract→instrument→replay→**PNG** in one shot.
  The replay writes a framebuffer→PNG via `NHL_HIGHCUT_C5_SHOT` (so renders are verifiable headless).
- **Offline decode:** `python tools/highcut_packet_decode.py <build_dir>` — per-draw state, the
  **surface buckets**, and the **resolve graph** (resolve dest ↔ sampling draws) — your primary map.
- **Stub census:** `tools/_stub_census.py <build_dir>` — what's still stubbed, by real Xenos format.
- **Per-draw state:** `tools/_goalie_state.py` (blend/depth/cull/exp_adjust), `_instrument_ps.py`
  (force PS output = a register/SSA/scalar; capture-at-point), `_check_residency.py` (asset preflight).
- **RenderDoc:** `_c5renderdoc.ps1` (Vulkan layer on the plume instance).
- Build: `_build_beta.bat` → `BUILD_EXIT=0` (vcvars64 + LLVM; build from a real shell).

## 8. Key files

- `gpu/hooks/plume_present.cpp`: `LoadC5Frames` (surface buckets + primary pick, ~1133); the
  `c.c5surfaces` offscreen-RT cache (C-5d.2); the per-draw render loop + the split (primary→swapchain,
  others→offscreen, ~1500-1560); `BuildRenderableDraw` (the texture-binding loop — where a
  resolve-dest binding would swap the untiled blob for the source RT's attachment); the cube branch in
  `createTextures`; `mapFmt`.
- `renderer/core/nhl_command_processor.cpp`: `untileBindings` (~2085 — the stub + cube + per-format
  switch; `td.fetch_base_addr` is the match key); the **Resolve capture** that writes
  `highcut_resolves.bin` (grep `H3RV` / `ResolveMarker` / `is_depth`); the surface-state capture.
- `gpu/hooks/highcut_draw_packet.h`: `TexturePacketDesc` (`fetch_base_addr`, `array_layers` for cube),
  the resolve sidecar layout (`RESOLVE_MAGIC`/`RESOLVE_DESC` in the decoder mirror it).
- `highcut_resolves.bin` (the resolve sidecar, written per capture).

## 9. One-line summary
> The direct render path is DONE (all formats untiled incl k_16, exp_adjust, numbers, camera). This
> phase wires **Resolve = host copy**: bind each resolved guest surface's offscreen RT where later
> draws sample its dest_addr — real shadow maps, reflection cubes, and HUD/scoreboard composite — which
> completes the lighting INPUTS and clears the whole deferred bucket (dark equipment, invisible glove,
> speckle, missing HUD). The resolve graph (`highcut_packet_decode.py`) is your map. Then → C-6 takeover.
