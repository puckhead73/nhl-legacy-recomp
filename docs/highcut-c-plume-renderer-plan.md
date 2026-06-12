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
2. ~~**Shaders = SPIR-V via the SDK.**~~ **DEAD (2026-06-11):** the SDK's `win-amd64` build is
   **D3D12-only** — it ships the `spirv_translator.h` *header* but the implementation is NOT in
   `rexruntime.lib` (`llvm-nm`: **0 `SpirvShaderTranslator` symbols**), and the header transitively
   needs glslang's `SPIRV/SpvBuilder.h` which isn't shipped either. So Xenos→SPIR-V via the SDK is
   not linkable. **REVISED — shaders = DXBC, backend = plume D3D12 sharing rexglue's device:**
   - Use the SDK's **DXBC** translator (already working in the beta path, exported) → DXBC bytes.
   - plume's D3D12 `createShader` only `assert`s the format *label* == DXIL but copies the raw
     bytecode into the PSO; the **D3D12 runtime accepts DXBC** containers → label DXBC as `DXIL`
     and it works. **No SPIR-V, no DXIL conversion, no glslang.**
   - The 2nd-D3D12-device TDR ([[highcut-h2-plume-present]]) is avoided by **patching vendored
     plume `D3D12Device` to ADOPT rexglue's existing `ID3D12Device`** (one device, multiple queues
     — standard D3D12) instead of creating its own. plume is vendored source, so this is fair game.
   - **RISK (must validate first):** sharing rexglue's *live, actively-submitting* device is a
     hypothesis (only the 2nd-*device* TDR was proven; one-device-multi-queue should be fine but
     isn't yet shown here). C's revised first step is a triangle on the adopted device.
   - Consequence: C is all-**D3D12** (present too can move off the H-2 Vulkan path once the device
     is shared), and the H-2 plume-Vulkan present becomes a fallback, not the C render path.
3. **Resources are ours.** Read guest RAM directly, upload to plume buffers (vertex/index/constant)
   — no shared-memory/write-watch/upload-ring machinery. Textures need an **untile** (Xenos tiled →
   linear) into a plume texture; do CPU untile first (simple), GPU-compute untile later (fast).
4. **RTs are flat.** Each guest color/depth surface → a plume RT at **logical** size (1280×720 /
   actual), never EDRAM pitch. Guest Resolve → host copy plume-RT → plume-texture. The fold cannot
   occur because EDRAM never exists here.
5. **Present = plume swapchain** (H-2 already drives it per guest Present).

## Shader path DECISION (2026-06-11, user): port a Xenos→SPIR-V translator (keep plume Vulkan)

Chosen over the shared-device D3D12+DXBC path to keep the render path fully decoupled from
rexglue's live device (no shared-device risk). What the port concretely requires:

- The SDK's `spirv_translator.h` IS Xenia's `spirv_shader_translator.h` (© Ben Vanik 2022, adapted
  to `rex::` by Tom Clay 2026), but the **implementation is not shipped**. So vendor the matching
  **Xenia `spirv_shader_translator*.cc`** (Xenia splits it across ~6–8 files: base + `_alu` +
  `_fetch` + `_rb` + `_memexport` + …), adapted to the `rex::` namespace and the SDK's
  `Shader`/`ShaderTranslator`/`Translation` types.
- **Dependency: glslang** — Xenia's translator emits via glslang's `SPIRV/SpvBuilder.h`. Vendor
  glslang (FetchContent) + its sub-deps (SPIRV-Tools, SPIRV-Headers) for the optimize/validate pass.
- **Risk: header drift.** Must find the Xenia commit whose `spirv_shader_translator.h` matches the
  SDK header, take its `.cc`, and reconcile any `rex::`-adaptation differences. The base
  `ShaderTranslator::TranslateAnalyzedShader` IS exported (so the analyzed-Shader plumbing is
  reusable); only the SPIR-V subclass impl is missing.
- **Effort: large / multi-session**, and it requires authorizing the vendoring of two substantial
  external codebases (glslang + Xenia translator source). The shared-device D3D12 alternative was a
  ~one-function plume patch reusing the already-working DXBC translator; kept as a fallback if the
  port's cost/risk proves prohibitive (the device-share hypothesis is cheaply testable in isolation).

Port milestones: **P-1 DONE (2026-06-11)** — glslang 14.3.0 vendored (FetchContent, ENABLE_OPT/HLSL
off), builds+links in the clang/MSVC toolchain, `spv::Builder` emits valid SPIR-V (magic 0x07230203),
and the SDK's `spirv_builder.h` compiles against it with no fatal API drift (probe:
`gpu/hooks/highcut_spirv_probe.cpp`, gate `NHL_HIGHCUT_SPIRV_PROBE`).

