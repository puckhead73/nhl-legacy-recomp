# Handoff: #6 — Multi-threaded command-processor recording (Vulkan)

> ## ⛔ SPIKE RESULT (2026-06-22) — #6 is a NO-GO; the bottleneck is NOT CPU compute
>
> The handoff's "do this FIRST" gate (is the CP thread pegged at ~100%?) **failed**,
> and chasing why overturned the whole premise. **The CP "GPU Commands" worker thread
> is only ~59% of one core during the bench — it is NOT compute-saturated. It spends
> ~40% of every frame BLOCKED on a synchronous GPU readback**, not running code.
> Parallelizing CPU work across cores (#6) cannot speed up a thread that is waiting on
> the GPU. **#6 is deferred. The real lever is the resolve-readback sync.**
>
> ### The evidence (all same-session, back-to-back; clean pristine `IssueDraw`)
>
> | Config | bench median ms (scene_04, N=6) | CP worker-thread CPU |
> |---|---|---|
> | `readback_resolve=full` (shipping/correct) | ~3770–3930 | **59% of one core** (blocked ~41%) |
> | `readback_resolve=fast` (delayed, double-buffered) | ~2250–2380 → **1.6–1.75× faster** | — |
> | `readback_resolve=some` (copy only on cache miss) | ~1680 → **2.3× faster** | — |
> | readback disabled (`NHL_VK_NO_READBACK`, INCORRECT) | ~1320 → 2.9× faster | **96% of one core** (saturated) |
>
> ### Root cause
> `VulkanCommandProcessor::IssueCopy_ReadbackResolvePath()` in
> `src/graphics/vulkan/command_processor.cpp` calls **`AwaitAllQueueOperationsCompletion()`
> per resolve** in `kFull` mode (line ~4811) — a full GPU drain, then a `memcpy` of the
> resolved EDRAM back to guest RAM. Each drain serializes CPU↔GPU, so the stall smears
> across *all* subsequent draws as submission backpressure. **This is exactly the
> "distributed cost, no hotspot, 75% rest" the old rdtsc profiler saw** (rdtsc keeps
> counting while the thread is blocked → attributes GPU-wait to whatever bracket is open).
> `#1–#5` measured as noise because they optimized *compute*, which was never the limit.
>
> `kFast`/`kSome` (already in the SDK) avoid the drain via a double-buffered readback that
> reads the *previous* frame's resolve (1 frame stale) — no per-resolve GPU sync.
>
> ### What shipped from the spike
> - Pristine `IssueDraw` restored (profiler + #1/#3 reverted to `7c69786`); clean SDK
>   Release `rexruntime.dll` rebuilt; baseline reconfirmed (median ~3042 ms) + serial
>   reference PNG md5 `37601B45F9ED65AB4EDF2C5C9E08E4BF`.
> - **New runtime knob `NHL_VK_READBACK_MODE={none,fast,some,full}`** added to
>   `CommandProcessor::GetReadbackResolveMode` (`src/graphics/command_processor.cpp`) —
>   overrides the configured mode live, in both bench and live gameplay. Default unset =
>   no change (uses the app's `full`). This is the test vehicle AND the eventual lever.
> - Bench/sample drivers: `scripts/_cpbench.ps1`, `scripts/_cpusample.ps1`,
>   `scripts/_replaypng.ps1`.
> - **Replay PNGs for `full`/`fast`/`some` are all byte-identical** (`37601B45…`) — the
>   deterministic correctness gate passes (replay re-runs identical PM4, so "previous
>   frame" == current frame). Proves the modes don't corrupt; does NOT prove *live*
>   correctness (live reads a genuinely 1-frame-stale resolve).
>
> ### RESOLUTION (2026-06-22, validated live) — size-gated readback
> `fast`/`some` (1-frame-stale on ALL resolves) DID regress equipment live (helmets, goalie
> gear) — confirmed. But the stall is needed only for the small runtime-COMPOSITED textures,
> not the large framebuffer resolves. Instrumentation (`NHL_VK_RB_STATS`): scene_04 has **17
> unique resolve dests, ~14.5 readbacks/frame**; the bulk are **3.6 MB** framebuffers
> (1280×720×4, ~7/frame, GPU-consumed for present, never CPU-read), while the composites are
> small (player helmet = a **960 KB** resolve, goalie = a **2.3 MB**-class resolve).
> Added **`NHL_VK_READBACK_MAX_LEN`** (max bytes still read back; 0 = off) in
> `IssueCopy_ReadbackResolvePath` — skips the CPU readback + per-resolve drain above the
> threshold. The GPU-side resolve (`shared_memory` copy + `MarkRangeAsResolved`, runs first in
> `render_target_cache_->Resolve`) still happens, so GPU consumers are unaffected and the
> kept composites get a FULL same-frame readback (zero staleness).
>
> Live results: `262144` (≤256 K) → helmets broke; `1048576` (≤1 M) → helmets OK, goalies
> broke; **`3000000` (≤3 M, skips only the 3.6 MB framebuffers) → helmets + goalies BOTH
> correct.** Bench (scene_04, N=6): full ~3106 ms / ≤3 M ~2334 ms = **~1.33×** (the entire
> win is the framebuffer skip; the 2.3 MB resolves at 0.13/frame cost ~nothing, so ≤3 M keeps
> maximal readback at no perf cost — the safe choice).
>
> SHIP: game-side default `NHL_VK_READBACK_MAX_LEN=3000000` (next to `readback_resolve="full"`,
> `nhllegacy_app.h`) + re-PGO; validate in a 2nd arena first (size is a proxy — clean here
> because composites ≤2.3 M < framebuffers 3.6 M). AIRTIGHT follow-up (size-independent): mark
> a resolve dest "needs readback" when a texture is later uploaded from its range
> (resolve→RAM→texture round-trip = composite; framebuffers never round-trip).
>
> ### SHIPPED + Phase B (2026-06-22)
> - **Phase A SHIPPED.** Game default `_putenv_s("NHL_VK_READBACK_MAX_LEN","3000000")` in
>   `nhllegacy_app.h`; SDK rebuilt+installed; game PGO relinked (existing profdata still valid —
>   the readback fix doesn't change the game's hot path). Verified the shipped exe auto-applies
>   the gate (no env; log `size gate 3000000 B`; bench 2756 vs 3744 full = ~1.36×). Helmets +
>   goalies correct live (the goalie composite is a ≤3 MB / 2.3 MB-class resolve). Kept knobs:
>   `NHL_VK_READBACK_MODE`, `NHL_VK_RB_STATS` (per-dest size dump).
> - **Task 1 (multi-scene validation): PASS.** `NHL_VK_RB_STATS` across scene_00 (menu),
>   01 (intro), 02/03 (create-player), 04 (gameplay) — identical in every scene:
>   `largest_kept=2304 KB, smallest_skipped=3600 KB`. The 3 MB threshold sits in a stable
>   2.3 MB→3.6 MB gap with nothing in it (~700/600 KB margin). Gate robust across content.
>   (A different *arena*/replay still warrants a glance, but dest sizes are set by render
>   resolution + fixed equipment-atlas sizes, not by arena.)
> - **Task 2 (size-independent classifier): REFUTED + reverted.** Built a "skip readback iff the
>   resolve dest is sampled as a texture by the GPU" classifier in shadow mode. Measurement
>   killed it: **15 of 17 resolve dests are GPU-sampled, INCLUDING the equipment composites**
>   (the GPU samples them *and* the CPU tints them), so `gpu_sampled` can't separate composites
>   from framebuffers — using it would skip the equipment readback and regress it. The size gate
>   works precisely because it keys on the real distinguishing property (size). The only truly
>   airtight alternative is CPU-read-fault demand readback (page-protect skipped dests, sync on
>   guest read) — invasive, tangles with the shared-memory write-watch system; NOT pursued given
>   the gate is validated. `#6` MT-recording remains DEFERRED.
>
> Everything below is the ORIGINAL #6 handoff, kept for context. Its core premise
> (single-thread CPU-saturated) is now known to be false for the shipping config.

---

> New-session prompt + full context. Goal: raise the engine's CPU-bound framerate
> ceiling by parallelizing the single-threaded command processor across the idle
> cores. This is the ONLY engine-side lever left after an exhaustive investigation
> proved the per-draw cost has no hotspot. Read the "Hard truth" section before
> writing any code — it changes the approach.

## TL;DR

- The recomp is **draw-submission/CPU-bound at ~12.5 µs/draw** (~1471 draws/frame in
  gameplay → a ~54 fps ceiling from the command-processor thread alone). Confirmed on the
  dev box AND on clean user hardware (an RTX 3070 user: 460 fps at 50 draws, ~40 fps at
  1700 draws — perfectly draw-scaled).
- Micro-optimization **cannot** help: profiling proved the per-draw cost is **distributed
  uniformly** across hundreds of small operations with **no hotspot** (the "obvious" targets
  sum to ~24% of `IssueDraw`; ~75% is scattered register reads / `draw_util` helpers / cache
  lookups / state computation). Four targeted opts (#1 descriptor reuse, #3 sampler cache,
  #4 `-mtune`, #5 index cache) all measured as noise — #1 even reused descriptors on 51-72%
  of draws with zero frame-time change.
- The command processor runs on **ONE thread** while the user's CPU has 6-8 idle cores.
  Parallelizing it is the only remaining engine lever. **But** the cost lives in *stateful*
  per-draw work (mutating non-thread-safe caches), not in the trivially-parallel command
  encoding — so this is hard. First task is a **spike to measure the achievable ceiling**,
  not a full build-out.

## Why we're here (the investigation, condensed)

Full record: `docs/cpu-perf-optimization-plan.md` (Phases 0-3 + the profile section) and the
memory note `vk-perf-profile-findings`. Key results, all measured on the deterministic bench
over a real-gameplay trace (`scene_04`, 1471 draws/frame):

| Attempt | Result |
|---|---|
| #5 primitive index cache | −6% (rejected) |
| #4 `-mtune=znver3` | noise (CP is memory/dependency-bound, not scheduling-bound) |
| #3 sampler-param cache | noise (texture churn → low hit rate) |
| #1 descriptor-set reuse | correct (byte-identical replay) + 51-72% reuse, yet **noise** |
| CP per-draw profile | named ops ~24%, **rest ~75% = distributed, no hotspot** |

The #1 result is the clincher: eliminating the transient-alloc + `vkUpdateDescriptorSets` +
rebind on most draws changed nothing → that work isn't the cost. The cost is everywhere.

## Hard truth — read before coding

The textbook parallelization is "decode the PM4 serially, then record Vulkan commands in
parallel into secondary command buffers." **That does not work here as-is**, because the
profile shows the split is upside-down:

- The **cheap** part is the actual command *encoding* — the `draw` stage (`SubmitBarriers`
  + `CmdVkBindIndexBuffer` + `CmdVkDraw/DrawIndexed`) is **0.3%** of `IssueDraw`.
- The **expensive** part is the *stateful resolution*: `RequestTextures` (texture cache),
  `RenderTargetCache::Update`, `ConfigurePipeline` (pipeline cache), `UpdateBindings`
  (descriptor allocators), shader modification/translation, plus the ~75% of distributed
  register/state work. **All of this mutates shared, non-thread-safe caches** and is
  inherently order-dependent (state accumulates across the PM4 stream).

So naively fanning out the encoding parallelizes ~0.3% → no win. A real win requires
parallelizing the *stateful* work, which means either (a) making the texture/RT/pipeline
caches + descriptor allocators thread-safe / shardable, or (b) a software-pipeline where the
serial cache-resolution stage is NOT the bottleneck — and right now it IS the bottleneck.

**This is why the realistic ceiling is uncertain and the first deliverable is a spike, not
a feature.** Be honest in the go/no-go: if the parallelizable fraction is small, say so.

## The codebase

- **The one CP thread:** `E:\Tools\rexglue-sdk\src\src\graphics\command_processor.cpp`
  — `worker_thread_` created ~line 140 ("GPU Commands"), `WorkerThreadMain` ~274,
  `ExecutePrimaryBuffer` ~316 walks the PM4 ring; every type-3 DRAW packet calls `IssueDraw`
  inline. All recording goes into one `deferred_command_buffer_` (one primary VkCommandBuffer
  per submission). The only other threads are async pipeline *compilation* (they don't record).
- **The per-draw work:** `E:\Tools\rexglue-sdk\src\src\graphics\vulkan\command_processor.cpp`
  — `VulkanCommandProcessor::IssueDraw` (~3748). The stateful caches it drives:
  `texture_cache_`, `render_target_cache_`, `pipeline_cache_`, the transient descriptor
  allocators, `shared_memory_`, `primitive_processor_`. None are thread-safe.
- **Our hook layer:** `e:\Repositories\nhl-legacy-recomp\renderer\core\nhl_vk_backend.cpp`
  — `NhlVkCommandProcessor` subclasses the SDK CP and overrides `IssueDraw`/`IssueSwap`. This
  is where our F9 capture + fps tap live; a good place for an `NHL_VK_MT_RECORD` flag.
- Note: upstream Xenia (which this SDK derives from) is also single-CP-thread — there is no
  upstream MT-recording design to port. This is original work.

## Environment & build

- **Game branch:** `perf/cpu-draw-submission`. **SDK** (`E:\Tools\rexglue-sdk\src`) is
  detached at `bd9b519` + uncommitted changes; pre-Phase-2/3 snapshot = dangling commit
  `7c69786` (use `git diff 7c69786 -- src/graphics/vulkan/command_processor.*` to see exactly
  what Phase 2/3 added).
- **Build the SDK Release runtime:** `scripts/_sdk_build_rexruntime.bat` → writes
  `E:\Tools\rexglue-sdk\src\out\win-amd64\Release\rexruntime.dll`. To bench it, copy that DLL
  over `out/build/win-amd64-vk-pgo/rexruntime.dll` (back up the shipping one first), run, then
  restore. (The canonical `scripts/_ffx_sdk_configure.bat` BREAKS under `cmd /c` — its
  `::`-comments contain parens; use a `REM`-comment helper like `_sdk_build_rexruntime.bat`.)
- **Game data root arg (required):** `--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"`.
- Dev box: Ryzen 7 5800X (Zen 3, 8c/16t), RTX 4080 SUPER. GPU auto-selected correctly.

## Measurement harness (use for every #6 A/B)

- **Deterministic CP bench** (the primary A/B — isolates the CP, bypasses guest recomp):
  `NHL_REPLAY_BENCH=4` + `NHL_REPLAY_XTR=gpu_trace\scene_04\454109EC_stream.xtr` +
  `NHL_VK_BACKEND=1` + `NHL_VK_NO_VSYNC=1`. Logs `[nhl-bench] ... warm ms median=...` over a
  260,307-draw replay (real gameplay, 1471 draws/frame). Baseline median ~3.0-3.5 s/iter
  (≈11-13 µs/draw). **Cross-session variance is ~6%; same-session back-to-back is ~0.5% —
  always A/B in one session, never against a stored number.**
- **Correctness gate (mandatory):** replay-to-PNG — same env but **without** `NHL_REPLAY_BENCH`
  → writes `replay_frame.png` next to the exe. The MT path's PNG must be **byte-identical**
  (MD5) to the serial path's. Any diff = a race/ordering bug.
- **Per-stage profiler** (rdtsc, env `NHL_VK_CP_PROFILE`): already wired in `IssueDraw`
  (search `NHL_VK_CP_PROFILE` / `CpTotalScope` / `cp_add`). Useful to re-measure the
  parallelizable fraction. **Revert it (and the perf-neutral #1/#3) before building #6** so
  you start from a clean `IssueDraw` — see Cleanup.
- **CPU utilization check (do this FIRST):** confirm the CP "GPU Commands" thread is actually
  pegged at ~100% of one core during the bench. If it's NOT (e.g. stalling on memory or a
  lock), the bottleneck isn't raw single-thread compute and parallelism may not be the fix —
  investigate that before committing.

## Suggested plan for the new session

1. **Cleanup (clean baseline):** revert the throwaway profiler and the perf-neutral #1/#3 from
   the SDK tree (see Cleanup), rebuild, confirm the bench baseline + a byte-identical replay PNG.
2. **Spike / feasibility (the real first deliverable):**
   - Confirm the CP thread is single-core-bound at 100% during the bench.
   - Re-measure (via `NHL_VK_CP_PROFILE`) what fraction of `IssueDraw` is *pure encoding*
     (parallelizable without touching shared caches) vs *stateful resolution*. Expect the pure
     part to be small (~the 0.3% `draw` stage). Quantify it — this is the naive-approach ceiling.
   - Evaluate the real options and pick one to prototype:
     - **(A) Shard/thread-safe the caches** so cache resolution can run concurrently. Highest
       payoff, highest risk (texture/RT/pipeline caches + descriptor allocators).
     - **(B) Software pipeline** (decode → resolve → encode on separate threads). Only helps if
       resolve isn't the sole bottleneck — measure first.
     - **(C) Per-draw state snapshot → parallel resolve+encode into secondary command buffers,
       with cache mutations funneled through a lock or a per-thread cache shard merged at
       submit.** The general form of the textbook approach, adapted for the stateful caches.
   - Prototype a **2-thread** version of the chosen option, measure on the bench. **Go/no-go.**
3. **Build-out (only if the spike shows a real win):** scale to N threads, gate behind
   `NHL_VK_MT_RECORD` (default OFF), validate.

## Success criteria

- Measurable reduction in `scene_04` bench median ms (the whole point).
- **Byte-identical** replay PNG vs the serial path (correctness — non-negotiable).
- Zero Vulkan validation-layer errors across a full gameplay session.
- Gated behind `NHL_VK_MT_RECORD`, default OFF until proven; serial path stays the default.
- An honest write-up of the achieved speedup and the ceiling (incl. a clean "not worth it"
  if the spike shows the parallelizable fraction is too small).

## Cleanup checklist (do as step 1)

All uncommitted in `E:\Tools\rexglue-sdk\src` (`vulkan/command_processor.cpp` + `.h`). Revert
these to get a pristine `IssueDraw`; the pre-existing GPU-selector/VP6 fixes elsewhere in the
tree must be KEPT (diff against `7c69786` to isolate):
- **Profiler (throwaway):** the `namespace { ... CpStage ... CpTotalScope ... }` block before
  `IssueDraw`, the `cp_t()`/`cp_add()`/`_cp_t_*` brackets throughout `IssueDraw`, the
  `<chrono>`/`<cstdio>` includes if now unused, and `NHL_VK_CP_PROFILE`.
- **#1 (perf-neutral, optional revert):** `NHL_VK_NO_TEXDESC_CACHE`, `cached_texture_image_info_*`,
  `texdesc_*`, the rewritten texture-descriptor section in `UpdateBindings`, the reuse-rate
  counter. Correct but no gameplay benefit.
- **#3 (perf-neutral, optional revert):** `NHL_VK_NO_SAMPLER_CACHE`, `current_samplers_up_to_date_`,
  `current_samplers_*_shader_`, `sampler_cache_enabled_`, the two `current_samplers_up_to_date_ =
  false` invalidations (lines ~2248/2330), the sampler-loop guard.
- **KEEP:** the F9 gameplay-capture tooling in `renderer/core/nhl_vk_backend.cpp` /
  `scripts/_capture_gameplay.ps1` (a genuine asset — it's how `scene_04` was captured).

## Parallel track worth flagging (not part of #6)

The investigation also found that **most user "slow" reports are config/environment, not the
engine**: one log showed 3× supersampling + experimental FSR3 + an Overwolf/OBS/RTSS hook
stack (7.5 fps); the clean case hit the genuine ~40 fps ceiling. High-leverage, low-effort
user-facing wins independent of #6: don't default to experimental FSR3, sanity-cap/warn the
supersampling slider, and surface capture-overlay overhead. These help the majority and could
ship while #6 is being spiked.
