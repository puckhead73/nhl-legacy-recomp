# Hybrid high-cut engine — development kickoff

> Paste everything below the line as the opening prompt of a new session. It is self-contained: it assumes no
> memory of the investigation that produced it. The architecture is **decided** — this is a build kickoff, not a
> re-evaluation.

---

We are building a **hybrid high-cut rendering engine** for `e:\Repositories\nhl-legacy-recomp` — a static
recompilation (PPC→x86, via the BSD-3 "rexglue" SDK, a Xenia-derived runtime) of Xbox 360 **NHL Legacy**. This
is a **build** session. The architecture below is settled by prior investigation; **do not re-litigate it** —
execute the first milestone.

## Background you must read first (then stop reading and build)
- `docs/highcut-m1-tap-correlation.md` — the runtime findings that define the hybrid (READ FULLY).
- `docs/highcut-m0-vendoring.md` — plume/XenosRecomp vendoring + the build toolchain.
- `docs/phase0-d3d9-hookability.md` + `docs/phase0-d3d9-entry-points.txt` — the 181 out-of-line D3D9 entry points.
- `docs/high-cut-pivot-and-tiling-investigation.md` — why we pivoted (the EDRAM fold) + the tiling verdict.
- Memories: `[[highcut-phase0-d3d9-hookable]]`, `[[tiling-verdict-no-game-tiling]]`,
  `[[high-cut-pivot-decision]]`, `[[unleashed-recompiled-no-edram]]`.

## The architecture (decided)
The game's rendering splits cleanly by **interception level**, because of how the Xbox 360 D3D9 was compiled:
- **Out-of-line (hookable via `REX_HOOK`) — the engine owns these at the D3D9 level, EDRAM-free:** resource
  creation (`CreateRenderTarget`/`CreateTexture`/`CreateVertexBuffer`/…), resource binding (SetRenderTarget,
  SetTexture/SetVertexBuffer), **Resolve** (`sub_827EF8E0`, confirmed — dest texture is an arg), **Present/Swap**
  (`sub_827F1C88`, confirmed — drives VdSwap, carries the present surface + 1280×720).
- **Inlined — the per-draw `DRAW_INDX` is written inline in game code** (confirmed: game fns advance the
  command-buffer pointer at device offset 48 and store packets directly; e.g. `sub_827FFEC8`). **Draws therefore
  reach the GPU only as PM4** and are decoded by rexglue's command processor as today.

**Why this kills the fold (the whole point):** the "fold"/green/projection bugs were artifacts of rexglue
rendering draws into EDRAM-**pitch**-sized render targets (e.g. 640-pitch for a 1280-wide image). At the D3D9
level we learn each render target's **logical** size from the `CreateRenderTarget` hook (1280×720), so we render
draws into **flat, logical-sized** RTs and the fold never forms. The residual EDRAM bookkeeping is tiny: a
**EDRAM-base → logical-RT** map (a handful of RTs/frame), with sizes already known from hooks — NOT the historical
fold reconstruction.

**plume** (vendored MIT RHI, D3D12+Vulkan, `third_party/plume`) owns presentation, host resources, and the
enhancement surface (internal-res/4K, ultrawide, modern AA, portability). The Xenos→DXBC shader translator
(rexglue `DxbcShaderTranslator`; XenosRecomp for the portable DXIL+SPIR-V path later) carries over.

**One design fork deferred to milestone H-3 (do NOT decide now):** how decoded draws ultimately render into the
flat plume RTs — (a) rexglue's SDK-D3D12 renders into flat logical-sized RTs (pragmatic, draws stay D3D12) vs
(b) reimplement draw execution on plume (portable to Vulkan, more work). H-1 is independent of this.

## What already exists (built + committed; `git log` on `master`)
- **plume vendored + a working standalone**: `gpu/smoke/` builds plume and clears+presents a window on **both
  D3D12 and Vulkan** (`SMOKE OK`). Build it with `gpu/smoke/_build_smoke.bat`. Deps fetched via
  `tools/fetch_thirdparty.ps1` (pinned; gitignored).
- **The D3D9 hook seam is proven on the live game**: `gpu/hooks/d3d9_tap.cpp` overrides ~41 guest D3D9 functions
  with call-counting pass-through hooks (env `NHL_D3D9_TAP`). **This is your hook template.** The
  `REX_HOOK_RAW(sub_X){ ...; __imp__sub_X(ctx, base); }` weak-alias override links cleanly into the recomp and
  fires on the live game.
- **git** is initialized (baseline + the high-cut work); big/regenerable artifacts gitignored.

## Build & run recipe (this machine)
- **Build the recomp** (incremental = 1 obj + link): `_build_beta.bat` (VS2022 BuildTools vcvars64 + LLVM/clang +
  Ninja; builds the `nhllegacy` target). Add new sources to `NHLLEGACY_SOURCES` in `CMakeLists.txt`.
- **Run live** (the guest D3D9 runs, so out-of-line hooks fire): launch
  `out/build/win-amd64-relwithdebinfo/nhllegacy.exe --game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"`
  (via PowerShell `Start-Process`), wait ~35–45 s for boot+render, then kill. It boots into a menu/attract scene
  rendering ~61 draws/frame, 2 resolves/frame.
- **`nhllegacy` is a WIN32 GUI app — no console.** Use **`REXLOG_INFO(...)`** (include `<rex/logging.h>`), NOT
  stderr. Output lands in `out/build/win-amd64-relwithdebinfo/logs/nhllegacy_*.log` alongside the
  `[nhl-gpu] frame N swap: draws=.. resolves=..` ground-truth lines.
