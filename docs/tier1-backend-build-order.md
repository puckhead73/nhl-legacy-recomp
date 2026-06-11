# Tier-1 Parity Backend — Scoping & Build Order

**Status:** design / for review. No backend rendering code is written yet.
**Goal:** reproduce the SDK D3D12 backend's output *exactly* (pixel/frame parity), nothing more.
Tier-2 enhancements (res scale, AF, texture replacement, post chain) are explicitly out of scope
here — but the architecture chosen below is dictated by what Tier-2 will need, so §1 matters most.

Companion docs/memory: plan §2 (approach D), §3–§5, §13; memories `renderer-injection-seam`,
`frame-feature-inventory`, `trace-capture-replay-constraint`, `rexglue-build-environment`.

---

## 0. What is already done (do not redo)

- **Injection seam (α, "delegate") works and is permanent.** `NhlD3D12GraphicsSystem :
  rex::graphics::d3d12::D3D12GraphicsSystem` overrides `CreateCommandProcessor()` to return
  `NhlD3D12CommandProcessor : rex::graphics::d3d12::D3D12CommandProcessor`, injected at
  `OnPreSetup`. The concrete D3D12 ctor + protected virtuals (`IssueDraw/IssueCopy/LoadShader/
  IssueSwap/WriteRegister`) are exported from `rexruntimerd.dll`. Today every override
  **log-and-delegates** to the base → bit-identical to stock.
  ([renderer/core/nhl_command_processor.cpp](../renderer/core/nhl_command_processor.cpp))
- **Capture/replay is complete and faithful for all 5 scene types** (menu, player-select,
  edit-player, gameplay, instant-replay). In-host replay (`NHL_REPLAY_XTR`) drives draws through the
  live backend + presenter and reads pixels via `presenter()->CaptureGuestOutput()`. Streaming +
  `NHL_CAPTURE_FULL=1` make traces self-contained. **This is our parity oracle.**
- **Frame-feature inventory is complete** (`frame-feature-inventory`). Tier-1 must support: all 7
  prim types (TriStrip dominant), the index path (≤26K idx), 1X/2X/4X MSAA + EDRAM store/resolve,
  depth test+write, backface cull, alpha-test, 2–8 blend states — and crucially **a single render
  target everywhere** (`mrt_draws=0`). No MRT/deferred in Tier-1.

---

## 1. The pivotal architecture decision (resolve this before any code)

### 1.1 What the SDK actually exposes (verified against the 0.8.0 headers)

The backend is *not* a black box: the whole concrete D3D12 stack ships as **public headers**, compiled
into `rexruntimerd.dll`. But the seams are uneven, and that unevenness decides everything:

| SDK class | Subclassable? | Constructible by us? | Notes |
|---|---|---|---|
| `D3D12GraphicsSystem` | **yes** (already done) | yes | `CreateProvider`, `CreateCommandProcessor` virtual |
| `D3D12CommandProcessor` | **yes** (already done) | yes | inherits abstract `CommandProcessor` front-end |
| `D3D12RenderTargetCache` (EDRAM) | **no — `final`** | **yes** — public inline ctor `(register_file, memory, trace_writer, scale_x, scale_y, D3D12CommandProcessor&, bindless)` | tightly bound to a `D3D12CommandProcessor&` |
| `D3D12TextureCache` | **no — `final`** | **yes** — `static Create(register_file, D3D12SharedMemory&, scale_x, scale_y, …)` | upload hook `LoadTextureDataFromResidentMemoryImpl` is private |
| `D3D12PrimitiveProcessor` | **no — `final`** | **yes** — public ctor `(register_file, memory, …)` | strip/fan/quad/rect expansion lives here |
| `PipelineCache` | yes (not final) | **yes** — public ctor `(D3D12CommandProcessor&, register_file, …)` | `LoadShader`, `ConfigurePipeline` |
| `D3D12SharedMemory` | yes (not final) | **yes** — public ctor `(D3D12CommandProcessor&, memory, …)` | guest-RAM mirror buffer |
| abstract `RenderTargetCache` / `TextureCache` | **yes** | yes | the *real* per-resource hook seam (`CreateTexture`, `LoadTextureDataFromResidentMemoryImpl`, `CreateRenderTarget`) |

Two facts dominate:

