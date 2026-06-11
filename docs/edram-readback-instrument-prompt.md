# Task prompt — Direct EDRAM compute-readback instrument (scene_02 green-band)

> Hand-off prompt for implementing the definitive instrument that ends the scene_02 "green player"
> investigation. Read `docs/agent-coordination-handoff.md` (2026-06-10 Claude entries) and the memory
> `rov-green-player-is-fold-color` FIRST — they contain the full elimination chain this builds on.

## Mission

Add an env-gated diagnostic that **reads the beta ROV EDRAM host buffer directly and dumps the
composite color surface and depth surface in their NATIVE 640-pitch EDRAM tile layout** (no
pitch<width unfold). This answers the one open question with direct evidence instead of inference:

- **If the native 640-wide color surface is CLEAN** (correct blue bg / no green) → the green is
  introduced by the SDK resolve's pitch<width UNFOLD to 1280 (resolve-side; it reads the left display
  band from the wrong EDRAM region).
- **If the native color surface ALREADY contains the `(0,127,15)` green** → it's render-side (the
  draws/EDRAM addressing wrote it).

Do NOT attempt a fix in this task. The deliverable is the instrument + the answer to the above.

## Proven context (don't re-derive)

The scene_02 ROV "green player" is a structural color defect, NOT missing materials (oracle-confirmed).
Already ruled out, each with a clean experiment: missing materials, PNG readback swizzle, ROV blend
constant, EDRAM snapshot reseed (`NHL_BETA_NO_RESEED` identical), 3D player passes (`ZEN_FILTER=0`
still green), guest clear-color (`RB_COLOR_CLEAR=0`/black), resolve MSAA sample averaging
(`NHL_BETA_FORCE_SAMPLE` sweep identical), and any specific draw (`NHL_BETA_SKIP_RANGE` over 0-278 all
byte-identical — note SKIP_RANGE skips geometry but resolves still run, so the green is resolve-driven).

The corruption splits at EXACTLY display x=640 (the 640-pitch fold line): left band [0,639] gets an
additive `(0,127,15)`; right band clean. `(0,127,15)` read as little-endian 24-bit = `0x0F7F00` ≈
depth 0.06 (near plane) — consistent with the left band being unfolded from a depth-like region.

### Key EDRAM parameters of the frontbuffer composite (resolve #14, dest 0x9000000)

From the `NHL_BETA_EDRAM_DIAG` base probe (`logs/nhllegacy_433.log`), the composite/player passes
(owned draws ~270-360) use:

- `color0_base_tile = 0`, `depth_base_tile = 736`
- `tile_pitch = 8 tiles` (`edram_32bpp_tile_pitch_dwords_scaled = 10240` dwords)
- `surf_pitch = 640`, **`msaa = 1`** (NOT 4X — a 1x surface)
- EDRAM tile = `kEdramTileWidthSamples`(80) x `kEdramTileHeightSamples`(16) samples; at 32bpp =
  `edram_tile_dwords = 1280` dwords (5120 bytes) per tile.
- Color surface = 8 tiles/row x ceil(720/16)=45 rows = **360 tiles -> tiles [0,360)**.
- Depth surface (24+8) at base 736 -> **tiles [736,1096)**. No trivial base overlap with color.

So the native color surface is 640x720, stored as 8x45 = 360 EDRAM tiles starting at tile 0.

## Repro / build / oracles

- Repro (PowerShell, run the script directly so the hashtable passes natively):
  `& "e:\Repositories\nhl-legacy-recomp\run_edram.ps1" -Scene scene_02 -Frame 30 -Edram 1 -Extra @{ NHL_BETA_RT_PATH='rov'; <YOUR_ENV>='1' }`
  Output: `out/build/win-amd64-relwithdebinfo/beta_owned_draw.png` (the green frontbuffer).
- Real-console oracle (correct): `out/build/.../oracle_editplayer_f30.png` — blue bg (10,9,225), blue UI.
- Offscreen path (correct, no EDRAM fold): `codex_offscreen_baseline.png`.
- Build (incremental = 1 obj + link; VS2022 BuildTools vcvars64 + LLVM on PATH per memory
  `rexglue-build-environment`):
  `cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && set "PATH=C:\Program Files\LLVM\bin;%PATH%" && cmake --build "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo" --target nhllegacy'`

## SDK seam

`beta_render_target_cache_` is a `rex::graphics::d3d12::D3D12RenderTargetCache` (header:
`E:\Tools\rexglue-sdk\0.8.0\win-amd64\include\rex\graphics\d3d12\render_target_cache.h`). It is
`final`; the EDRAM resource `edram_buffer_` is PRIVATE — you cannot `CopyBufferRegion` it directly.
Reachable public methods:

- `void WriteEdramRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle)` — writes a raw
  ByteAddressBuffer SRV of the whole EDRAM buffer into a descriptor you own.
- `void WriteEdramRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle)` — raw UAV (Codex already uses
  this to bind EDRAM for the ROV shader; see how the shmem/EDRAM descriptors are built in
  `IssueDraw`/the owned-draw path around the `WriteEdram*Descriptor` callsites).
- EDRAM size = `xenos::kEdramSizeBytes` (~10 MB). The ROV path keeps live data in `edram_buffer_`.

Because the resource pointer is private, the read path is: **bind the EDRAM raw SRV (or UAV) to a
trivial compute shader that copies EDRAM dwords into an output UAV buffer you own, then
CopyResource/CopyBufferRegion that into a READBACK buffer, map it, and untile in C++.**

