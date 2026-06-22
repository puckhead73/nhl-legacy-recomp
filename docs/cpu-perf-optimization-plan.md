# CPU Draw-Submission Optimization — Phased Plan

Goal: a single shipping build (`win-amd64-vk-pgo` / packaged) with all six identified
CPU optimizations implemented and measured. The game is draw-submission / single-thread
CPU bound (~1600 draws/frame, ~10–18 µs/draw on the one "GPU Commands" worker thread).

## The six opportunities

| # | Change | Binary | Effort | Risk | Expected |
|---|--------|--------|--------|------|----------|
| 1 | Reuse texture/sampler descriptor sets across draws (`command_processor.cpp:6736` TODO) | SDK | Med | Med-High | Highest single lever |
| 2 | Short-circuit `GetCurrentStateDescription` via register dirty-bit (`pipeline_cache.cpp:1100/1362`) | SDK | Med | Med | Med-High |
| 3 | Cache sampler params per fetch-constant (`command_processor.cpp:3920` TODO) | SDK | Med | Med | Med |
| 4 | Add `-mtune` (currently unset) | Build flags | Low | Low | Low-Med (Intel-asymmetric) |
| 5 | Re-test `primitive_processor_cache_min_indices` (default 0) | SDK cvar | Low | Low | Med (index-conv draws) |
| 6 | Multi-threaded command recording (secondary cmd buffers) | SDK | High | High | Potentially highest |

All six touch the **Xenia-derived SDK** at `E:\Tools\rexglue-sdk\src` except #4 (game + SDK
build scripts). Per SDK code change: rebuild+install (`scripts/_ffx_sdk_build_install.bat`)
then relink the game (`scripts/_game_ffx_build.bat` for dev, or `_build_vk_opt.bat` for a
measurement build). PGO is recaptured only once, at the end (Phase 5).

## Measurement harness (used every phase)

Two complementary measurements:
- **Deterministic SDK-isolating bench** — `NHL_REPLAY_BENCH=<N>` with `NHL_REPLAY_XTR=<trace>`
  + `NHL_VK_BACKEND=1` + `NHL_VK_NO_VSYNC=1`. Replays captured PM4 N times on the CP thread,
  logs warm min/median/mean ms (`[nhl-bench]`). **Bypasses the guest recomp → isolates the SDK
  command processor**, which is exactly where #1/#2/#3/#6 live. This is the primary A/B for
  those four (no PGO needed — relink the SDK only).
  - **TRACE REPRESENTATIVENESS (important).** The canonical `gpu_trace/scene_00..03`
    streams are NOT gameplay: scene_00=menu, scene_01=intro, scene_02/03=create-a-player
    (~400 draws/frame, ONE player model, low binding diversity). Benching on them
    *understates* #1/#2/#3 (whose payoff scales with binding/draw diversity, not raw
    draw count). Phase 0 captures a real on-ice gameplay stream (~1600 diverse
    draws/frame) → `gpu_trace/scene_04/`. Capture tool: F9 hotkey on the Vulkan-fsi
    backend (`NHL_HOTKEY_CAPTURE=1`, added to `nhl_vk_backend.cpp::PollHotkeyCapture`);
    driver script `scripts/_capture_gameplay.ps1`. **Use scene_04 (gameplay) as the
    primary A/B trace for all phases.**
  - For #5: an index-conversion-heavy (quad-list/HUD) trace — the HUD-heavy frames of
    the gameplay capture cover this; or capture a menu frame separately.
- **Live gameplay** — `_vkfps.ps1 -NoVsync`, read the `[nhl-vk-fps]` tap (fps + draw count).
  This is the only way to measure #4 (game-side codegen) and the end-to-end result.

Record a **Phase 0 baseline** (below) and re-run the same two measurements after every phase.

---

## Phase 0 — Baseline & branch (no code changes)

1. Create a feature branch off `master` (game) and snapshot the current SDK source
   (`E:\Tools\rexglue-sdk\src` is detached — note the current commit).