1. **The base `D3D12CommandProcessor` builds its five caches as *private* members** (`render_target_cache_`,
   `texture_cache_`, `pipeline_cache_`, `shared_memory_`, `primitive_processor_`). If we keep delegating
   to the base, we can never reach them — so **no texture replacement, no sampler/AF override, no
   shader override is possible in architecture α**. α is a perfect parity oracle and a permanent
   dead-end for Tier-2.
2. **The three worker caches we'd most want to customize are `final`** — so the EDRAM cache and the
   texture cache cannot be customized by *subclassing the D3D12 class*. Tier-2 hooks therefore come
   from one of two places: owning the *call sites* (we decide what we feed the cache and when), or
   replacing a cache wholesale with a subclass of its **abstract base**.

### 1.2 The three candidate architectures

- **α — Delegate-and-intercept (today).** Subclass `D3D12CommandProcessor`, forward the draw path to
  the base. *Parity: free (it IS the SDK).* *Tier-2: impossible* (caches private+final). Keep it
  forever as the regression oracle, selectable by env var. **Not the Tier-1 product.**

- **β — Owned CP over the reused (final) D3D12 caches (RECOMMENDED Tier-1).** Subclass
  `D3D12CommandProcessor`; in our own `SetupContext()` construct our *own* instances of all five
  D3D12 caches (passing `*this` as the `D3D12CommandProcessor&`, and our protected `register_file_`/
  `memory_`/`trace_writer_`); reimplement `IssueDraw/IssueCopy/IssueSwap/LoadShader` to drive them.
  We **reuse** the hard, correct machinery verbatim — EDRAM tiled store + MSAA resolve
  (`D3D12RenderTargetCache::Update/Resolve`), untiling/format conversion (`D3D12TextureCache`),
  ucode→DXBC (`PipelineCache::LoadShader/ConfigurePipeline`), strip/fan/quad/rect expansion
  (`D3D12PrimitiveProcessor`) — and we **own** the orchestration (bindings, root signatures,
  descriptor heaps, system constants, viewport/scissor, present). *Parity: must be re-earned, draw by
  draw, but EDRAM is not reimplemented.* *Tier-2: unlocked* — because we own the cache instances and
  every call site, we can swap a cache for a γ-style abstract subclass exactly where a hook is needed.

- **γ — Owned caches via the abstract bases.** Subclass abstract `TextureCache` /`RenderTargetCache`
  and reimplement `CreateTexture` / `LoadTextureDataFromResidentMemoryImpl` / `CreateRenderTarget`.
  This is the *only* way to get a true per-resource upload hook (texture replacement at the moment of
  upload). It also means re-deriving the D3D12 texture/RT machinery the SDK already wrote.
  **Not Tier-1.** Reserve it for Tier-2, and only for the texture/pipeline caches — never for EDRAM.

### 1.3 Recommendation

**Build Tier-1 as β. Keep α as the permanent oracle. Defer γ to Tier-2, texture/pipeline only.**

Rationale: β is the *minimum* architecture that even gives us a texture-cache instance we control —
that is the gate for every headline Tier-2 goal. And β re-frames the project's #1 risk: **we do not
reimplement EDRAM.** `D3D12RenderTargetCache` is reused intact; its `Update()` (bind RTs before a
draw) and `Resolve()` (EDRAM→shared-memory copy with MSAA downsample) are called from our IssueDraw/
IssueCopy. The genuinely hard work shifts from "emulate EDRAM" to **"reproduce the CP's binding and
constant computation exactly so our draws bind identically"** — see §4.

> **Honest cost of β:** we re-derive the body of `D3D12CommandProcessor.cpp` (which does *not* ship
> as source) — `UpdateBindings`, `UpdateSystemConstantValues`, `UpdateFixedFunctionState`, root-
> signature selection, descriptor-heap pools, the IssueDraw sequence. The class header
> (`d3d12/command_processor.h`) exposes every method signature, member, root-parameter enum, and
> system-constant layout we must match, so this is *transcription against a known spec with a
> pixel-exact oracle*, not blind reinvention. That is the bulk of Tier-1's effort.

---

## 2. Reuse vs build — component ledger

**Reuse from the binary/headers (do NOT reimplement in Tier-1):**

- Front-end, inherited via `D3D12CommandProcessor` ← abstract `CommandProcessor`: ring buffer, PM4
  parse (`ExecutePacketType0/1/2/3*`), `RegisterFile`, MMIO trap, `BeginTracing`/`RequestFrameTrace`,
  `CallInThread`, `RestoreEdramSnapshot`, gamma-ramp/occlusion/memexport plumbing.
