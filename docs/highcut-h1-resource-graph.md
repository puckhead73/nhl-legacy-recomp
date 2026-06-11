# H-1 — logical resource graph from D3D9 hooks (DONE ✅)

> Milestone H-1 of the hybrid high cut. Pure **observation** pass: reconstruct the
> *logical* render-target graph entirely at the D3D9-hook level, proving we have the
> EDRAM-free information the rest of the hybrid renders from. No plume, no behavior
> change. Source: `gpu/hooks/d3d9_resources.cpp`, env-gated `NHL_HIGHCUT`.

## Result: the logical graph is fully recoverable at the D3D9 level ✅

A new TU overrides the out-of-line D3D9 verbs with **pass-through** hooks (same
weak-alias mechanism as the M1 tap; `__imp__sub_X` called for real). When `NHL_HIGHCUT`
is set it maintains a process-global registry `guestPtr → {kind,width,height,format,
baseAddr,pitch,tiled}`, current bindings, and a per-frame resolve list, dumped once per
frame in the Present hook. Run live (menu/attract boot, ~1400 frames). Verified:

- **Every logical-size signal at the D3D9 level is the TRUE logical size — never the
  EDRAM pitch (640).** This is the fold-killer proof: at this interception level the
  fold's 640-pitch surface simply does not exist; it is purely an artifact of the
  PM4/EDRAM emulation below.

| signal | source hook | value (menu scene) |
|---|---|---|
| present surface | `sub_827F1C88` | **1280×720** (args carry w/h directly) |
| viewport extent | `sub_827E6480` | **1280×720** (decoded from the transform block) |
| resolve dest (backbuffer) | `sub_827EF8E0` | `BFC6D0C0` **1280×720** fmt=6 |
| resolve dest (offscreen) | `sub_827EF8E0` | `BFB38B00` **1280×720** fmt=6 |
| sampled textures | `sub_827E5938` | `BFBD8140` 1280×720 fmt=18; `BFB2C080` 512×256 fmt=6 |

## Pinned identities

- **`sub_827EF8E0` = Resolve — CONFIRMED (count-exact).** Our resolve-hook count tracks
  rexglue's `[nhl-gpu] frame N swap: copies(resolves)=…` ground truth *exactly* through
  the scene transition: 1/frame on the early splash (frames 0–~300), 2/frame once the
  menu comes up (frame ~360+). The dest texture is hook arg **r6** (`r4=0x300` flags,
  `r6=destTex`); decoding it with the texture fetch-constant decoder gives the resolved
  RT's logical size (1280×720).
- **`sub_827F1C88` = Present/Swap — CONFIRMED.** 1/frame, args `(device, surface*, _,
  w=1280, h=720)`.
- **`sub_827E6480` = SetViewport / 2D-transform — NOT SetRenderTarget (corrects M1).**
  The block it copies into device slot `(index+120)*16` is a scale/offset **transform**
  (`+0`=1.0, `+20`=−1.0 Y-flip, `+48`=−640.0, `+52`=360.0, `+56`=**1280.0**) with **no
  GPU surface base address** — a viewport, not a pixel surface. M1 had flagged this
  identification as "likely SetRenderTarget, not airtight"; the descriptor content
  settles it: it is a viewport. The color-RT *bind* is not this function (consistent
  with the M1 finding that the per-draw / core render-state path is **inlined**).
  **This does not block the hybrid:** the logical RT size is fully recovered from
  Present + viewport extent + the resolve-dest texture decode (all out-of-line, all
  1280×720), so we never needed an out-of-line SetRenderTarget to size flat RTs.

## How dimensions are decoded (validated)

A D3D resource object carries a `GPUTEXTURE_FETCH_CONSTANT`-style header. The builder
`sub_827E5938` re-packs it into the hardware fetch constant; reading that code pins the
header layout: **fetch `dword_2` (width:13 | height:13 | stack:6, each minus 1) lives at
object+36**, format/base at object+32 (`xenos.h xe_gpu_texture_fetch_t`). Decode:
`width = (G32(obj+36) & 0x1FFF)+1`, `height = ((G32(obj+36)>>13) & 0x1FFF)+1`. Verified
against the full-screen texture `BFBD8140`: `+36 = 0x0059E4FF` → 1279+1=**1280**,
719+1=**720**. The viewport object uses a *different* (float transform) layout and is
decoded separately (`+56`=width, `+52`=half-height).

## What this gives the hybrid

`where`/`what` — present surface+size, viewport extent, bound textures, and resolve
src→dest — are all available at the D3D9 hook level as **logical** values. The residual
EDRAM bookkeeping is therefore light (attribute each inlined PM4 draw to the current
logical extent; render into a flat RT sized from it). The fold dies for free.

## Reproduce

```
_build_beta.bat                                   # swaps in gpu/hooks/d3d9_resources.cpp
$env:NHL_HIGHCUT=1 ; <launch nhllegacy.exe --game_data_root "H:\…\NHL Legacy - Vanilla">
#  grep "highcut] ===== frame"  -> per-frame logical graph dump
#  grep "highcut] Resolve"      -> resolve dest decode (vs nhl-gpu copies(resolves)=N)
#  grep "highcut] WIDE VPORT"   -> the viewport transform block
```

## Note on the build

`gpu/hooks/d3d9_resources.cpp` and the M1 tap `gpu/hooks/d3d9_tap.cpp` override the
**same** guest symbols (Present/Resolve/binders/…), so only one may be in
`NHLLEGACY_SOURCES` at a time. H-1 swaps the tap out (kept in-tree for reference). Both
are pure pass-through when their env var is unset, so the default build is unchanged.

## Next (H-2)

Link plume into the `nhllegacy` target; create a plume device in-process; take over
`Present` (`sub_827F1C88`) to drive a plume swapchain (start with a clear / a mirror of
the current frame).