- **RE without a flat image**: the generated recomp at `generated/default/nhllegacy_recomp.*.cpp` preserves the
  full call graph (guest→guest calls are `sub_XXXX(ctx,base)`) AND the original PPC disasm as `//` comments.
  Tools: `tools/d3d9_probe.py` (callers/characterize), `tools/d3d9_label.py` (bucket/label). Don't rebuild the
  deleted flat image.

## Known entry points (from the live tap; device pointer = guest `r3 = D3DDevice`)
- `sub_827F1C88` = **Present/Swap** (1/frame, VdSwap; arg surface = present backbuffer + 1280×720).
- `sub_827EF8E0` = **Resolve** (≈2/frame; builds the 3-vertex k_32_32_FLOAT resolve rect; arg = dest texture).
- `sub_827E6480` = **SetRenderTarget**-class surface bind (≈0.8/frame; VMX-copies a 16-dword surface descriptor
  into device slot `(index+120)*16`; live boot only exercised one RT so identity is "very likely SetRenderTarget"
  — confirm in H-1 once `CreateRenderTarget` is hooked and you can correlate).
- `sub_827E5938` (137/frame) / `sub_827E2140` (48/frame) = per-draw resource binders (fetch-constant builders =
  SetTexture/SetVertexBuffer class; take resource pointers + slot indices).
- `sub_827E3140` = device/resource **allocator** (startup-only; reaches Vd-init + MmAllocatePhysicalMemory) — the
  `Create*` functions route through this.
- `sub_827EB558`/`sub_827EB4E0` = COM AddRef/Release.

---

## MILESTONE H-1 (do this now): build the logical resource graph from D3D9 hooks

**Goal:** at the D3D9-hook level, capture the complete *logical* resource graph each frame — every render
target/texture/buffer with its **logical size + format + guest address**, the current bindings, and each
resolve's **src→dest** — proving we have the EDRAM-free information the rest of the hybrid renders from. This is
pure hook work (no plume rendering yet), fully verifiable from the log, and it definitively pins SetRenderTarget.

**Steps:**
1. **Find the resource-creation entry points.** Start from the allocator `sub_827E3140` and the resource-binding
   functions; use `tools/d3d9_probe.py callers/char` + read bodies to identify `CreateRenderTarget`,
   `CreateTexture`, `CreateVertexBuffer`, `CreateIndexBuffer`, `CreateDepthStencilSurface`. They are out-of-line,
   called mostly at load time (so hook them and watch FIRST-CALL + args at boot). A surface/texture object's
   width/height/format/EDRAM-or-guest-address are in its fields — capture the object pointer + decoded dims.
2. **Add `gpu/hooks/d3d9_resources.cpp`** (new TU; add to `CMakeLists.txt`; env-gate behind `NHL_HIGHCUT` so the
   default build is unchanged). Hook: the `Create*` functions, `SetRenderTarget` (`sub_827E6480`), `SetTexture`/
   binders, `Resolve` (`sub_827EF8E0`), `Present` (`sub_827F1C88`). All **pass-through** (call `__imp__sub_X`)
   for now — observe only.
3. **Maintain a registry** (plain C++ structures, guarded by a mutex; the guest is multi-threaded):
   - `guestPtr → Resource{ kind(RT/DepthStencil/Texture/VB/IB), width, height, format, guestAddr, edramBase? }`
   - current bindings: `currentRT[0..3]`, `currentDepth`, `currentTexture[stage]`
   - resolve log: list of `{ srcRT(currently-bound), destTexture(arg), srcRect, frameIndex }`
4. **Dump the graph** once per frame (in the Present hook) via REXLOG: the bound RT's **logical** size (expect
   **1280×720**, NOT 640-pitch — this is the fold-killer proof), the bound textures, and the frame's resolves
   (src→dest). Cross-check against the `[nhl-gpu] frame N swap: draws=.. resolves=..` line (resolves should match
   your resolve-hook count).

**Done when:** the per-frame dump shows the scene's render target(s) at their **logical** dimensions, the bound
textures, and resolve src→dest pairs that match rexglue's resolve count — i.e. we can reconstruct the logical
render-target graph entirely from D3D9 hooks, and `sub_827E6480`'s SetRenderTarget identity is confirmed.

## Milestone ladder (after H-1)
- **H-2:** link plume into the `nhllegacy` target; create a plume device in-process; take over `Present`
  (`sub_827F1C88`) to drive a plume swapchain (start by presenting a clear or a mirror of the current frame).
- **H-3:** render draws into **flat, logical-sized** render targets (sized from the H-1 graph) — validate the
  **fold is gone** on a 3D scene (the green/projection bug absent). *(Here you decide the deferred draw-execution
  fork: SDK-D3D12-into-flat-RTs vs reimplement-on-plume.)*
- **H-4:** Resolve hook → host copy into the dest texture; CreateTexture hook → plume texture (enables texture
  mods); resource coherence with guest memory.
- **H-5+:** enhancements — internal-res/4K, ultrawide, modern AA; XenosRecomp DXIL+SPIR-V for Vulkan portability.

## Guardrails
- Don't re-litigate the architecture or re-run the inlined-draw investigation — it's settled (draws inline,
  everything else out-of-line). Don't rebuild the flat image; use the generated C++ + the tap.
- Keep new behavior **env-gated** (`NHL_HIGHCUT`) and **pass-through** until a milestone explicitly takes over, so
  the default build/rendering stays byte-identical and every step is reversible (commit per milestone).
- `git commit` per working step (you have a clean `master`).