- The five worker caches (β constructs and drives them; does not modify them):
  `D3D12RenderTargetCache`, `D3D12TextureCache`, `D3D12SharedMemory`, `PipelineCache`,
  `D3D12PrimitiveProcessor`.
- Shader translation: `pipeline/shader/dxbc_translator.h` (via `PipelineCache::LoadShader`),
  `format/ucode.h`, `packet_disassembler.h`.
- Texture untile/convert/sampler: `pipeline/texture/{conversion,util,info}.h`, `sampler_info.h`.
- Draw glue: `util/draw.h` — `GetHostViewportInfo`, `GetScissor`, `GetResolveInfo`, `ViewportInfo`,
  `Scissor`, `ResolveInfo`, `MemExportRange`, resolve-copy shader enums.
- Present: `ui/graphics_provider.h`, `ui/presenter.h`, `ui/d3d12/d3d12_provider.h`,
  `GraphicsSystem::SetupPresentation` + `presenter()->CaptureGuestOutput`.

**Own (the new code, Tier-1):**

- `NhlD3D12CommandProcessor::SetupContext/ShutdownContext` — construct/own the five caches + the
  descriptor-heap pools, root signatures, constant-buffer pool, submission/fence/command-list
  lifecycle (mirroring `BeginSubmission/EndSubmission/CheckSubmissionFence`).
- `IssueDraw` — state→host translation, bindings, system constants, fixed-function state, RT update,
  pipeline configure, descriptor allocation, the actual `DrawIndexedInstanced`/`DrawInstanced`.
- `IssueCopy` — drive `D3D12RenderTargetCache::Resolve` + shared-memory writeback.
- `IssueSwap` — gamma/present via the owned presenter.
- `LoadShader` — thin wrapper over `PipelineCache::LoadShader` (Tier-2 will inject here).

---

## 3. Build order (each step has a parity oracle and gates the next)

The spine: stand up β's lifecycle and caches as an *empty* CP first, then move the draw path off α
onto β **one capability at a time**, diffing each against the α oracle on the captured scenes. The
captured-scene replay harness + `CaptureGuestOutput` per-pixel diff is the validation engine at every
step (menu → player-select → edit-player → gameplay → instant-replay, in rising complexity).

> Convention below: **Reuses** = SDK pieces leaned on; **Oracle** = how we prove the step.

### Phase 2 — Caches & lifecycle bring-up (no draws yet)
- **2.1 SetupContext skeleton.** Override `SetupContext`/`ShutdownContext`; construct
  `D3D12SharedMemory`, then `D3D12TextureCache::Create`, `D3D12RenderTargetCache`,
  `D3D12PrimitiveProcessor`, `PipelineCache`; stand up the constant-buffer pool, view/sampler
  descriptor-heap pools, submission fence + command allocators + `DeferredCommandList`. Mirror the
  base's `kQueueFrames=3` triple-buffering and `BeginSubmission/EndSubmission` exactly.
  - *Reuses:* all five cache ctors; `GetD3D12Provider()`, `ui::d3d12` heap/upload pools.
  - *Oracle:* headless `runtime.Setup` succeeds, `SetupContext` returns true, clean shutdown (no
    heap-corruption `0xC0000374` — cf. `trace-capture-replay-constraint` shutdown note). Caches
    initialize (`render_target_cache_->Initialize()`, `texture_cache` init, etc.). Still delegate
    `IssueDraw` to base so the game keeps rendering while the new lifecycle is proven inert.
- **2.2 Shared-memory + register state.** Route `WriteRegister`/`WriteRegistersFromMem` to keep our
  own view consistent (the base already updates `register_file_`; we mainly mirror the dirty/upload
  signalling the caches expect). Drive `D3D12SharedMemory` uploads of guest RAM ranges.
  - *Oracle:* `frame-feature-inventory` register reads (`ReadRegisterValue`) match between α and β
    paths for the same frame; shared-memory contents hash-match a guest-RAM snapshot.

### Phase 3 — Shader translation path
- **3.1 LoadShader → PipelineCache.** Implement `LoadShader` over `PipelineCache::LoadShader`
  (ucode→`D3D12Shader`); disassemble for debugging via `format/ucode.h`.
  - *Reuses:* `PipelineCache`, `dxbc_translator.h`.
  - *Oracle:* every VS/PS in each captured frame translates and compiles without error; shader count
    per frame matches the α `[nhl-gpu]` log (~16 distinct binds/frame on menu).