2. Build current shipping config clean; capture baselines:
   - Bench: menu frame median ms + dense `454109EC_stream.xtr` median ms.
   - Live: gameplay fps at a noted draw count (~1600) and a heavy scene (~2700).
   - Turn on **Vulkan validation layers** for the dev build — establishes a clean baseline
     so any new validation error in Phases 1–4 is attributable.
3. Confirm the deterministic bench variance is <1% (it was previously) so per-phase deltas
   are trustworthy.

Exit criteria: reproducible numbers for both measurements, zero validation errors.

### Phase 0 RESULTS (2026-06-19) — DONE

- Branch `perf/cpu-draw-submission`; SDK snapshot HEAD `bd9b519` + working-tree dangling
  commit `7c69786`.
- Added F9 streaming capture to the Vulkan-fsi backend
  (`nhl_vk_backend.cpp::PollHotkeyCapture`, `NHL_HOTKEY_CAPTURE=1`); driver
  `scripts/_capture_gameplay.ps1`. Captured a real on-ice gameplay stream →
  `gpu_trace/scene_04/454109EC_stream.xtr` (441 MB, 155 frames, **1471 draws/frame**).
- **BASELINE — `scene_04` gameplay, shipping Release runtime (win-amd64-vk-pgo,
  `rexruntime.dll`), N=6:** 260,307 draws/iter; warm ms min=3121.3 / **median=3254.8** /
  mean=3224.1 / max=3296.1 (~5% spread, A/B-grade).
  - **12.5 µs/draw** of pure CP CPU (replay bypasses guest recomp).
  - At 1471 draws/frame → **18.4 ms/frame of CP-thread cost alone → ~54 fps hard ceiling**
    from the command processor, before recomp/present. Live on dev box (RTX 4080 SUPER,
    instrumented rd runtime + OBS) was ~48 fps — consistent.
- Methodology note: do NOT compare across runtimes. The instrumented `rexruntimerd.dll`
  is ~55% slower (create-a-player 6765 ms vs Release 4244 ms historically) — bench only
  the Release `rexruntime.dll` for shipping-representative numbers.
- **All later phases re-run this exact bench (scene_04, vk-pgo Release runtime, N=6) as
  the A/B.** Target: drive 12.5 µs/draw down; every µs/draw saved ≈ 1.5 ms/frame at 1471
  draws.

---

## Phase 1 — Free wins: build flags + cvar (#4, #5)

No hot-path surgery — establishes early gains and exercises the harness.

**#4 `-mtune`:**
- Add `-mtune=<x>` to the game flag lines (`scripts/_build_vk_pgo.bat:20-22`,
  `_build_vk_pgogen.bat:20-22`, `_build_vk_opt.bat:27-29`) and SDK
  (`scripts/_ffx_sdk_configure.bat:21-23`). ISA stays `-march=x86-64-v3` → ABI-safe, no SSE4a.
- A/B three options on the dense bench + live: (a) unset/generic, (b) `-mtune=x86-64-v3`
  (clang generic-v3 model), (c) `-mtune=intel`. Keep the best **single** value for the unified
  build. (Two-SKU Intel/AMD split is a deliberate non-goal for "a build"; note it as a future
  option if the Intel delta is large.)

**#5 primitive index cache:**
- `primitive_processor_cache_min_indices` defaults to 0 (`primitive_processor.cpp:40`). Try a
  non-zero threshold; measure on the index-conversion-heavy trace (quad-list draws). Keep if a
  win, revert if the page-protection overhead still dominates. Change is a cvar default only.

Exit criteria: chosen `-mtune` value committed; cvar decision made + measured.

### Phase 1 RESULTS (2026-06-19) — DONE, both negative

Measured back-to-back on `scene_04`, shipping Release runtime (warm baseline ~3020 ms;
within-session variance ~0.5%).

