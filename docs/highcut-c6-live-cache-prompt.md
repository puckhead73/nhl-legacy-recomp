# Kickoff — C-6 live feed: the draw/PIPELINE cache (make live rendering performant)

**Use this as the opening prompt for a fresh session.** It targets ONE thing: make the C-6 LIVE FEED
fast enough to be usable by **caching pipelines (and ideally textures) across frames** instead of
rebuilding every draw from scratch every frame. Background lives in auto-memory
`highcut-composition-resolve-hostcopy` (read its CURRENT-STATE header + the "C-6 LIVE TEST #1 RESULT"
line first) and `highcut-c-plume-renderer`.

---

## Where we are (don't re-derive — it's committed and proven)

C-6 "live feed" replaces the disk `.bin` intermediary with an in-memory hand-off so plume renders the
**live** game (not a replayed capture). It is **built, committed, and gated `NHL_HIGHCUT_LIVE_FEED`**.

**Live test #1 (user F10 on a running game) proved the two hardest unknowns work:**
- **Co-run is stable** — beta-takeover (D3D12) + plume-present (Vulkan) in ONE process, NO
  `0x887A`/TDR/device-removed. (This was the plan's #1 risk. Cleared.)
- **Transport works** — "loaded N live-fed owned draws" + "presented plume frame": the bridge delivers
  a frame and the plume thread rebuilds + renders it.

**The blocker that remains = PERFORMANCE.** The consumer (`RenderClear` → `LoadC5Frames` live path)
**rebuilds every draw's pipeline from scratch every committed frame**. On the first FULL gameplay frame
(300+ draws) that's a multi-second plume-thread stall → looks like a hang/"crash" (it isn't a crash —
no exception logged; the GPU is fence-serialized, so it's not a lifetime race, just raw rebuild cost).

(There's a SEPARATE minor cosmetic issue — live test #1's first committed frame was a 5-draw HUD-only
partial frame because F10 armed mid-frame. Real gameplay frames commit full. Don't chase this first;
it's timing, not the perf wall.)

## The goal

Cache the expensive per-draw GPU objects across frames so a committed frame only rebuilds what actually
changed. Target: live rendering runs at interactive frame rate (tens of fps), not one-frame-then-stall.

## What's expensive (measure, but this is the strong prior)

In `BuildRenderableDraw` (gpu/hooks/plume_present.cpp, ~line 800) each draw creates, every frame:
1. **`createShader` ×2** (VS+PS SPIR-V) and **`createGraphicsPipeline`** — *the big cost* (shader
   compile + PSO build). These depend ONLY on the shader bytes + render state, NOT on per-frame
   constants/vertices.
2. Vertex/index/constant **buffers** + **descriptor sets** + **texture uploads** (untiled blobs → GPU).
   Cheaper than PSO creation, but texture uploads for static art (rink/boards/crowd) re-uploading every
   frame is also wasteful.

So most draws' *packets* change every frame (view/proj/bone constants, dynamic verts) → a naive
whole-packet hash cache has a LOW hit rate. **Cache the PIPELINE, keyed by what it actually depends on.**

## Design (recommended, layered — do layer 1 first, verify, then decide on 2/3)

**Layer 1 — PIPELINE/shader cache (the main win).**
- Key = hash of (VS SPIR-V bytes, PS SPIR-V bytes, render state: blend src/dst/op + alpha, color write
  mask, topology, depth enable/write/func, stencil enable/ref/masks/ops, cull/winding, and the RT
  formats the pipeline declares). All of these are in the packet header (`hdr`) the consumer already
  parses. The SPIR-V bytes are in the packet too.
- Cache `{RenderShader vs, RenderShader ps, RenderPipelineLayout, RenderPipeline}` in a
  `std::unordered_map<uint64_t, CachedPipeline>` that lives in `PlumeCtx` (persists across frames).
- In `BuildRenderableDraw`: compute the key; on hit, point `d.vs/d.ps/d.layout/d.pipeline` at the cached
  objects (they're shared — make `RenderableDraw` hold non-owning pointers OR shared_ptr for these, and
  keep the OWNED per-draw stuff — buffers, descriptor sets, textures — as before). On miss, build them
  and insert.
- Still rebuild per-draw buffers + descriptor sets + texture uploads each frame (layer 1 leaves these).

**Layer 2 (if still slow) — TEXTURE cache.** Key = (guest base addr, dims, format, content hash of the
untiled blob). Reuse the uploaded `RenderTexture` + view across frames; static art hits, dynamic
(players' skinned composites) miss. Watch: composition re-points some texture slots (host-copy depth,
HUD scene) — those bindings are set per-frame in the render loop, independent of the upload cache.

**Layer 3 (optional) — full RenderableDraw cache** for draws whose ENTIRE packet is byte-identical
frame-to-frame (fully static). Marginal once 1+2 exist.

## Lifetime + eviction (important)

- The plume thread fence-waits each frame (`waitForCommandFence`, PlumeThreadMain ~line 1964), so the
  GPU is idle between frames — destroying a cached object during the rebuild is safe. But DON'T destroy
  cached pipelines/textures that this frame still uses.
- The live reload currently `c.c5draws.clear()` + clears `c5surfaces`/`surfaceDims`/`sampledSrcOrder`/
  `resolveMap` (LoadC5Frames live branch). With a cache, the cache map must SURVIVE the clear — only
  `c.c5draws` (the per-frame ordered list) is rebuilt; the pipeline/texture caches persist.
- Eviction: tag each cache entry with the last frame (`c.frame` / `g_liveSeq`) it was used; after the
  rebuild, drop entries not used for ~N frames (bound memory — a long game touches many shaders).

## Key files / seam (all already in place from C-6 foundation)

- `gpu/hooks/plume_present.cpp`:
  - `struct RenderableDraw` (~225) — holds `layout/pipeline/vs/ps/set0..3/buffers/textures`. Split into
    "cacheable (pipeline/shaders)" vs "per-frame (buffers/sets/textures)".
  - `BuildRenderableDraw` (~800) — where shaders+pipeline+buffers+sets+textures are created; add the
    cache lookups here. `hdr` (packet header) carries all the state for the pipeline key.
  - `LoadC5Frames` (~1222) live branch — clears `c.c5draws` + derived state each live frame; make the
    cache maps persist.
  - `RenderClear` live-reload (~1443) — swaps `g_livePending` under `g_liveMutex` on `g_liveSeq` change,
    calls `LoadC5Frames(c, &draws, &resolves)`.
  - `PlumeThreadMain` (~1994) — per guest Present → `RenderClear`; the fence wait is ~1964.
  - bridge globals (~349): `g_liveMutex / g_liveBuild / g_livePendingDraws / g_livePendingResolves /
    g_liveSeq`; `HighcutLivePushDraw` / `HighcutLiveCommitFrame` (~2010).
- `renderer/core/nhl_command_processor.cpp` (producer, no changes expected for the cache): packet build +
  `HighcutLivePushDraw` (~2553); frame-boundary `HighcutLiveCommitFrame` (~2491); `live_feed` flag
  (~2484, requires `NHL_HIGHCUT_FRAME_CAPTURE`).
- `gpu/hooks/highcut_draw_packet.h` — packet header layout (the pipeline-key fields).

## Test / verify (inherently LIVE — needs F10 on a running game; can't headless-verify the co-run)

- `_live.ps1` — co-runs beta-takeover + plume-present + live feed. Launch, drive to LIVE GAMEPLAY,
  press F10, watch the "NHL high-cut (plume Vulkan)" window.
- SUCCESS = the window tracks the live game smoothly (tens of fps), not one-frame-then-stall.
- Add a per-frame timing log in `LoadC5Frames` live path: draws total, cache hits vs misses, and ms to
  rebuild. First full frame should show mostly-misses; subsequent frames mostly-hits (static art) → low
  ms. That's the proof.
- The DISK replay path (`_c5render.ps1`, `_audit.ps1`) must stay byte-identical — regression-check it
  headlessly after the change (it shares `BuildRenderableDraw`/`LoadC5Frames`).

## Scope guardrails

- This is the **perf cache** thread only. Don't chase the 5-draw black-first-frame (F10 timing) yet, and
  don't add the snapshot-composite (live composite samples plume's own scene) yet — both are separate,
  post-perf. Equipment/shadows/HUD (C-5l/m/n) are DONE and apply to the live path unchanged.
- Keep it gated under the existing `NHL_HIGHCUT_LIVE_FEED`; the disk capture/replay path must not change.
- Commit the working `NHL_HIGHCUT_LIVE_FEED` foundation is already in (see `git log` C-6); build on it.