- **P-2a DONE (2026-06-11)** — ported `gpu/spirv/spirv_builder.cc` (Xenia → `rex::graphics`,
  mechanical namespace + include remap). Compiles clean.
- **P-2b DONE (2026-06-11)** — ported the 5 `gpu/spirv/spirv_shader_translator*.cc`; all compile
  clean and `SpirvShaderTranslator` LINKS (probe constructs one + calls
  `GetDefaultVertexShaderModification`).
  - **KEY CORRECTION to the "find the matching Xenia commit" premise:** there is **no** upstream
    Xenia commit/branch whose SPIR-V translator matches the SDK header. The SDK's
    `spirv_translator.h` is a **ReXGlue-enhanced kVersion-12** API (Tom Clay forward-ported the
    feature set from Xenia's *DXBC* translator: tessellation, user clip planes, draw-resolution
    scale, memexport-compute fallback, float24 depth modes). Upstream Xenia's SPIR-V translator —
    including latest master `95a5c3e` — is **kVersion-6** and never reaches 12. So the bodies are
    ported from upstream `95a5c3e` (kVersion-6) and **reconciled** against the kVersion-12 SDK
    headers; the kVersion-12-only features are **NOT implemented** by these bodies (the core
    ALU/fetch/control-flow/RB/memexport paths are identical across versions and port verbatim).
  - **Reconciliation surface (tiny):** 4 files compiled clean as-is; the main `.cc` needed 4
    spot-fixes — `CreateDepthOnlyFragmentShader` + `StoreResult` signature additions, a
    `kOutputPerVertexMemberCount`→`…CountMax` rename, and glslang's `makeFunctionEntry` gaining a
    `LinkageType` arg (passed `spv::LinkageTypeMax`). Plus 3 const data tables the SDK declares but
    doesn't export (`ucode::kAluVectorOpcodeInfos`, `draw_util::kD3D10StandardSamplePositions{2x,4x}`)
    supplied verbatim in `gpu/spirv/spirv_translator_tables.cc`.