- **#5 index cache — REJECTED.** Sweep of `primitive_processor_cache_min_indices`:
  0=3049 ms, 1024=3060 (noise), 4096=3237 (**+6.2%**), 32768=3223 (+5.7%). Disabled (0)
  is optimal for NHL too; the cache's lock + page-protection overhead beats its reuse.
  SDK default-0 confirmed correct. No change ships.
- **#4 `-mtune=znver3` — REJECTED (on the evidence).** Built a znver3 `rexruntime.dll`
  from identical source (ABI-safe, `-march` floor unchanged) and A/B'd vs no-mtune
  (interleaved ×2): no-mtune 3014/3023, znver3 3030/3158 — best-case mins identical
  (~0.2%, noise). The CP path is already AVX2+O3+ThinLTO and is dependency/memory-bound,
  not scheduling-bound, so `-mtune` (scheduling-only) doesn't move it. The guest recomp
  *might* benefit but the bench can't see it and a live A/B can't beat the ±6%
  cross-session variance; a single shared SKU tuned for znver3 also risks an Intel
  regression. Revisit only under a per-vendor 2-SKU split with live measurement.

**Phase 1 takeaway:** both "free" build-knob levers are empty on a representative
gameplay scene. This *confirms* the build-opt avenue (shipped in v0.2.0) is tapped out and
the real headroom is the per-draw CODE costs — exactly #1/#2/#3. Methodology lesson
applied: always A/B back-to-back same-session (cross-session drifts ~6%; same-session ~0.5%).

---

## Phase 2 — Warm-up caching in the CP (#2, #3)

Lower-risk per-draw cost reductions that validate the invalidation-hook pattern (`WriteRegister`
dirty bits, `TextureFetchConstantWritten`) that #1 also relies on. Do these before the big one.

**#2 `GetCurrentStateDescription` short-circuit:**
- Today `ConfigurePipeline` (`pipeline_cache.cpp:1048`) rebuilds the full `PipelineDescription`
  (~30 register reads + bitfield pack) *before* the `last_pipeline_` compare at line 1100.
- Add a `pipeline_state_dirty_` bit set from `WriteRegister` (`command_processor.cpp:2220`) for
  the registers feeding the description (render-pass key, depth/stencil, cull, blend, topology).
  When clean **and** the same VS/PS are bound, return `last_pipeline_` without rebuilding.
- Validation: assert (debug-only) that the short-circuited description equals a freshly-built
  one for a few thousand draws before trusting the bit.

**#3 sampler parameter caching:**
- `GetSamplerParameters` + `unordered_map::find` on `samplers_` runs per sampler per draw
  (`command_processor.cpp:3922-3965`, `texture_cache.cpp:678/765`); TODO at line 3920.
- Cache resolved sampler params keyed on (shader, fetch-constant slot); invalidate via the
  existing `TextureFetchConstantWritten` hook (`command_processor.cpp:2250`).

Bench + live after each; verify no visual regression.

Exit criteria: #2 and #3 each show a measured improvement with zero validation errors and no
visual diff vs Phase 0 screenshots.

### Phase 2 RESULTS (2026-06-19)

- **#3 sampler-param cache — IMPLEMENTED, measured SUB-NOISE on gameplay.** Clean
  same-binary A/B via the `NHL_VK_NO_SAMPLER_CACHE` opt-out (cache ON vs OFF,
  interleaved): ON 3289/3337, OFF 3358/3326 — ~1% and direction-inconsistent (ON_B >
  OFF_B), i.e. within noise. Root cause: gameplay churns textures (fetch constants)
  nearly every draw → cache invalidates → low hit rate. Helps only low-churn scenes
  (menus/create-player), which already run too fast to matter. Code is correct + low-risk
  (invalidation co-located with the proven `kConstantBufferFetch` clear); kept on the
  branch, but NOT a win. Files: `vulkan/command_processor.cpp` (sampler loop ~3922,
  invalidation 2248/2330, record after retry loop ~3982) + `command_processor.h` members.
