# High-cut C-5 — RESTART kickoff prompt (self-contained for a cold session)

> **Mission:** render a full **3D gameplay frame** correctly on the plume renderer. C-5 = capture every
> owned draw of one guest frame → replay them all in plume (flat RTs, no EDRAM). C-5a..C-5d built this
> incrementally and proved the pipeline works (menu ~90%, a real 3D rink renders, single skinned player
> meshes render correctly), BUT a full gameplay frame still does not compose: most of it is black or the
> players explode. The incremental path accumulated a fragmented picture. **This restart re-grounds C-5
> from first principles, keeps every proven fact + dead-end so nothing is re-derived, and lays out a
> RenderDoc-driven, verify-each-step plan.** Full path-C history: `docs/highcut-c-plume-renderer-plan.md`.
> Memory: `[[highcut-c-plume-renderer]]`.

---

## 1. The architecture (PROVEN — do NOT rebuild)

`recompiled game → PM4 → SDK CommandProcessor DECODE (PM4 parse, vertex/index, registers, the
SpirvShaderTranslator) → a self-describing packet per owned draw → OUR plume Vulkan renderer` at flat
logical sizes (1280×720), no EDRAM/fold. NOT a D3D9 reimpl — draws/RT-binds are inlined, so the CP
decode is the geometry source.

- **plume is Vulkan, in-process, on a dedicated thread** (a 2nd D3D12 device TDRs rexglue). Driven by
  the guest present. The window is `NHL high-cut (plume Vulkan)`.
- **Packet = `gpu/hooks/highcut_draw_packet.h` (v8).** Per draw: translated VS+PS SPIR-V (inline);
  vertex bytes (ALL `vertex_bindings()` packed into one SSBO, each fetch-constant rebased to its packed
  dword offset); kGuestDMA index bytes (big-endian — the VS swaps via `vertex_index_endian`); PS
  textures (→ descriptor **set3**); **VS bone-palette textures (→ set2)**; packed VS/PS float constants;
  bool/loop; SystemConstants; fetch constants; per-draw blend/depth/stencil/cull/viewport/scissor;
  surface identity (depth_base/pitch/msaa/color_base/format); resolve markers (sidecar).
- **Capture** = `renderer/core/nhl_command_processor.cpp` `RenderBetaOwnedDraw` (the C-4 survey + packet
  machinery; dumps every interesting owned draw to `highcut_frame_<N>.bin` + `highcut_frame.count`).
  **Replay** = `gpu/hooks/plume_present.cpp` (`BuildRenderableDraw`, `LoadC5Frames`, `RenderClear`).

---

## 2. What WORKS (proven — don't re-verify from scratch)

- **Menu (C-5a/b):** ~90% faithful 2D menu renders (cup/logos/crest, text, blend, scissor, color mask).
- **3D geometry (C-5c):** depth+stencil+cull + **indexed** (kGuestDMA) + multi-binding vertex draws
  render a coherent rink (goal, boards, kickplate, post) with correct occlusion — no fold/shear.
- **The translated Xenos shaders are CORRECT.** Fetch + ALU SPIR-V translators are **byte-for-byte
  verbatim** vs upstream Xenia @95a5c3e (diffed normalized: 0 logic diffs; only the mechanical
  `xe::`→`rex::` rename). Texture swizzle is **view-handled** (`image_view_format_swizzle` ON; the VS/PS
  never read sys-const members 17/18). **⇒ The translator is NOT the bug. Do not re-investigate it.**
- **Per-draw skinning CAN be correct.** The bone palette is `k_32_32_32_32_FLOAT` (Xenos fmt 38, slot
  16, 384×160 or 384×64) = float4 matrix rows; captured + untiled (`kTexRGBA32F`) + bound to **set2**
  (texture@0 unsigned + texture@1 signed + sampler@2; exact `vs_sampler_count`). Bone matrices verified
  valid orthonormal (row0⊥row1, unit length) + non-zero in guest RAM. **Single skinned draws render
  correctly:** draws 369 (VS 49608) and 375 (VS 49064) each render a clean distant-player mesh ALONE;
  RenderDoc confirms 708/708 verts have sane in-frustum NDC (w≈13.4). So set2 binding + bone data path
  are right for those variants.

---

## 3. What's BROKEN (the restart must solve — with current evidence)

