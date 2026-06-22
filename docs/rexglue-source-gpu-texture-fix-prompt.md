# Kickoff prompt — fix the GPU-built equipment texture by patching rexglue source

**Use this as the opening prompt for a new session.** It targets ONE thing: getting the
correct **GPU-built equipment texture** (the goalie pad diffuse, "slot1") into the high-cut
plume renderer, which is blocked by rexglue's SDK API and likely needs a **rexglue source-code
patch**. Background lives in the auto-memory: `highcut-green-equipment-is-capture-residency`
(read its CURRENT-STATE TL;DR first) and `nhl12-decomp-bc5-normal-leads`.

---

## Goal

The high-cut plume goalie renders MOSTLY clean now, except **`slot1` — the pad diffuse
(BC1 1024×512)** — which stays green/striped. Get the *correct* texels for that texture into
the high-cut packet, by understanding and (if needed) patching how rexglue's texture cache
holds it. Success = the goalie pads render clean (and ideally team-tinted) in the plume replay.

## What's already known (don't re-derive — see memory for the full chain)

- The high-cut capture (`NHL_BETA_TAKEOVER=1`, `_c5dump.ps1`) builds per-draw packets and reads
  each texture from **guest RAM** (`memory_->TranslatePhysical`). For RUNTIME-BUILT equipment
  textures the guest RAM is stale/garbage at capture time.
- We added a GPU readback path (`NHL_HIGHCUT_GPU_TEX=1`, method
  `ReadbackActiveTextureRGBA` in `renderer/core/nhl_command_processor.cpp`): writes rexglue's
  SRV via `WriteActiveTextureBindfulSRV`, samples it with a compute shader, copies to a readback
  buffer. It **fixed every equipment texture EXCEPT slot1.**
- Why slot1 resists: during the **takeover capture**, our beta path drives
  `RequestTextures`, which **untiles slot1 from the corrupt guest RAM into the cache** — so the
  cache's slot1 is *itself* corrupt during takeover, and the readback faithfully returns it. The
  CLEAN slot1 exists only in **native (non-takeover) rexglue rendering**, where rexglue binds it
  as a **GPU-built resource it never untiles from guest RAM**.
- API wall: `class D3D12TextureCache final` — can't subclass to hook
  `LoadTextureDataFromResidentMemoryImpl`; no public accessor for an active texture's
  `ID3D12Resource`; no readback/dump method. (Verified on latest `main`, 0.8.1.32 — no upstream
  fix; rexglue source is public + BSD-3.)

## THE decisive question to answer FIRST (before any patch)

**How does rexglue populate the cache's slot1 resource correctly during NATIVE rendering, and
why is it corrupt during our takeover?** Hypothesis (strong, but VERIFY in source): slot1 is a
GPU-built texture (render-to-texture / EDRAM resolve / memexport) — rexglue keeps the correct
data in its D3D12 resource and never round-trips it through guest RAM, while our takeover forces
a guest-RAM untile that clobbers it. (We confirmed slot1 is NOT in our resolve sidecar, but that
only tracks one resolve hook — there are other mechanisms.)

Resolve this by reading the rexglue SOURCE (github.com/rexglue/rexglue-sdk, `src/graphics/`):
1. `src/graphics/pipeline/texture/cache.cpp` + `src/graphics/d3d12/texture_cache.cpp` —
   `LoadTextureDataFromResidentMemoryImpl`, residency/dirty tracking, when it untiles from guest
   memory vs reuses a resident resource, and how `MarkRangeAsResolved` / invalidation interacts.
2. The **resolve** path (`render_target_cache` / EDRAM resolve → texture memory): does a resolve
   to slot1's address write GUEST RAM, update the cache resource, or both? Is there a resolve
   our high-cut capture doesn't hook?
3. **memexport** handling — does the game build the composite via memexport (GPU→memory)?
4. Cross-check with the NHL12 sibling RE (`docs/nhl12-decomp-reference/`,
   `graphics/textures-and-shaders-feeding.md` + `re/equipment-recolor/`): they describe the
   equipment recolor as runtime-generated/in-shader; reconcile with what you find in source.

The answer picks the fix:
- **If the cache resource IS correct during takeover** (and our readback reads the wrong thing)
  → simpler: a small source patch to expose the resource / a clean readback, no behavior change.