- **#2 GetCurrentStateDescription short-circuit — NOT STARTED; recommend SKIP.** Same class
  as #3 (CPU-side micro-caching) but riskier (wide/transitive register dependency → a
  missed reg = wrong PSO). With #3/#4/#5 all proving that CPU-side micro-caching is
  noise on the draw-bound gameplay path, #2 is very likely another noise result for
  more risk.

**Phase 2 pattern (decisive):** #3 (sampler), #4 (mtune), #5 (index cache) ALL come up
noise/negative. The per-draw CP cost is NOT in the CPU-side decode/lookup micro-work — it's
in the **Vulkan descriptor/driver calls and raw command recording**. That is exactly what #1
(per-draw descriptor-set reuse: `vkUpdateDescriptorSets` + transient alloc + bind, eliminated
on unchanged bindings) targets, and #6 (threaded recording). PIVOT to #1.

---

## Phase 3 — Descriptor-set reuse (#1, the big one)

Dedicated phase — highest value, most correctness-sensitive (descriptor lifetime / wrong-texture
hazards).

- Implement the TODO at `command_processor.cpp:6736`. Today both texture descriptor sets are
  invalidated unconditionally every draw, forcing per-draw: rebuild of
  `descriptor_write_image_info_`, a transient descriptor-set allocation
  (`WriteTransientTextureBindings:6921`), a `vkUpdateDescriptorSets`, and a `vkCmdBindDescriptorSets`.
- Approach: hash (or compare against last-draw) the actual `VkImageView` + `VkSampler` handles
  feeding each set. When unchanged from the previous draw, skip the invalidate at 6737-6739 and
  reuse the already-bound set (leans on the existing
  `current_graphics_descriptor_sets_bound_up_to_date_` machinery).
- Correctness gates (mandatory):
  - Vulkan validation layers clean across a full gameplay session.
  - Visual regression vs Phase 0 screenshots — player jerseys, atlas text, HUD, crowd — using
    the existing oracle/screenshot tooling; watch the cases the residency work flagged
    (jersey numbers, equipment textures).
  - Stress: rapid camera cuts / replays where bindings churn hard.

Exit criteria: largest bench delta of the project so far, validation-clean, pixel-clean.

### Phase 3 RESULTS (2026-06-19) — DECISIVE

- **#1 implemented + PROVEN CORRECT, but perf is NOISE.** Value-based descriptor reuse
  (`vulkan/command_processor.cpp` UpdateBindings ~6758: build candidate image infos,
  compare resolved VkImageView/VkSampler handles to the previous draw, only invalidate
  changed sets; opt-out `NHL_VK_NO_TEXDESC_CACHE`). Correctness gate PASSED:
  replay-to-PNG with cache ON vs OFF is **byte-identical** (MD5 match). Perf A/B (same
  binary, ON vs OFF, interleaved): 3160/3150 vs 3170/3180 ms = **~0.6%, within noise**.
- **The decisive measurement — reuse rate (NHL_VK_TEXDESC_STATS):** vertex set **~72%**
  reused, pixel set **~51%** reused. So #1 SKIPS the transient alloc + vkUpdateDescriptorSets
  + vkCmdBindDescriptorSets on half-to-three-quarters of draws — and frame time does NOT
  move. **Therefore descriptor management is provably NOT a meaningful fraction of the
  12.5 µs/draw.**

### Q1 VERDICT (across Phases 1-3)

Four targeted optimizations of the per-draw CP path — #1 descriptor reuse, #3 sampler
decode cache, #4 -mtune, #5 index cache — ALL measure as noise/negative on the
representative gameplay scene, and #1's high reuse rate proves the biggest "obvious" target
is not the cost. **The 12.5 µs/draw is distributed across irreducible per-draw work** (PM4
decode, register translation `copy_and_swap`, system-constant updates, deferred command-
buffer recording, shared-memory residency, RequestTextures) with **no single cacheable
hotspot.** Micro-optimization of the CP path cannot win.