- **3.2 ConfigurePipeline.** Wire `PipelineCache::ConfigurePipeline` (PSO from VS/PS + render state).
  - *Oracle:* a PSO is produced for every (VS,PS,state) tuple seen in a captured frame; none fail.

### Phase 4 — First owned draw (the β/α switchover begins)
- **4.1 One static draw, color-only, 1X MSAA, no depth.** For the *simplest* menu draw, take β's
  IssueDraw: `GetHostViewportInfo`/`GetScissor` → `UpdateFixedFunctionState` → primitive-processor
  conversion → `render_target_cache_->Update(...)` (binds the single color RT) → `ConfigurePipeline`
  → `UpdateSystemConstantValues` + `UpdateBindings` (fetch/float/bool-loop CBVs, textures, samplers)
  → `DrawIndexedInstanced`/`DrawInstanced`. Gate by a per-draw predicate so only this draw runs on β,
  the rest still delegate to α.
  - *Reuses:* `util/draw.h`, `D3D12PrimitiveProcessor`, `D3D12RenderTargetCache::Update`,
    `D3D12TextureCache`, `PipelineCache`; the base's public `PushTransitionBarrier`,
    `GetDeferredCommandList`, `GetRootSignature`, `RequestViewBindfulDescriptors`,
    `GetConstantBufferPool`.
  - *Oracle:* offline replay of that single draw to the offscreen RT renders a recognizable mesh; its
    region in `CaptureGuestOutput` matches the α replay within tolerance.
- **4.2 Indexed + alpha-test + 2–8 blend states + cull.** Extend to the full menu draw set (no depth,
  mostly 1X). Menu is 85% alpha-test, 0% depth, single RT — the right second target.
  - *Oracle:* **whole main-menu frame** β replay == α replay ≥ the streaming-replay bar already hit
    (100% of pixels within 8/255, mean channel diff ≈3.4). This is the first full-frame β parity gate.

