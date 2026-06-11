# H-3 — RenderDoc checklist: crack the player composite (draw #430)

> Goal: in ONE frame capture, determine why beta renders the create-player model
> shifted-right + wrapped while the base renders it center-right. Everything else about
> draw #430 is already pinned (correct full-frame viewport, samples a correct 1280×720
> player RTT texture `0x1AF09000`). The unknown is **untile vs UV/sampler** — RenderDoc
> answers it directly where logs can't (the generic vertex diag can't decode these 2D
> composite quads). Context: docs/highcut-h3-flat-rt.md.

## 0. The one question to answer

For draw #430: **is the bound player texture (SRV) already split/shifted, or is it correct
and the blit places it wrong?** That single look bisects the whole problem:
- SRV **split/shifted** → the bug is the **texture untile/addressing** of a *resolved
  render-target* texture (the flat/offscreen path doesn't track `0x1AF09000` as a resolved
  surface, so the texture cache untiles it as a plain 2D texture).
- SRV **correct** (player center-right) → the bug is **downstream**: the composite quad's
  **UVs** (past 1.0) and/or the **sampler address mode** (WRAP) and/or the **vertex
  positions**.

## 1. Capture setup (beta takeover frame)

The hook is already wired (`NHL_BETA_RENDERDOC` → StartFrameCapture before the first owned
draw, EndFrameCapture after the swap). Launch the game **through RenderDoc** so
`CreateIfConnected` succeeds:

- RenderDoc → *Launch Application*:
  - Executable: `out\build\win-amd64-relwithdebinfo\nhllegacy.exe`
  - Working dir: `out\build\win-amd64-relwithdebinfo`
  - Command line: `--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"`
  - Environment:
    - `NHL_BACKEND=beta`
    - `NHL_BETA_TAKEOVER=1`
    - `NHL_BETA_DEPTH=1`
    - `NHL_BETA_CAPTURE_FRAME=150`
    - `NHL_BETA_RENDERDOC=1`
    - `NHL_REPLAY_XTR=<bdir>\gpu_trace\scene_02\454109EC_stream.xtr`
- It fast-forwards (the per-frame poll is gated) to frame 150, auto-captures that frame,
  and the `.rdc` appears in RenderDoc's capture list. Confirm via the log line
  `[nhl-beta] RenderDoc StartFrameCapture (takeover frame)` /
  `EndFrameCapture -> captured`.

## 2. Locate draw #430 in the capture

The takeover frame is one D3D12 frame containing ~581 owned `DrawIndexedInstanced`/
`DrawInstanced` calls in order. Draw **#430** is the player composite. Find it by either:
- **Texture Viewer → Inputs**: scrub the Event Browser to the draw whose bound SRV is the
  **1280×720** texture (it'll be obvious — most other draws bind 32×32…256×256 UI atlases).
  Per the log, #430 binds base `0x1AF09000`, 1280×720, fmt=6, tiled, pitch=1280, fc=0.
- Or count: it's right after the blue background fill and the first UI draws (bisection put
  it at ~430; the full player is present by ~470).

Pin the EID. (Optional: re-run with `NHL_BETA_MAX_DRAW=431` to make #430 the *last* rendered
draw, so the final RT in the capture is exactly its output — easiest to read.)

## 3. The five things to read off draw #430

1. **Texture Viewer → the bound SRV (the player RTT).** THE decisive item (see §0).
   - Confirm dims 1280×720, format, and **look at the image**: player center-right (correct)
     or split/shifted to the edges (untile bug)?
   - Note the SRV's **row pitch / tiling / format** RenderDoc reports vs the guest fetch
     constant (1280px pitch, tiled, fmt 6). A pitch/format mismatch here = the untile bug.
   - *Save Texture* to a PNG so it can be diffed against the ground truth.
2. **Mesh Viewer → VS Output (the quad).** The composite is a full-screen quad/triangle.
   - **SV_Position** of each corner (expect full-screen: NDC ±1 → screen 0..1280 / 0..720).
   - **TEXCOORD/UV** of each corner. Expect [0,1]×[0,1] for a 1:1 blit. **If any U/V exceeds
     1.0 or is offset, that's the shift+wrap** (with a WRAP sampler the overflow wraps to the
     left edge → the sliver).
3. **Pipeline State → PS → Samplers.** The address mode (AddressU/AddressV): **WRAP vs
   CLAMP**. The console's blit uses clamp/border; if beta bound WRAP, UVs slightly past 1.0
   produce the left sliver. (beta's sampler reconstruction is a known weak spot — header
   note at the sampler-heap code.)
4. **Pipeline State → PS → Resources.** Confirm the SRV at the slot the PS samples is
   actually `0x1AF09000` (not a stale/other texture), and the **SRV swizzle / component
   mapping** (fetch swizzle was 0x60A — a wrong channel map would discolor, not shift, but
   confirm it's sane).
5. **Output RT before/after #430.** Step the Event Browser one draw: confirm #430 is the
   draw that paints the player (RT changes from no-player to player). Rules out a later draw.

## 4. Decision tree → the fix

| Finding | Root cause | Fix direction |
|---|---|---|
| SRV image **split/shifted** | resolved-RTT untile wrong (flat path) | In the flat/offscreen path, make the texture cache treat `0x1AF09000` as a resolved surface (track RT ownership / correct tile mode+pitch for resolved targets), so its untile matches the base. The EDRAM path's RT cache does this; the flat path skips it. |
| SRV **correct**, **UV > 1.0** | composite UV decode | Check the quad's UV vertex attribute fetch (format/endian) and the VS UV math; beta's vertex/UV reconstruction for this 2D quad differs from base. |
| SRV correct, UV [0,1], **sampler WRAP** | sampler address mode | Reconstruct the guest sampler's clamp/border address mode for this draw (beta's sampler path). |
| SRV correct, UV [0,1], CLAMP, **SV_Position offset** | quad geometry | Vertex position fetch/transform for the composite quad. |

## 5. Ideal: a base-path reference for the same draw

Beta's capture alone usually decides it (§0). If needed, capture the **base** path rendering
the same frame and compare the equivalent composite draw's SRV/UVs/sampler:
- Launch through RenderDoc with only `NHL_REPLAY_XTR=...scene_02...stream.xtr` (no
  `NHL_BACKEND`), let it reach the create-player frame, and trigger a manual RenderDoc
  capture (default hotkey **F12** / PrtScrn) on a frame showing the player. Find the draw
  binding the 1280×720 player texture and read the same five items. Any field that differs
  from beta's #430 is the divergence to fix.

## 6. What to bring back (so the fix can be written)

- The **SRV image** of draw #430 (split or correct?) — PNG.
- The quad's **4 corner UVs** and **SV_Positions** (Mesh Viewer).
- The **sampler AddressU/AddressV** (Pipeline State).
- The SRV's reported **format + row pitch** vs the fetch constant.

Those four data points pick the row in §4 and pin the exact fix.