A real gameplay frame (test capture: 1221 draws, **8 distinct surfaces**, **~9 distinct skinning VS
variants**) does not compose. Evidence from bisecting the "most-textured" surface (depth=736/640/2X, 538
draws):
- **The "displayed scene = most-textured surface" heuristic is WRONG.** depth=736's non-skinned static
  draws (idx 226–368) render **BLACK**; its skinned draws (369–765) **explode**. So depth=736 is not a
  cleanly-displayable surface. The actual displayed frame is built by the guest's **compositor**
  (per-surface render → resolve → front-buffer present), not one surface shown directly. Other surfaces
  in the capture: depth=360/241tex, depth=720/**1280px**/88tex (full-width — a composite?), depth=184
  (prepass), several 320-pitch mask passes.
- **Skinning is NOT fully solved across VS variants.** Of the depth=736 skinned VS sizes
  {49608:12, 49064:14, **65988:94**, 74024:16, 73608:28, 81964:36, 48936:2, 40692:12, 75484:4}:
  - 49608 (draw 369) ALONE = clean. 49064 (draw 375) ALONE = clean.
  - **65988 (draw 381) ALONE = BLACK (collapses to zero — like the pre-set2-fix bug).** 65988 is the
    MAJORITY (94 draws). So the most-common skinning shader still produces degenerate output. **THIS is
    the single highest-value unknown — 94 of ~218 skinned draws.**
  - **[2026-06-14 — DISPROVEN by LIVE RenderDoc. The 65988 VS does NOT collapse.]** Captured draw 381
    (65988) and draw 375 (49064) live in RenderDoc with the CURRENT build. 381's VS-Out `gl_Position`
    is finite and structured (VTX0 `-0.164, 2.058, 14.053, w=14.152`; all verts w≈14.15 positive,
    positions vary smoothly; coherent mesh in the VS-Out preview), the SAME CLASS as the working 375
    (VTX0 `0.290, 1.228, 13.342, w=13.441`). Bindings/UBOs byte-for-byte equivalent. ⇒ the prior
    "381 collapses to black" was a **STALE-ENV / STALE-BUILD artifact** (exactly the dead-end §7 warns
    about), NOT a shader bug. **Skinning is working for BOTH variants.** The real residual symptom: both
    players project to a TINY distant blob (~1.5% of screen, z/w≈0.993, near far plane). That is either
    the real camera or a projection/scale issue — but it is NOT a skinning collapse. **C-5's center of
    gravity moves OFF skinning and ONTO composition/surface-selection (R2).**
  - **[2026-06-14 — FULLY NARROWED via RenderDoc.]** The isolated player blob is BLACK on the swapchain,
    but Pixel History shows the player PS **Shader-Out = R=G=B≈0.00013, A=0** — i.e. players RASTERIZE
    and the front primitive PASSES depth/cull/blend, but the PS emits near-black. Systematically
    ELIMINATED (each verified): VS position ✓, raster ✓ (Highlight-Drawcall coverage), depth ✓ (overlay
    green; cleared 1.0 vs z≈0.993), cull ✓ (overlay green; `-NoCull` still black), blend=copy + cwm=0xF ✓,
    PS textures have data ✓, `color_exp_bias`=1.0 ✓, texture `exp_adjust`(Ldexp)=0 ✓, PS float constants
    non-zero ✓, **VS→PS interpolators all healthy ✓** (interp_0 texcoord; interp_2/3/4/5 = unit-length
    normals/tangents/lighting vectors; interp_1/6/7/8/9 zero but PS overwrites them as scratch). ⇒ the
    bug (if any) is in the **lit player PS math itself** producing a near-zero, OR the pass renders at a
    low LDR range that a later composite scales up. **Next cheap test: crank RenderDoc Range/exposure on
    the blob** — dim-player-appears ⇒ uniform exposure/scale fix; flat ⇒ pixel-debug the 2988-id PS.
    Memory: [[highcut-c5-skinning-solved-ps-dark]].
  - **[2026-06-14 UPDATE — bone-palette hypothesis DISPROVEN offline]** Decoded the VS textures of draw
    381 (65988, collapses) vs draw 375 (49064, works) directly from the v8 packets. They bind the
    **byte-identical** set2 bone palette: both `vstex0/1` = fmt=4 RGBA32F 384×160, base `0x1C7AE000`,
    same data (47214/61440 nonzero float4 rows, identical leading matrix floats). Both have
    `vs_texture_count=2, vs_sampler_count=1` and **identical constant-bank sizes** (fetch=768, sys=512,
    bool=160, vs_float=272). ⇒ the collapse is **NOT** "a 2nd bone palette / more bone matrices / a
    missing set2 input." The ONLY differences are mesh-side: vertex_count 708→1517, shared_bytes
    (packed vertex SSBO) 10944→24960, the vertex bindings, and the shader itself. **New prime suspects:**
    the per-vertex blend-index/weight attributes packed into the SSBO (wrong bone gets sampled →
    collapse), the vertex-fetch layout for THIS mesh's bindings, or the 65988 shader. The multi-binding
    SSBO pack + fetch-rebase ([nhl_command_processor.cpp:1948-1967](../renderer/core/nhl_command_processor.cpp)) is shared with the working 375, so
    it is not an obvious 381-specific bug — the divergence is in the *data* a 1517-vert mesh feeds it.
  - The cohort 369–381 rendered TOGETHER explodes (sane central cluster + streaks) — consistent with the
    65988 collapse + the others, NOT a multi-draw state bug (plume gives each descriptor set its own
    pool, maxSets=1 — full per-draw isolation; ruled out aliasing).
