# M1 — D3D9-hook seam proven on the live game + runtime correlation

> First milestone of M1. Proves the D3D9-hook approach works on the **real
> recompiled game** (not just the plume standalone), and uses call-frequency
> correlation to pin entry-point identities the static labeling couldn't.

## Result: the hook seam works end-to-end ✅

`gpu/hooks/d3d9_tap.cpp` overrides 10 candidate guest D3D9 functions with
call-counting **pass-through** hooks (`REX_HOOK_RAW(sub_X){ count; __imp__sub_X(); }`),
env-gated by `NHL_D3D9_TAP`. Built into the main `nhllegacy` target (clang weak-alias
override — **linked with no duplicate-symbol errors**) and run against a live boot
(`--game_data_root`, ~45 s, ~1320 frames rendered):

- **All 10 hooks fired** (each logged a `FIRST-CALL`) — the linker wired our strong
  `sub_X` over the recomp's weak alias, and the recompiled game calls through them.
- **Rendering stayed intact** (pass-through) — the game rendered normally to frame
  1320+, ~61 draws/frame, exactly as without the tap. Zero behavior change.

This is the decisive proof for the whole high-cut pivot: we can intercept the
game's D3D9 calls on the live title and route them wherever we want (next: plume).

## Runtime correlation — measured call frequencies (over ~1320 frames)

| guest fn | calls/frame | refined identity |
|---|---|---|
| `sub_827F1C88` | **1.00** | **Present/Swap** (calls VdSwap) — confirmed. Drives one frame. |
| `sub_827FA878` | 0.00 (2 total) | NOT per-frame present (static guess wrong). Rare — device reset / mode change / BeginScene-once. |
| `sub_827E3140` | 0.01 (10 total) | **device / resource init** (startup-only) — confirmed rare. |
| `sub_827E2140` | 48.2 | **DrawIndexedPrimitive** candidate (≈ the 61 PM4 draws/frame; remainder likely a 2nd draw entry e.g. DrawPrimitive). |
| `sub_827E5938` | 132 | hottest **per-draw state setter** (~2×/draw) — SetVertexShaderConstantF / SetTexture class. |
| `sub_827EE438` | 7.2 | mid-freq per-frame (render-pass / target setup?). |
| `sub_827ED9A8` | 3.5 | low per-frame. |
| `sub_827EF8E0` | 1.78 | low-freq per-frame op — **static "Draw/flush" label was WRONG** (it writes 133 cmd dwords but only ~2×/frame → BeginScene/Clear/SetRenderTarget-ish, not the per-primitive draw). |
| `sub_827EB558` | 1.75 | COM Release. |
| `sub_827EB4E0` | 1.25 | COM refcount (AddRef/Release). |

**Key correction:** static structure alone mislabeled the draw. The per-primitive
draw is the **~48–60/frame** function (`sub_827E2140`), not the big-but-infrequent
`sub_827EF8E0`. Frequency correlation is the right tool to finish pinning.

## Next (M1 cont.)

1. **Finish pinning** the core entries by tapping more candidates and matching exact
   counts: confirm `sub_827E2140` = DrawIndexedPrimitive (vs DrawPrimitive split),
   identify the 132/frame setter, the resolve, CreateTexture/VB/IB, SetRenderTarget,
   Clear, the real CreateDevice. (The swap log gives ground-truth per-frame draw/
   resolve/shader counts to match against.)
2. **Route the first frame through plume:** with the seam proven, replace the swap
   `sub_827F1C88` (and device init) with host code that drives the plume device +
   swapchain (clear+present), rexglue GPU disabled — the first real high-cut frame.

## Reproduce
```
# build tap into the recomp:  (CMakeLists already lists gpu/hooks/d3d9_tap.cpp)
_build_beta.bat
# live boot with the tap, then read logs/nhllegacy_*.log:
$env:NHL_D3D9_TAP=1 ; <launch nhllegacy.exe --game_data_root ...>
#   grep "d3d9-tap] tick"  -> per-frame call frequencies
#   grep "FIRST-CALL"       -> which hooks the linker wired in
```