- **P-3 DONE (2026-06-11)** — a real game vertex shader translates to valid SPIR-V at runtime.
  Gated `NHL_HIGHCUT_XLAT_TEST` in `RenderBetaOwnedDraw` (once/process): build a fresh
  `SpirvShader` from `beta_current_vs_`'s ucode (ISOLATED — never touches the live DXBC
  `D3D12Shader`, so the validated beta path is byte-identical and there is no clobber risk),
  `AnalyzeUcode` → `GetDefaultVertexShaderModification` → `GetOrCreateTranslation` →
  `TranslateAnalyzedShader`. Result on a 24-dword VS: `is_valid=true`, **6216 bytes, magic
  0x07230203**, dumped to `highcut_p3_vs.spv`. Structurally verified (SPIR-V 1.5; instruction
  stream walks exactly to end; Capability/MemoryModel/EntryPoint/Function/Return/FunctionEnd all
  present). Needed the ported `gpu/spirv/spirv_shader.cc` (the `SpirvShader` subclass).
  - **glslang drift fix (risk #5):** glslang 14.3.0's `Builder::createAccessChain` derives its
    result type from the STATEFUL `accessChain` (`getResultingAccessChainType`), but Xenia's
    translator calls it statelessly with explicit base+offsets → `NoResult` assert at runtime.
    Fixed by patching glslang to compute the type statelessly from base+offsets (idempotent
    `string(REPLACE)` in CMakeLists, guarded by a marker; glslang's own `collapseAccessChain`
    passes the stateful base/indexChain so it is unaffected). Kept glslang at 14.3.0 (re-pinning
    to Xenia's submodule SHA would have reverted the P-2b `makeFunctionEntry` fix and risked the
    SDK header / toolchain compat P-1 validated).
  - **`spirv-val`-CLEAN (2026-06-12).** With the Vulkan SDK installed (`C:\VulkanSDK\1.4.350.0`),
    `spirv-val` initially caught a REAL bug the structural check missed: `OpIAdd %int %int %v3float`
    (int + ndc_scale). Root cause = the P-2b kVersion mismatch — the `.cc`'s system-constants build
    array was the upstream kVersion-6 member set, but the translator accesses members via the SDK
    header's kVersion-12 `SystemConstantIndex` enum, so every constant past index 2 was off (this
    was also the C-3 driver crash). Fixed by rebuilding the `.cc` array to the full kVersion-12
    member set (added line_loop_closing_index, vertex_index_reset, compute_memexport_vertex_count,
    user_clip_planes, vertex_index_min/max, textures_resolution_scaled, alpha_to_mask). Now
    **`spirv-val` exits 0** on the 6644-byte module. (This is the kind of latent kVersion-6-body bug
    the P-2b notes warned about; other shaders may surface more — spirv-val each one.)
- **C-2/C-3 (next)** — feed the P-3 SPIR-V to plume-Vulkan `createShader` + build a pipeline,
  rejoining the C milestones below.

## Milestones (incremental, each independently testable)

- **C-1 — plume renders GEOMETRY (not just clears).** Extend the in-process plume thread
  (`gpu/hooks/plume_present.cpp`) to draw a triangle: SPIR-V VS/PS + vertex buffer + pipeline +
  `drawInstanced`, into the swapchain. Template = `third_party/plume/examples/triangle/main.cpp`.
  Proves the plume geometry + SPIR-V + pipeline path works in the required Vulkan/in-process setup.
  *Done = a triangle in the plume window, synced to guest Present.* **(START HERE.)**
- **C-2 — Xenos ucode → SPIR-V → plume. DONE (2026-06-11): shader bridge proven.** The P-3
  translated Xenos VS SPIR-V is handed to the plume Vulkan thread (in-memory
  `HighcutPublishTranslatedVS` from the CP thread, plus a `highcut_p3_vs.spv` disk fallback so
  C-2 runs present-only without beta takeover), which calls `createShader` →
  **`vkCreateShaderModule` returned `VK_SUCCESS`** on a real Vulkan driver, no validation/VUID
  errors on stderr. So plume-Vulkan accepts the ported translator's output as a shader module.
  Run: `NHL_HIGHCUT_PRESENT=1` (present-only; the disk fallback loads the prior P-3 dump). NOTE:
  beta-takeover + present same-run doesn't co-run cleanly yet — beta fires the owned draw during
  early boot before plume's 30-frame init, so the in-memory same-run path is wired but unproven;
  the disk path is the reliable demonstrator. Caveat: VK_LAYER_KHRONOS_validation is not installed
  (no Vulkan SDK), so this is driver-acceptance, not full validation-layer spirv-val.
  The remaining "render a triangle" part (pipeline + draw) needs the VS's descriptor/binding
  environment (system-constants UBO, shared-memory SSBO for vertex fetch) → that is **C-3**.