- **Resolves don't feed the opaque pass** (v7 capture: opaque draws sample 0 resolve targets; only a
  self-referential depth=184 blur is sampled). So resolve=host-copy is low-value for the *visible* pass;
  the composition problem is elsewhere (which surface presents + skinning).

---

## 4. The reframing (likely what we were missing)

C-5 has been "pick a primary surface and show it." The frame is actually a **guest compositor result**.
Two foundational things to establish FIRST, before more draw-debugging:

1. **Which surface/address does the guest PRESENT?** H-1 found the present surface (`sub_827F1C88` args:
   `surface*`, w, h). The LAST resolve of the frame (e.g. dest `0x1A329000`, src depth=360) is a
   front-buffer candidate. Determine the presented surface's EDRAM/address and which draws+resolves
   build it. Reconstruct THAT, not "most textured."
2. **The 65988 skinning VS must render non-degenerate alone.** 94 draws depend on it. **The set2/bone-
   palette input is already RULED OUT (§3 update — 381 and 375 bind identical palettes).** So the VS-Out
   inspection must focus on the VERTEX side: the post-VS `gl_Position` per vertex (w≈0 / origin?), then
   trace back to which fetched attribute (blend indices/weights/position stream) is wrong. RenderDoc
   Mesh-Viewer VS-Out for 381 vs 375 is the GUI ground truth; **the autonomous alternative is to add a
   VS-position READBACK to the plume replay** (transform-feedback / SSBO write of `gl_Position`) so 381
   vs 375 can be diffed offline without a manual F12 capture — this unblocks the whole R1 track headless.

---

## 5. Systematic plan (RenderDoc-driven — minimize blind render round-trips)

The lesson from the incremental grind: **use RenderDoc + offline packet decode as ground truth; render
single isolated draws, not the whole frame.** Proposed order:

- **C5-R1 — Catalog the skinning VS variants.** For each distinct VS size, pick one draw, render it ALONE
  (`_c5render.ps1 -MinDraw N -MaxDraw N+1`) + capture in RenderDoc (`_c5renderdoc.ps1 -Draw N`). Record:
  renders-clean / collapses-black / explodes. Focus on the BLACK ones (65988 first). For a black/exploded
  variant, RenderDoc shader-debug one vertex: trace the bone fetch + the final gl_Position; compare to a
  working variant (375). Find the missing input. (Offline: `tools/highcut_packet_decode.py` to compare
  packet fields — vs_texture_count, fetch constants, vertex bindings, shared bytes.)
- **C5-R2 — Identify + select the DISPLAYED surface.** Render each surface alone
  (`_c5render.ps1 -PrimaryPitch P -PrimaryDepth D`); find the coherent scene. Cross-reference the guest
  present surface (H-1) + the final front-buffer resolve. Replace the "most-textured" primary heuristic
  with the real presented surface (or a composite of the right passes).
- **C5-R3 — Render the displayed surface with all VS variants fixed** → the scene with players.
- **C5-R4 — Composition** (resolves / per-surface only if R2 shows the frame is multi-pass-composited).

---

## 6. Tooling + run recipes (all exist, build-clean)