### Barrier note

`edram_buffer_state_` and `TransitionEdramBuffer`/`CommitEdramBufferUAVWrites` are private. After the
composite draws the buffer is in a ROV/UAV state. Prefer reading it via the **raw UAV** descriptor in
your compute shader (matches its current state, avoids a transition you can't drive). If you hit a
state-mismatch/debug-layer error, fall back to the SRV path and add a UAV barrier
(`ResourceBarrier` of type UAV) before your dispatch; you do not have a transition barrier because you
lack the resource ptr, so design around the UAV state.

## Implementation

Add `NHL_BETA_EDRAM_DUMP` (env-gated; default path completely unaffected). Suggested shape, modeled on
the existing `RecordBetaGpuDumps`/`WriteBetaGpuDumps` and `DumpBetaEdramSwap` in
`renderer/core/nhl_command_processor.cpp`:

1. **Trigger point:** in `BetaResolveEdram`, when `this_resolve == 14` (the frontbuffer composite,
   `copy_dest_pitch == 1280`, dest `0x9000000`) and BEFORE the `beta_render_target_cache_->Resolve(...)`
   call — so you capture the EDRAM the unfold is about to read. Gate on `NHL_BETA_EDRAM_DUMP`. Make it
   one-shot (a `bool beta_edram_dump_done_` member) since the replay loops frames.
2. **Compute copy:** create (once) a tiny compute PSO + root signature: SRV/UAV table param 0 = EDRAM
   raw (via `WriteEdramRawSRVDescriptor`/`WriteEdramRawUAVDescriptor` into your own non-shader-visible
   staging heap, then copy to a shader-visible heap, or write directly into a shader-visible heap),
   UAV param 1 = your `out_buffer` (a DEFAULT-heap ByteAddressBuffer sized `kEdramSizeBytes`). HLSL:
   `[numthreads(64,1,1)] void main(uint3 id){ out.Store(id.x*4, edram.Load(id.x*4)); }` dispatched over
   `kEdramSizeBytes/4` dwords. (You can compile the shader offline to DXBC and embed it, or reuse the
   provider's shader-compile path if one is exposed.)
3. **Readback:** `CopyResource` `out_buffer` -> a READBACK buffer; fence-wait; map.
4. **Untile + write PNGs.** EDRAM tile untile for a surface at `base_tile` B, pitch `P` tiles, width
   W=640, height H=720, 32bpp:
   ```
   for y in [0,H): for x in [0,W):
     tile_index   = B + (y/16)*P + (x/80)
     within_tile  = (y%16)*80 + (x%80)            // sample order within an 80x16 tile
     dword        = tile_index*1280 + within_tile  // 1280 = edram_tile_dwords
     pixel32      = mapped[dword]                  // 8888; apply the same endian/swap as DumpBetaEdramSwap
   ```
   Emit two PNGs (use `nhl::replay::WritePng`, BGRA->RGBA like the existing dumps):
   - `edram_native_color.png` — color surface B=0, P=8, 640x720, interpret as 8888.
   - `edram_native_depth.png` — depth surface B=736, P=8, 640x720, interpret the 32-bit as D24S8
     (visualize depth as luma; also dump raw so a value of `0x0F7F00`/0.06 is checkable numerically).
   VERIFY the tile/sample swizzle against a known-good region: the right-half of the bg should be a
   clean blue gradient — if the untile is wrong the image will stripe (as the guest-tiling gpudump did).
   If 8 tiles/row looks wrong, also try the MSAA-doubled sample pitch the ROV constants use
   (`pitch_samples = surf_pitch * (4xMSAA?2:1)`), but at msaa=1 the plain 8-tile pitch should be right.

## Success criteria / what to report

- Two readable PNGs of the native 640-pitch EDRAM color and depth surfaces at the composite, plus the
  raw dword at a few left-band coordinates (e.g. the bg region that displays as `(0,127,15)`).
- A one-line verdict: **does the native 640-wide color surface contain the green, or is it clean?**
  - Clean color + depth≈0.06 in the corresponding region -> confirms RESOLVE-SIDE unfold reads
    depth-as-color for the left display band. Next fix target: the resolve source pitch/base setup.
  - Green already in color tiles [0,360) -> RENDER-SIDE; next target is the ROV draw EDRAM addressing.
- Update `docs/agent-coordination-handoff.md` (validation + change ledger + a session entry) and the
  `rov-green-player-is-fold-color` memory with the verdict.

## Guardrails

- Env-gated, one-shot, zero effect on the default/non-takeover and 2D-parity paths. Don't change
  resolve behavior in this task.
- Preserve all existing diagnostics already added (env-gated): `NHL_BETA_RESOLVE_DIAG` (resolve state +
  `RB_COLOR_CLEAR` probe), `NHL_BETA_FORCE_SAMPLE`, `NHL_BETA_SKIP_RANGE`, the `NHL_BETA_EDRAM_DIAG`
  base probe. The existing `NHL_BETA_GPUDUMP` reads GUEST shared memory (post-resolve) and is scrambled
  for these folded surfaces — it is NOT a substitute for reading `edram_buffer_`.
- Regression signal for any later fix: the left band must match `codex_offscreen_baseline.png`
  (red channel byte-identical), and the bg/UI must match `oracle_editplayer_f30.png` (blue, not cyan).
- Ownership: claim the EDRAM-resolve row in the coordination ledger before editing; preserve Codex's
  ROV work.