- **C-3 — one real decoded guest draw. IN PROGRESS (C-3a scaffolding built; pipeline create
  BLOCKED on a driver crash).** Bridge from the CP decode: take a 2D menu draw's
  vertex/index/constants/viewport (the data the beta CP already decodes in `RenderBetaOwnedDraw`),
  upload to plume, build a pipeline from its translated SPIR-V, draw it flat. Solid-color first
  (defer textures). *Done = a recognizable menu element rendered by plume.*
  - **VS resource interface (reflected from the SPIR-V):** set0/binding0 = `xe_shared_memory`
    (StorageBuffer, vertex fetch); set1/bindings 0,3,4 = system / bool-loop / fetch constants
    (UBOs); input `gl_VertexIndex`; output `gl_Position`. NO IA vertex input (fetch is via SSBO),
    no float-constant or texture bindings for this simple VS.
  - **C-3a built (gated `NHL_HIGHCUT_C3`):** solid-color PS (`gpu/hooks/shaders/solid.hlsl`,
    zero-input) + a reflection-driven descriptor/pipeline layout (set0 byte-address buffer; set1
    CBV@0,3,4) + `createGraphicsPipeline`, in `plume_present.cpp`. Layout + PS + VS module all
    create fine.
  - **Float-controls portability fix:** P-3 now translates with `signed_zero_inf_nan_preserve` /
    `denorm_flush_to_zero` / `rounding_mode_rte` **disabled** (`Features(true)` had enabled them).
    plume's device doesn't enable `VK_KHR_shader_float_controls`, and those execution modes made
    the driver crash. The VS SPIR-V is now vanilla (capabilities = `[Shader]` only, 6144 bytes).
  - **C-3a pipeline now CREATES (2026-06-12).** The driver crash was the invalid `OpIAdd` above
    (the kVersion system-constants mismatch), NOT the layout or float-controls. After the
    `spirv-val` fix, `createGraphicsPipeline` from the translated VS + solid PS + the reflected
    layout **succeeds** (`[highcut-C3a] graphics pipeline ... CREATED`, no stderr). So the full
    chain translate -> SPIR-V -> plume module -> plume **pipeline** works on a real Vulkan driver.
    (Float-controls stay disabled — never re-tested with them on; the real crash was the IAdd, so
    they can likely be re-enabled once plume's device advertises `VK_KHR_shader_float_controls`.)
  - **C-3b.1 DONE (2026-06-12): bind + draw infrastructure, VALIDATION-CLEAN.** plume_present.cpp
    (gated `NHL_HIGHCUT_C3`) now creates the VS's 4 descriptor buffers (system/bool/fetch UBOs +
    shared-memory SSBO via `RenderBufferDesc::UploadBuffer` CONSTANT/STORAGE), creates the 2
    descriptor sets, `setBuffer`s them, and `drawInstanced(3)` with both sets bound. Verified with
    the Vulkan SDK validation layer (`VK_LAYER_KHRONOS_validation` active on plume's instance, via
    `vk_layer_settings.txt` logging): **0 errors/VUID** — the descriptor sets match the shader and
    the draw is valid. Buffers are zero-filled here, so nothing visible renders (degenerate
    positions); this proves the bind+draw mechanics before real data.
  - **C-3b.2 STARTED (2026-06-12): real vfetch draw identified.** Added a draw survey to the P-3
    block (`NHL_HIGHCUT_XLAT_TEST`): translate the first 24 owned draws' VS, dump each to
    `highcut_p3_vs_NNN.spv`, log per-draw `vfetch_bindings`/`tex_bindings`; `NHL_HIGHCUT_XLAT_DRAW=N`
    selects which goes to `highcut_p3_vs.spv` + the plume bridge. **Finding:** the capture FRAME
    matters — frame 0 (default) is a trivial dark-overlay draw (VS computes a degenerate point, NO
    vfetch — confirmed in the disasm: position = `(0,0,0,1)`, no shared_memory access). With
    `NHL_BETA_CAPTURE_FRAME=120` (a menu frame), draw#0 is a **real geometry VS** — `vfetch_bindings=1`,
    27 ucode dwords, 8768-byte SPIR-V, 10 shared_memory/fetch_constant accesses, **spirv-val-clean**.
    That is the C-3b.2 render target.
  - **C-3b.2 DATA PATH DONE (2026-06-12): real decoded draw rendered through the translated VS,
    validation-clean.** A self-describing draw packet (`gpu/hooks/highcut_draw_packet.h`,
    `highcut_p3_draw.bin`) is dumped from `RenderBetaOwnedDraw` and loaded by the plume thread:
    - **fetch constants** — the full 192-dword fetch register space (`regs[0x4800]`) → the shader's
      `uvec4[48]` UBO, with the vertex fetch slot's base address REBASED to 0.
    - **system constants** — a `SpirvShaderTranslator::SystemConstants` built from `vpi.ndc_scale/offset`,
      `VGT_INDX_OFFSET`/`VGT_*_VTX_INDX`, `result.host_shader_index_endian`; flags=0 (no-perspective
      2D start). (The struct is public + the C-3a layout fix makes member offsets correct.)
    - **shared-memory SSBO** — the guest vertex bytes (`memory_->TranslatePhysical(f.address<<2)`,
      `f.size<<2`), placed at SSBO offset 0 to match the rebased fetch address.
    Verified: the selected menu-frame draw (3 verts, 28-byte stride, Xenos RectangleList,
    ndc 640×360) loads + binds + draws every frame, **0 validation errors**, runs stable (no crash,
    presents continuously). The full chain guest-draw-decode → ported translator → plume render is
    green end to end.
  - **C-3b.2 OBJECTIVELY CONFIRMED (2026-06-12): the translated Xenos VS produces CORRECT on-screen
    geometry.** Pixel readback via plume's texture-copy path was blocked (swapchain lacks
    `TRANSFER_SRC`; offscreen-RT `copyTextureRegion` GPU-faults), so instead the solid PS atomically
    increments a host-visible UAV per fragment (set 2, `space2/u0` in solid.hlsl) — no copy needed.
    Result: **460,800 fragments rasterized per frame = EXACTLY half of 1280×720.** The menu draw is a
    Xenos RectangleList (3 verts of a full-screen rect); drawn as a TRIANGLE_LIST, its 3 corners make
    a triangle covering half the rect = half the screen — precisely. So the VS fetched the real
    vertices (rebased SSBO), applied the real `ndc_scale/offset`, and produced the right geometry.
    Validation-clean, stable. The full pipeline guest-draw → ported translator → plume render is
    proven correct.
  - **C-3b.3 DONE (2026-06-12): FULL rect renders.** The beta path leaves a Xenos RectangleList as
    host_prim=rect/hvst=kVertex (the SDK D3D12 backend expands rects natively); plume has no rect
    primitive, so the P-3 translation now forces `kRectangleListAsTriangleStrip` for rect draws (the
    VS synthesizes the 4th corner in-shader), the packet carries a `topology` field (→ TRIANGLE_STRIP)
    + host vertex count (4 per rect), and the plume pipeline is created with that topology. Result:
    **921,600 fragments = exactly 1280×720 (full screen)** — the complete full-screen rect, up from
    the 460,800 half. Validation-clean. **C-3 (one real decoded guest draw, rendered correctly) is
    COMPLETE.**

- **C-4 — textures (STARTED 2026-06-12: target identified).** The survey now skips trivial
  boot-overlay draws (only translates draws with a vfetch VS or a textured PS) and reports the PS
  texture-binding count. In live mode it found **textured draws** — e.g. draw#22/#26/#30 have
  `vfetch_bindings=1` + `ps_tex=1` (a real textured menu element; VS 8528 B). That is the C-4 render
  target (select via `NHL_HIGHCUT_XLAT_DRAW`).
  - **C-4 REMAINING (a large milestone):** (1) translate the PIXEL shader too (currently only the VS
    is dumped; the textured PS samples a texture); (2) reflect the PS's texture + sampler descriptor
    sets; (3) **untile the guest texture** (Xenos tiled → linear) into a plume texture — the hard
    part (tiling/addressing math); the texture fetch constant (6-dword, the texture slot in
    `regs[0x4800+slot*6]`) gives base/format/size; (4) create the plume texture + sampler + SRV,
    bind them to the PS descriptor sets; (5) build a pipeline with VS + real PS + the full layout and
    render textured. Then C-5 (full frame), C-6 (takeover).
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
