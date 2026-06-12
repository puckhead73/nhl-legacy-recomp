# High-cut path C — own the renderer on plume (CP-decode → plume RHI)

> **Goal:** render the game on a renderer **we own** (plume RHI, Vulkan in-process), at flat
> logical sizes, with **no EDRAM / no fold / no SDK `final` ceilings** — the high enhancement +
> performance ceiling chosen over the flat-RT hybrid (see [[high-cut-pivot-decision]],
> docs ranking in the session that picked C).

## What C is (and isn't)

C is **not** a pure D3D9 reimplementation — H-1 proved the game's **draws and RT-binds are
inlined** in the recompiled code, so they can't be hooked as clean D3D9 calls. So C =

```
recompiled game → PM4 → [SDK CommandProcessor decode]  → OUR plume renderer (flat, no EDRAM)
                         reused FRONT-END (decode only)   NEW BACK-END (we own RTs/passes/PSOs)
```

We **reuse the SDK's decode front-end** (PM4 parse, PrimitiveProcessor index/vertex, register
decode for viewport/constants/fetch-constants/RT+texture bindings, and the **SPIR-V shader
translator**) and **replace the output** — instead of submitting to the SDK D3D12 backend
(EDRAM, `final` RT cache, upload ring, the fold), we build plume pipelines/buffers/textures and
render into **flat host RTs at logical size**.

## Architecture decisions (validated this session)

1. **Backend = plume Vulkan.** A 2nd D3D12 device in-process TDRs rexglue ([[highcut-h2-plume-present]]);
   plume Vulkan coexists. (plume D3D12 only becomes viable once rexglue's GPU is fully off — a
   late "takeover" milestone, deferred.)
2. **Shaders = SPIR-V via the SDK.** plume's shader formats are DXIL/SPIRV/METAL (not DXBC). The
   SDK ships `rex/graphics/pipeline/shader/spirv_translator.h` (`SpirvShaderTranslator`, Xenia's
   Vulkan path) + a full `rex/graphics/vulkan/` backend. So Xenos ucode → `SpirvShaderTranslator::
   CompleteTranslation()` → SPIR-V bytes → `device->createShader(..., RenderShaderFormat::SPIRV)`.
   **No DXBC→SPIRV cross-compile.** (The beta CP currently uses the DXBC translator; the plume
   path uses the SPIR-V one instead.)
3. **Resources are ours.** Read guest RAM directly, upload to plume buffers (vertex/index/constant)
   — no shared-memory/write-watch/upload-ring machinery. Textures need an **untile** (Xenos tiled →
   linear) into a plume texture; do CPU untile first (simple), GPU-compute untile later (fast).
4. **RTs are flat.** Each guest color/depth surface → a plume RT at **logical** size (1280×720 /
   actual), never EDRAM pitch. Guest Resolve → host copy plume-RT → plume-texture. The fold cannot
   occur because EDRAM never exists here.
5. **Present = plume swapchain** (H-2 already drives it per guest Present).

## Milestones (incremental, each independently testable)

- **C-1 — plume renders GEOMETRY (not just clears).** Extend the in-process plume thread
  (`gpu/hooks/plume_present.cpp`) to draw a triangle: SPIR-V VS/PS + vertex buffer + pipeline +
  `drawInstanced`, into the swapchain. Template = `third_party/plume/examples/triangle/main.cpp`.
  Proves the plume geometry + SPIR-V + pipeline path works in the required Vulkan/in-process setup.
  *Done = a triangle in the plume window, synced to guest Present.* **(START HERE.)**
- **C-2 — Xenos ucode → SPIR-V → plume.** Drive `SpirvShaderTranslator` on one real guest shader
  and render a triangle with the translated SPIR-V (hardcoded geometry). Proves the shader bridge.
- **C-3 — one real decoded guest draw.** Bridge from the CP decode: take a 2D menu draw's
  vertex/index/constants/viewport (the data the beta CP already decodes in `RenderBetaOwnedDraw`),
  upload to plume, build a pipeline from its translated SPIR-V, draw it flat. Solid-color first
  (defer textures). *Done = a recognizable menu element rendered by plume.*
- **C-4 — textures.** Untile guest tiled textures → plume textures; samplers; bind. *Done = a
  textured menu draw.*
- **C-5 — full frame, flat multi-pass.** All draws of a frame; per-surface flat plume RTs; guest
  Resolve = host copy. Validate menu, then a 3D scene (the fold is structurally absent → no shear).
- **C-6 — takeover.** Suppress rexglue's present/GPU where possible so plume is the only output;
  optionally switch plume to D3D12 once rexglue's device is off.

## Where the reusable decode lives

`renderer/core/nhl_command_processor.cpp::RenderBetaOwnedDraw` already decodes every piece C-3+
needs (ProcessingResult, vfetch constants → addr/size, texture fetch constants, float/bool
constants, viewport/NDC, primitive topology, index buffer). C reuses that decode and swaps the
**output half** (SDK deferred command list → plume command list). The live residency learning
([[beta-live-render-path]]) carries over: read CURRENT guest RAM per draw (force-refresh dynamic
ranges) — but C uploads to its own plume buffers, sidestepping the SDK ring entirely.

## Risks / unknowns

- **SpirvShaderTranslator driving.** Need the exact API to translate a `Shader` ucode → SPIR-V
  outside the SDK's Vulkan pipeline cache (modification/register-count setup). C-2 derisks this.
- **Vertex-fetch in SPIR-V.** Xenia's SPIR-V VS pulls vertices via the shared-memory SSBO model;
  on plume we may instead supply a real IA vertex buffer + input layout (simpler) OR replicate the
  SSBO fetch. Decide at C-3 (start with IA vertex buffers from decoded vfetch ranges).
- **Untile.** CPU untile is simple but slow; fine for bring-up, optimize at C-4/C-5.
- **Coexistence cost.** Until C-6, both rexglue's GPU and plume run — extra overhead, acceptable
  for bring-up.

## Test harness

In-process, env-gated `NHL_HIGHCUT_PRESENT` (Vulkan plume thread). The game boots normally;
plume renders in its own window driven by guest Present. No takeover of the game's output until
C-6, so every milestone is additive and safe to leave gated-off.
