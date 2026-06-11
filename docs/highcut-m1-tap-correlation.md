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

## Expanded correlation — 34 functions over ~1320 frames

Ground truth from the swap log: **draws≈62/frame, resolves=2/frame, shader_loads=16/frame**.

| guest fn | calls/frame | read (by frequency vs ground truth) |
|---|---|---|
| `sub_827E5938` | **137** | hottest per-draw setter (~2.2×/draw) — Set{Vertex,Pixel}ShaderConstantF / SetSamplerState class |
| `sub_827E2140` | **48** | **DrawIndexedPrimitive** (≈ the 62 PM4 draws; remainder likely a 2nd draw entry) |
| `sub_827EE438` / `sub_827E38B8` | 7.4 / 7.1 | per-some-draws setter (SetTexture / SetStreamSource / state batch) |
| `sub_827E39D8`/`ED9A8`/`E6978`/`E6D58` | 3.8–4.4 | mid per-frame state |
| `sub_827E8C20` | 2.8 | |
| `sub_827EB558` | 2.5 | COM Release |
| `sub_827E12B8` (tiny) / `sub_827E7138` | **~2.0** | **Resolve** candidates (match resolves=2/frame) |
| `sub_827EB650` | 1.9 | Get* (0 stores) |
| `sub_827EF8E0` | 1.8 | low-freq per-frame (Clear / BeginScene-class; NOT the draw) |
| `sub_827EB4E0` | 1.8 | COM refcount |
| `sub_827E2260` / `sub_827EC318` | **1.00** | **per-frame-once** ops — Clear-backbuffer / SetRenderTarget / BeginScene/EndScene |
| `sub_827F1C88` | 1.00 | **Present/Swap** (VdSwap) |
| `sub_827E3140` | 0.01 | device/resource init (startup) |
| `sub_827FA878` | 0.00 (2) | rare — reset/mode-change |
| `sub_827EED20`/`F97E0`/`ECD58`/`E5570`/`EDEC0` | ~0 | cold paths (init/teardown/error) — not in the hot render loop |

**Confident:** swap=`sub_827F1C88`, device-init=`sub_827E3140`, draw≈`sub_827E2140`,
hottest-setter=`sub_827E5938`, per-frame-once∈{`sub_827E2260`,`sub_827EC318`},
resolve∈{`sub_827E12B8`,`sub_827E7138`}. **To finalize** Clear/Resolve/Draw/Present exactly: read the
~5 candidate bodies (full PPC disasm in `generated/default/nhllegacy_recomp.31.cpp`) for the tell-tale
register writes (VGT_DRAW_INITIATOR for draw, RB_COPY_CONTROL/kCopy for resolve, color-clear for Clear).

## ⚠️ Key architectural finding: the per-draw path is INLINED (hybrid required)

Tapping the out-of-line packet-writing verb candidates (the in-lib callers of the reserve-space primitive
`sub_827EC318`: `sub_827F1588/24C8/2B60/4488/51B8/52A8/8B10`) shows them **cold — ~0 calls/frame** during
gameplay. Cross-checked with:
- **reserve-space `sub_827EC318` = 1.00/frame** (NOT per-draw — so draws don't call a reserve helper per packet).
- the **hot per-draw functions are fetch-constant builders** (`sub_827E5938` writes a 24-byte = 6-dword Xenos
  fetch constant, 137/frame = SetTexture/SetVertexBuffer; `sub_827E2140` 48/frame, similar).
- **no out-of-line function matches the ~61 draws/frame.**

⇒ **NHL Legacy inlines the per-draw `DrawIndexedPrimitive` packet emission** (the DRAW_INDX is written inline as
a few dword stores into a bulk-reserved command buffer); only the *resource-binding* helpers (SetTexture/
SetVertexBuffer fetch-constant builders) and the cold setup verbs are out-of-line.

**This refines (partially walks back) the optimistic Phase-0 read.** The cleanly-hookable out-of-line D3D9
surface gives us: **resource binding** (SetTexture/SetStreamSource), **present/swap** (`sub_827F1C88`),
**device/resource creation** (`sub_827E3140`), **refcounting**, and cold-path setup — but **NOT the per-draw
call**. So a *pure* "hook every D3D9 verb → plume" high cut **cannot cleanly capture draws** for this game.

### Strategic options (decision point)
1. **Hybrid high cut:** hook the out-of-line surface (resources / render targets / present / device) at the D3D9
   level — which is enough to **own render-target sizing and kill the EDRAM fold** — but obtain **draws/clear/
   resolve from the PM4 stream** (the rexglue low cut), since those are inlined. More moving parts than pure-A,
   but still removes the fold (we control RT allocation at the D3D9 level) and is the realistic shape for NHL.
2. **Reconsider scope:** if the draw path must stay PM4-level anyway, weigh how much the high cut actually buys
   vs. fixing the EDRAM modeling in the existing low cut.

This is exactly the kind of ground truth that only the live tap could reveal, and it should gate the next big
investment (plume routing) — routing *resources+present* through plume is viable; routing *draws* is not, for
this title, without PM4.

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