### Phase 5 — EDRAM, depth, MSAA, resolve (the integration-hard part — see §4)
- **5.1 Depth test+write.** Enable the depth RT through `render_target_cache_->Update` +
  `UpdateFixedFunctionState(normalized_depth_control)`. Validate on edit-player (26% depth) before
  gameplay (66%).
  - *Oracle:* edit-player β==α; per-pixel + a depth-buffer diff (dump depth RT via the RT cache's SRV).
- **5.2 MSAA 2X/4X store in EDRAM.** Drive the MSAA sample count into RT keys; rely on
  `D3D12RenderTargetCache` tiled-MSAA storage. Gameplay is 72% 2X; instant-replay has 4X.
  - *Oracle:* gameplay β==α (the 1590-draw/2X-dominant scene); instant-replay 4X draws match.
- **5.3 Resolve (IssueCopy).** Implement `IssueCopy` via `GetResolveInfo` → `D3D12RenderTargetCache::
  Resolve(memory, shared_memory, texture_cache, written_address, written_length)` → shared-memory
  writeback; handle MSAA downsample and the `RB_COPY_*` path that `trace_resolve_analysis.py` decodes.
  - *Oracle:* resolve targets land in guest/shared memory matching α; whole-frame parity on **all 5
    scenes**, including instant-replay (3110 draws, broadcast camera). This is the Phase-5 exit and the
    Tier-1 "renderer is correct" proof.

### Phase 6 — Live + present + HUD
- **6.1 Owned present.** `IssueSwap` through the owned `Presenter` (gamma ramp apply, frontbuffer
  present). Confirm `SetupPresentation`/provider-with-presentation lifecycle (open question Q2).
- **6.2 Flip the live game to β.** Remove the per-draw α/β gate; run `nhllegacy.exe` fully on β.
  Fix HUD/2D/text/alpha-ordering regressions surfaced only live.
  - *Oracle:* live frame hashes match the α baseline on the scene suite; manual playthrough
    menu→gameplay→replay→edit-player at parity; `NHL_SHOT_FRAME` live screenshots match α.

**Tier-1 done** at the end of Phase 6: the game runs entirely on our owned backend at parity, with α
retained as the oracle. Tier-2 (γ-swap the texture/pipeline caches for the enhancement hooks) starts
from here.

---

## 4. Where the genuinely hard part sits (re-framed)

Plan §13 ranks **EDRAM resolve** as risk #1 assuming we reimplement it. Under β we **do not** — we
call `D3D12RenderTargetCache::Update/Resolve` verbatim. So the hard part relocates:

1. **Binding & system-constant parity (highest real risk, Phase 4–5).** Our `UpdateBindings` and
   `UpdateSystemConstantValues` must produce the *exact* root-parameter layout, CBV contents
   (`DxbcShaderTranslator::SystemConstants`), fetch/float/bool-loop constant packing, and descriptor
   tables the DXBC the translator emits expects (plan Q4). A subtle mismatch = wrong pixels with no
   crash. *Mitigation:* the header pins the entire `RootParameter` enum and `SystemConstants` struct;
   diff our generated CBV bytes against an α capture for the same draw before trusting pixels.
2. **EDRAM *integration* (Phase 5), not emulation.** Calling `Update` at the right time (before
   rasterization, with correct `normalized_color_mask`/`normalized_depth_control`) and `Resolve` with
   the correct `ResolveInfo`, plus the tile-ownership/transfer barriers the cache requests via the
   CP's `PushTransitionBarrier`/`RequestPixelShaderInterlockBarrier`. Wrong sequencing = corruption.
   *Mitigation:* `trace_resolve_analysis.py` + per-resolve dest diffs (the IssueCopy capture already
   exists).
3. **Submission/fence/descriptor lifecycle (Phase 2, foundational).** Reproducing triple-buffered
   `BeginSubmission/EndSubmission`, command-allocator recycling, and descriptor-heap pool ring without
   the base's body. Bugs here are device-removal / use-after-free, not pixels. *Mitigation:* the
   header exposes every member; bring up inert (delegating draws) and stress with the long
   instant-replay capture before moving draws onto β.

MSAA tiled store, untiling, and shader CF/predication — plan's other top risks — are **reused** and
drop to integration concerns.

---

## 5. Open questions to close before Phase 2 code (plan §15)

1. **Q1 (β feasibility spike) — ✅ DONE 2026-06-07.** The load-bearing unknown was whether the SDK
   *cache* symbols (not just the CP's) are exported and usable from our consumer TU. Probe behind
   `NHL_BACKEND=beta` in `SetupContext` (`RunBetaCacheProbe`): construct → `Initialize()` →
   `Shutdown()` → destruct our own `D3D12SharedMemory(*this, *memory_, trace_writer_)`. Result: **link
   succeeds, Initialize returns true with a valid buffer, clean shutdown + destruct, no crash** (run
   headless via `NHL_REPLAY_XTR`). β is viable at its foundation. *Remaining sub-question, deferred to
   Phase 2:* whether a `SetupContext` that builds our own caches and **skips** the base `SetupContext`
   survives the base dtor — low risk (null `unique_ptr` destruction is safe; we override
   `ShutdownContext`).
2. **Q2 (present):** own `Presenter` via `SetupPresentation`/provider-with-presentation, or reuse the
   base graphics-system lifecycle and only override the CP? Lean reuse (we already subclass the
   concrete `D3D12GraphicsSystem`); confirm `CaptureGuestOutput` still works on the β CP.
3. **Q4 (root signature shape):** does `PipelineCache`/`GetRootSignature` *require* the exact
   `RootParameter` layout in the header, or can we choose our own? (Determines how much of
   `UpdateBindings` we can simplify vs must mirror.) Inspect `GetRootSignature` behavior in a 4.1 spike.
4. **Bindless vs bindful:** the base supports both (`bindless_resources_used_`). Pick **bindful** for
   Tier-1 (simpler, matches `RequestViewBindfulDescriptors`); revisit for Tier-2 perf.
5. **Q5 (camera/projection recovery):** defer — Tier-2/ultrawide/DLSS concern, not parity.

## 6. First concrete steps

1. Write the β-feasibility spike (Q1) into `NhlD3D12CommandProcessor` behind an env flag
   (`NHL_BACKEND=beta`), default off → α stays the live path.
2. Add `renderer/xenos/` (caches glue), `renderer/shaders/` (PipelineCache glue), `renderer/present/`
   per plan §14; keep `renderer/core/` as the CP/GS.
3. Stand up the Phase-2 SetupContext skeleton inert (delegating draws), prove lifecycle + clean
   shutdown on the longest capture (instant-replay, 90f/279K draws) before writing a single IssueDraw.
4. Only then begin the Phase-4 per-draw α→β switchover, gated per-draw, diffing every step against the
   α oracle on the 5 captured scenes.