The two remaining levers are structural, not incremental:
1. **#6 multi-threaded command recording** — parallelize the WHOLE per-draw pipeline across
   the idle cores (the CP is single-threaded; the box has 6-8 cores). The only lever that
   can multiply throughput rather than shave per-draw ns. Major effort/risk (PM4 is a serial
   stateful stream → needs decode→work-item→parallel-secondary-record→ordered-submit).
   Now JUSTIFIED by the data (the cheap alternatives are exhausted).
2. **Draw-count reduction** — game/algorithmic, very hard (requires understanding guest
   rendering); out of scope for the SDK layer.

Status of the Phase 2/3 code: #1/#3 are correct + low-risk but perf-neutral on gameplay;
kept on the SDK working tree pending the #6 decision (revert if not pursued). The reuse-rate
counter is a keepable diagnostic.

---

## Phase 4 — Multi-threaded command recording (#6, gated)

Structural ceiling-raiser; highest effort and risk. **Gate: only proceed if Phases 1–3 have not
reached the perf target.** Land behind an env flag (`NHL_VK_MT_RECORD`, default OFF) so the
shipping build is unaffected until it's proven.

- Today one thread does PM4 decode + all Vk recording into a single `deferred_command_buffer_`
  (`command_processor.cpp:274/316`, `IssueDraw:3744`). PM4 is a serial state-interleaved stream,
  so this needs a decode → per-draw work-item → parallel secondary-command-buffer recording →
  ordered-submit split.
- Treat as a **spike with a go/no-go**: prototype on the deterministic bench first; if it doesn't
  show a clear multi-core win without hazards, defer it out of this build and ship Phases 1–3+5.

Exit criteria: either a validated multi-core win (flag flipped on by default) or a documented
defer decision. Phase 5 proceeds regardless.

### Phase 4 RESULT (2026-06-22) — DEFERRED. Premise overturned by the "do-this-first" gate.

The spike's mandatory first check — *is the CP "GPU Commands" thread actually pegged at
~100%?* — **failed**: it sits at **~59% of one core** during the scene_04 bench. It is NOT
compute-saturated; it spends ~41% of each frame **blocked on a synchronous GPU readback**.
You cannot parallelize away a thread that is waiting on the GPU, so **#6 is deferred.**

Root cause: `readback_resolve=full` (set in `nhllegacy_app.h:192`) drives
`VulkanCommandProcessor::IssueCopy_ReadbackResolvePath()`, which calls
`AwaitAllQueueOperationsCompletion()` **per resolve** (`command_processor.cpp:~4811`) — a full
GPU drain + `memcpy` to guest RAM. The drain serializes CPU↔GPU; the stall smears across all
subsequent draws as submission backpressure, which is **precisely the "distributed cost / no
hotspot / 75% rest" the rdtsc profile reported** (rdtsc counts blocked cycles). That also
explains why #1–#5 were all noise: they optimized compute, and compute was never the limit.

Same-session A/B (scene_04, N=6), via the new `NHL_VK_READBACK_MODE` runtime override:

| mode | median ms | µs/draw | speedup | CP thread CPU |
|---|---|---|---|---|
| full (shipping) | ~3850 | ~14.8 | 1.0× | 59% |
| fast (double-buffered, 1-frame stale) | ~2310 | ~8.9 | **1.66×** | — |
| some (copy on cache-miss only) | ~1680 | ~6.5 | **2.3×** | — |
| disabled (incorrect) | ~1320 | ~5.1 | 2.9× | **96%** |

→ Pure CP compute ≈ 5.1 µs/draw; the kFull readback adds ~7–10 µs/draw of pure GPU-sync
stall. `kFast`/`kSome` (already in the SDK) eliminate the per-resolve drain via a double-
buffered readback. **This is the real lever** — a one-line default change worth ~1.4–2.3×,
versus #6's high-risk attempt to parallelize the 5.1 µs/draw that isn't even the bottleneck.