- **Build:** `_build_beta.bat` → `BUILD_EXIT=0` (clang/MSVC; needs vcvars64 + LLVM on PATH).
- **Capture a frame:** `_c5dump.ps1` (beta-live, F10 at a 3D scene, hold ~5s) → `highcut_frame_*.bin` +
  `highcut_frame.count` + `highcut_resolves.bin`. **Capture is naturally ~1 guest frame** (per-draw
  dump is slow; the present hook does NOT fire under takeover, so frame-delimiting is moot — it's one
  frame). Packets are v8 → re-dump after any version bump.
- **Replay:** `_c5render.ps1` (params, NOT persistent env — the script CLEARS stale overrides):
  `-MinDraw N -MaxDraw M` (draw window), `-PrimaryPitch P -PrimaryDepth D` (pick the surface),
  `-NoSplit` (=C-5c all-to-swapchain), `-Offscreen` (per-surface offscreen RTs), `-NoCull`, `-FlipFace`.
  Default = primary-only (most-textured) → swapchain. Prints `C5 overrides this run: ...`. 0 VUID target
  via the validation layer.
- **Offline decode:** `python tools/highcut_packet_decode.py <build_dir>` — per-draw state, surface
  buckets, resolve dependency graph; `--png` decodes textures. Struct format tracks the v8 header.
- **RenderDoc (live inspect):** `_c5renderdoc.ps1 [-Draw N] [-FullFrame]` — enables RenderDoc's Vulkan
  layer via env (no admin: `VK_LAYER_PATH` + `VK_INSTANCE_LAYERS=VK_LAYER_RENDERDOC_Capture` +
  `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`; RenderDoc 1.44 portable at `E:\Personal Projects\NHL Modding
  Studio\Tools\Graphic Editors\RenderDoc_1.44_64\RenderDoc_1.44_64`). Focus the plume window → F12 →
  `%TEMP%\RenderDoc\*.rdc` → open in qrenderdoc.exe. Mesh Viewer **VS Out** = post-VS positions (export
  to CSV for offline analysis); **VS In** shows only VTX/IDX (attrs fetched in-shader from the SSBO);
  Debug Vertex = step the VS (may crash on big shaders — prefer the Mesh Viewer tables). No headless
  Python in this RenderDoc build.

---

## 7. Gotchas / dead-ends (do NOT repeat)

- **Translator is verbatim-correct** (§2). Don't diff/port/re-investigate it. The bug is in OUR INPUTS
  or the composition model.
- **Stale env overrides = the recurring "all black".** `_c5render.ps1` now clears them; never rely on
  manually-`Remove-Item`'d env vars (they leaked for hours: a stale `PRIMARY_DEPTH=184` pinned the black
  prepass surface to screen and produced every "exploded" render before we caught it).
- **Big-endian indices** (RenderDoc shows IDX 0,256,512 = guest 0,1,2; the VS swaps — not a bug).
- **`texture_swizzles`=0 is harmless** (never read; swizzle is view-side). Don't "fix" it.
- **Per-surface offscreen rendering is OFF by default** (pure overhead until resolves use it;
  `-Offscreen` to enable). The non-primary surfaces aren't sampled.
- **MSAA / per-surface depth-stencil** still simplified (one shared depth on the swapchain path).
- Unsupported tex formats still → 2×2 magenta: fmt=49 (k_DXN/BC5 normal maps), fmt=24 (k_16). Bone
  palette fmt=38 IS handled. None block the displayed-surface/skinning questions.

## 8. Key files
- Packet: `gpu/hooks/highcut_draw_packet.h` (v8).
- Capture: `renderer/core/nhl_command_processor.cpp` (`RenderBetaOwnedDraw` ~1885 vtx/idx, ~2089 hdr,
  ~2060 untile incl. `untileBindings` lambda for PS+VS textures, ~1994 SystemConstants).
- Replay: `gpu/hooks/plume_present.cpp` (`BuildRenderableDraw` ~625, `createTextures` lambda w/ filter,
  set2/set3 layout+bind, `RenderClear` render loop ~1235, primary-surface selection ~895).
- Present/surface: `gpu/hooks/d3d9_resources.cpp` (`sub_827F1C88` present; `sub_827EF8E0` resolve).
- Tools/scripts: `tools/highcut_packet_decode.py`, `_c5dump.ps1`, `_c5render.ps1`, `_c5renderdoc.ps1`.
- Memory: `[[highcut-c-plume-renderer]]`.