- **If the cache resource is corrupt during takeover** (our forced untile clobbers a GPU-built
  resource) → the patch must make the takeover path NOT re-untile that texture (replicate
  native's GPU-built handling), OR capture the texture from a native frame.

## Candidate fixes (rank after the investigation)

1. **Minimal source patch to extract the texture** — build rexglue from source, add a public
   `ID3D12Resource* GetActiveTextureResource(fetch_constant)` (or a `ReadActiveTextureToBuffer`)
   and/or drop `final`. Lets the high-cut grab the GPU-correct texture cleanly. *Only sufficient
   if the cache resource is correct during takeover.*
2. **Don't re-untile GPU-built textures during takeover** — patch the cache so a texture rexglue
   knows is GPU-resident/resolved isn't re-untiled from guest RAM by our `RequestTextures`. This
   preserves the clean resource so readback works.
3. **Capture equipment textures from a native (non-takeover) frame** and feed them to the replay
   out-of-band (keyed by base addr or content) — avoids the takeover-corruption entirely. May be
   simpler than a source patch if (1)/(2) prove deep.

## Build-from-source logistics

We currently consume rexglue as a PREBUILT SDK (headers+libs) at `E:\Tools\rexglue-sdk\`
(`0.8.0`, `0.8.1.31-dev`); nhl-legacy-recomp finds it via `CMAKE_PREFIX_PATH` (see memory
`rexglue-sdk-location-license` and `rexglue-build-environment`). To patch:
1. Clone `github.com/rexglue/rexglue-sdk` at/near tag `0.8.1.31`/`.32` (match our version to
   avoid codegen/runtime drift — we're on `0.8.1.31-dev.gbd9b519`).
2. Build it from source (VS2022 BuildTools vcvars64 + LLVM/clang on PATH; it has `cmake/` +
   `scripts/`). Produce an installed tree with the patched graphics lib + headers.
3. Point nhl-legacy-recomp's `CMAKE_PREFIX_PATH` at the local patched install, reconfigure,
   rebuild `nhllegacy` (`_build_beta.bat`).
4. Keep the patch minimal and on a branch; document the exact diff so it survives SDK upgrades.

## Verification

- Capture: `_c5dump.ps1` with `NHL_HIGHCUT_GPU_TEX=1`, drive to the goalie, F10, hold ~10s.
- Offline check the pad draw's slot1 (the 8255-index skinned draw, `highcut_frame_<N>.bin`
  with `ps_tex=20`): decode it — clean = recognizable pad, not green/striped. (Reuse the
  /tmp/*.py decoders from the prior session or re-derive from the packet layout: header
  58×u32=232B, then sized blobs, then 40B `TexturePacketDesc` each; `tex_format` 0=RGBA8.)
- View: `_c5render.ps1` with `NHL_HIGHCUT_C5_SHOT=<png>`, or `_c5renderdoc.ps1 -FullFrame` (F12)
  + RenderDoc Pixel History on a pad pixel (the breakthrough tool — the green pad draw was
  `EID 1201 vkCmdDrawIndexed(8255,1)`).

## Key references / symbols

- Our code: `renderer/core/nhl_command_processor.cpp` — `ReadbackActiveTextureRGBA`,
  `untileBindings` (packet build, ~line 2390), the `EndSubmissionTag` Thief idiom (~line 120),
  `beta_texture_cache_` usage, `WriteActiveTextureBindfulSRV` call sites.
- rexglue API: `rex/graphics/d3d12/texture_cache.h` (the `final` class + SRV-write methods),
  `rex/graphics/pipeline/texture/cache.h` (`LoadTextureDataFromResidentMemoryImpl` virtual),
  `rex/graphics/pipeline/texture/util.h` (`GetGuestTextureLayout`, `GetTiledOffset2D`).
- rexglue source: github.com/rexglue/rexglue-sdk `src/graphics/`. Version: 0.8.1.31-dev.
- NHL12 RE: `docs/nhl12-decomp-reference/` (CZ recolor system, equipment material stack).

## Scope guardrails

- This is the **slot1 / GPU-built-texture** thread (thread C). The **tint/recolor not applied**
  (thread A) and **trapper culled** (thread B) are SEPARATE native-render bugs — don't conflate.
- All current high-cut changes are **gated env flags, uncommitted** — verify `git status` and
  the memory's CODE STATE list before building on them. Consider committing the working
  `NHL_HIGHCUT_GPU_TEX` path first.
- VERIFY the GPU-built hypothesis in source before investing in a build-from-source patch — a
  source patch is a real undertaking; make sure it's the right one.
