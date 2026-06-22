# Graphics — Rendering Pipeline

The renderer is the most heavily investigated subsystem in this project (Phase 4
bring-up). **This doc is a structured entry point**; the authoritative, blow-by-blow
detail lives in the existing investigation notes `[INV]`:

- [`../nhl12_renderer_system_notes.md`](../nhl12_renderer_system_notes.md) — the master renderer notes.
- [`../renderer_investigation_blue_texture_bug.md`](../renderer_investigation_blue_texture_bug.md) — the blue/green texture-corruption hunt + **Renderer Architecture Map**.
- [`../nhl12_renderer_regression_guardrails.md`](../nhl12_renderer_regression_guardrails.md) — what not to regress.
- [`../nhl12_vp6_green_video_fix_plan.md`](../nhl12_vp6_green_video_fix_plan.md) — the movie/video green-artifact plan.
- [`../nhl12_equipment_db_color_notes.md`](../nhl12_equipment_db_color_notes.md) — equipment colour DB notes.

## 1. Two-layer reality
There are **two** renderers to keep straight:
1. **The game's renderer** — original NHL 12 code: RenderWare 4 (`rw::`) + `nhlrender`
   glue, emitting **Xenos GPU register writes + command buffers**. This is what we're
   documenting.
2. **The host backend** — **RexGlue's D3D12** command processor that *consumes* those
   guest command packets and translates them. This is what makes pixels on a PC.

The game doesn't know about (2); it thinks it's talking to a 360. The boundary is the
`Vd*` ring (see [`../recompilation/rexglue-runtime.md`](../recompilation/rexglue-runtime.md) §4).

## 2. Host backend path (CONFIRMED `[INV]`)
```
recompiled game code ── writes Xenos regs + command buffers ─┐
                                                             ▼
RexGlue/src/graphics/command_processor.cpp        (consume guest packets)
RexGlue/src/graphics/d3d12/command_processor.cpp  (D3D12 backend)
   ├─ pipeline_cache.cpp        → Xenos→DXBC shader translation + PSO cache
   │     dxbc_translator*.cpp    (see shaders-materials.md)
   ├─ pipeline/texture/cache.cpp → guest fetch consts → TextureKey
   │     d3d12/texture_cache.cpp  (D3D12 resources + SRV descriptors)
   └─ d3d12/render_target_cache.cpp → EDRAM render-target emulation
D3D12CommandProcessor::IssueSwap  → resolve/present frontbuffer, logs [SWAP-FPS]
```

### The ROV requirement (CONFIRMED, critical)
For NHL 12, **ROV (Rasterizer-Ordered Views) is the known-good 3D path**
(`render_target_path_d3d12=rov`, baked into `Nhl12App::OnPreSetup`). The RTV path is
**not** a safe fallback — it can leave the gameplay scene **black** while UI still
renders. This was the root cause of the "black 3D scene" bug (it was the D3D12 RTV
resolve path, *not* color-grade or tfetch3D). `[INV]` (project memory: "Black 3D scene
FIXED").

## 3. The game's frame (INFERRED passes)
The original render thread (see [`../architecture/main-loop.md`](../architecture/main-loop.md))
builds a frame from a sim-state snapshot. Inferred pass order for a hockey game:
1. Shadow / depth pre-pass.
2. **Ice** (the rink surface — reflective, with painted lines/logos under a gloss).
3. **Players** (skinned skeletal meshes + equipment; see shaders-materials.md).
4. **Crowd** (cheap instanced/impostor geometry).
5. **Glass / transparent** rink elements.
6. **Particles** (`nhlrender/particle` + Lynx — ice spray, snow, breath).
7. **Post-processing** (color grade, bloom).
8. **UI / HUD** (scorebug, overlays — see [`../ui/frontend.md`](../ui/frontend.md)).
9. Present (`VdSwap` → `IssueSwap`).

> Pass order is INFERRED (standard sports-game compositing). Pinning it means reading
> the render submission in `nhlrender`/RW. **Not reached at runtime yet** (build stalls
> pre-first-frame on the `cache:\` content gap). `[RT]`

## 4. EDRAM & render targets (CONFIRMED `[INV]`)
The 360's 10 MB EDRAM and its tiling/resolve behaviour are emulated by
`render_target_cache.cpp`. NHL 12 relies on the ROV path here (§2). MSAA, gamma RT
mode, and draw-resolution scale are translation-key inputs (see shaders-materials.md).

## 5. Known rendering bugs (status, from `[INV]`)
| Bug | Status | Where |
|---|---|---|
| Black 3D scene | **FIXED** (ROV path) | memory; renderer notes |
| Blue equipment textures | **FIXED** in a known-good backup (`WORKING_RENDERER_NEVER_DELETE`); some movie-DXT hacks reintroduce it | blue_texture_bug.md |
| Green intro **video** | **OPEN** (paused) — DXT1, source clean, GPU-side, video-specific | vp6_green_video_fix_plan.md |
| Equipment colour/mip issues | investigated; see equipment notes | equipment_db_color_notes.md |

## 6. Recomp/GPU assumptions (CONFIRMED)
- Standard `Vd*` ring boundary — no MMIO needed.
- D3D12 required (ROV); Vulkan path exists in RexGlue but D3D12 is the tested one.
- Xenos shaders translated live to DXBC (see shaders-materials.md).

## Open questions
- The actual pass list/order (pin `nhlrender` submission).
- Camera system (see [`cameras.md`](cameras.md)).
- Crowd rendering technique.

See [`shaders-materials.md`](shaders-materials.md), [`cameras.md`](cameras.md),
and [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