Gate before shipping the switch: a **live** equipment/jersey correctness check (the deterministic
replay PNG is byte-identical across full/fast/some, but live reads a genuinely 1-frame-stale
resolve, and `full` was chosen for the runtime-composited equipment textures). Test live with
`NHL_VK_READBACK_MODE=fast` then `some`; if clean, flip `nhllegacy_app.h:192` and re-PGO.

---

## Phase 5 — Re-PGO, ThinLTO, package the unified build

The hot path changed, so the existing `pgo/nhllegacy.profdata` is stale.

1. Final SDK: Release + ThinLTO, rebuilt+installed with all accepted SDK changes (#1/#2/#3/#5,
   and #6 if it landed).
2. Game PGO recapture cycle: `_build_vk_pgogen.bat` (instrument) → play a 3–5 min gameplay
   session (with the new `-mtune`) → `llvm-profdata merge` → `_build_vk_pgo.bat` (use).
3. Package via `release/package.ps1 -Preset win-amd64-vk-pgo` (auto `-SkipBuild`); ensure
   `amd_fidelityfx_vk.dll` at top level.
4. Final A/B vs Phase 0: deterministic bench + live gameplay on AMD (and Intel if a box is
   available) at matched draw counts. Report combined delta and frame-time variance.

Exit criteria: one packaged build, combined measured improvement vs Phase 0, no regressions.

---

## Per-draw CP profile (2026-06-21) — DEFINITIVE: cost is distributed, no hotspot

Instrumented `VulkanCommandProcessor::IssueDraw` with scoped timers (env `NHL_VK_CP_PROFILE`;
rdtsc-based after finding chrono::now ≈360ns/probe contaminated the first pass) and ran the
`scene_04` bench. Total IssueDraw ≈ 10.3 µs/draw (83% of the 12.5 µs/draw; PM4/register
processing is the other ~17%). Breakdown as % of IssueDraw:

  shader 5.6 · bindings 5.3 · reqtex 4.5 · samplers 1.6 · predraw 1.4 · rtcache 1.2 ·
  prim 1.1 · sysconst 1.0 · pipeline 0.9 · entry 0.8 · midstate 0.4 · draw 0.3 ·
  plhandle 0.2 · vstype 0.1   → named ops sum to ~24%;  **rest = ~75%**

Bracketed essentially the whole function; the 75% is NOT one block — it's the hundreds of
small ops between the major calls (register-file reads, draw_util helpers, state computation,
dynamic-state diffing, residency checks). Both chrono and rdtsc agree (named ~16-24%, rest
~75-84%). **No hotspot exists** — which is exactly why #1-#5 (each a 1-6% slice) were noise.
The only engine lever is #6 (parallelize the distributed work across idle cores); the
distribution is the argument FOR #6 (no serial hotspot blocking parallelism). Profiling
instrumentation is throwaway (gated, default-off) — revert with #1/#3 if #6 not pursued.

## Risk register

- **#1 wrong-texture hazard** — caught by validation layers + jersey/atlas visual diff.
- **#2 stale pipeline** — a missed register in the dirty-bit mask binds the wrong PSO; debug
  assert (built description == short-circuit) before trusting.
- **#3 stale sampler** — missed fetch-constant invalidation; same assert pattern.
- **#6 ordering/hazard bugs** — gated behind a default-off flag; spike-first.
- **PGO portability** — control-flow profile is host-CPU-independent; capture once after all
  code changes, not between phases.
- **Build coupling** — every SDK change needs install + game relink; only Phase 5 needs PGO.

## Note on expectations

Individually these are single-digit-to-modest percent (#1 the wildcard); stacked they raise the
single-thread ceiling but will not by themselves convert a 10–20 fps report into 60 — that gap
(vs the dev's 57–65 fps native) is being diagnosed separately from logs (GPU selected, build
version, `draw_resolution_scale`, draw count).
