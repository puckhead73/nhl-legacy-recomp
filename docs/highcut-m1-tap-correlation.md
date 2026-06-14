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

## SetRenderTarget / Resolve pinned (sizes the hybrid's EDRAM bookkeeping)

Tapped arg signatures (first calls; device = `r3=ADEC6A80`) + body reads:

- **Resolve = `sub_827EF8E0` — OUT-OF-LINE, CONFIRMED.** Body does `fcfid`→`stfs` of a **3-vertex (x,y) float
  rectangle** (`stfs f,0(r3)…20(r3)`) = the D3D9 resolve's `kRectangleList`/`k_32_32_FLOAT` rect (per SDK
  xenos.h). Args = `(device, flags=0x300, …, destTex=BFC6D0C0)` → **resolve dest texture is a hook argument**. ~2/frame = resolves=2.
- **The whole resource-binding layer is OUT-OF-LINE and pointer/descriptor-based.** `sub_827E5938` (137/frame)
  and `sub_827E2140` (48/frame, indexed r4=0,1,…) build 24-byte fetch constants from a resource; `sub_827E6480`
  (~0.8/frame) **copies a surface/texture descriptor block into a device slot** `(index+120)*16` via a VMX copy
  loop, args `(device, index=0, surface=ADBC7B00, 16)` — i.e. **SetRenderTarget / SetTexture-class bind**, at
  RT-change cadence. So bound textures / vertex buffers / render targets are visible **by pointer** at the hook
  level.
- **Swap/Present `sub_827F1C88`** args `(device, surface=BFC6D0C0, …, 0x500=1280, 0x2D0=720)` → present surface +
  size at the hook.

**Verdict:** Resolve + the resource-binding verbs are **out-of-line and carry logical resource pointers**; only
the **per-draw DRAW_INDX is inlined**.

### Double-check (per request): DRAW is inlined — CONFIRMED directly
`sub_827FFEC8` is a **game** render function (recomp.32.cpp, 563 insns) that repeatedly **loads/stores the
command-buffer current pointer at device offset 48** (`lwz r11,48(r31)` / `stw r10,48(r31)` …) — i.e. it writes
GPU packets **inline** by advancing the ring pointer, interspersed with out-of-line resource binds and an
out-of-line `Resolve` (`sub_827EF8E0`). So draws/state packets are emitted inline in game code; reserve-space
(`sub_827EC318`) is the bulk reserve (1/frame), not per-draw. Combined with "no out-of-line fn at ~61/frame,"
**the per-draw path is inlined** — re-confirmed by direct inspection, not just by absence.

### SetRenderTarget — partly resolved, with a caveat (per request)
`sub_827E6480` is an out-of-line surface bind: VMX-copies a 16-dword descriptor into device slot
`(index+120)*16` and sets a dirty flag. Over 30 live calls it is **always `(index=0, surface=ADBC7B00)`** at
~0.8/frame — a render-target-class bind (distinct from the present surface BFC6D0C0). But the captured boot is
menu/attract-heavy and only ever exercises **one** render target, so this is consistent with **SetRenderTarget
binding the single main RT** *and* with a fixed-resource bind — I could not force multi-RT variety on the live
boot to disambiguate. So: **an out-of-line RT-surface bind exists, likely SetRenderTarget, not airtight.**

**Why it barely matters for the hybrid either way:** the logical render-target **sizes/formats come from the
out-of-line `CreateRenderTarget` (resource-creation) hooks** regardless. So even if the per-frame RT *bind* were
partly inline, the residual EDRAM bookkeeping is just a small **EDRAM-base → logical-RT** map (a handful of RTs/
frame), with logical sizes already known — not the full fold reconstruction. The fold still dies (flat RTs sized
from CreateRenderTarget). Airtight SetRenderTarget identification would need a multi-RT live scene or the device
vtable (flat-image RE) — deferred as non-blocking.

### Why this shrinks the hybrid's EDRAM cost a lot
Because *where/what* (current RT, bound textures, resolve src→dest, present surface+size) all come from D3D9
hooks **as logical pointers**, the hybrid does **not** need the heavy "reverse-map EDRAM base → which resource"
machinery. The residual bookkeeping is light: **attribute each inlined PM4 draw to the currently-bound logical RT
by call ordering, render it to a flat RT sized from the draw's viewport.** The fold dies (flat RTs) and the
EDRAM↔resource reverse-mapping — the historically hard/buggy part — is largely avoided. (One item still worth a
final airtight check: that `sub_827E6480` is specifically `SetRenderTarget` vs `SetTexture` — tap by the
render-target surface pointer to confirm; strong-inference for now from the API's consistency.)

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

## 2026-06-11 — Cursory REVIEW of the "per-draw path is inlined" determination (post the PS-bank/VS-tex upsets)

Re-examined because this date's session overturned several "settled" conclusions. Verdict:
**the operational conclusion (draws not interceptable at any known out-of-line function →
hybrid/PM4 decode) SURVIVES, and is now better evidenced than M1 left it — but two of M1's
supporting claims were unsound, and one caveat should be recorded.**

What was checked:
1. **`sub_827E2140` recharacterization — VERIFIED by body read** (recomp.31.cpp:13999). It
   stores an (address, size) pair into a device shadow slot at `((17-index)+222)*8`, appends
   the previously-bound resource to a deferred-release ring (alloc at device+13928/+13932,
   flush via `sub_827E8EC0`), and sets dirty bits — a SetStreamSource-class BIND. No packet
   writes, no ring-pointer (+48) access. M1's *first* guess (DrawIndexedPrimitive) was the
   wrong part; its correction was right.
2. **Static scan for PM4 draw headers** across all 183 recomp TUs: NO `ori …,0x2200`
   (DRAW_INDX) construction exists anywhere; `ori …,0x3600` (DRAW_INDX_2) exists in exactly
   7 functions: `sub_827E5DB8/827EB7D0/827EB928/827EF8E0` (lib TU 31), `sub_827FFEC8/
   sub_82800798` (TU 32), `sub_83739050` (TU 163). Each has ≤2 static callers (vtable-
   dispatched). So DRAW_INDX_2 emission is CENTRALIZED and out-of-line — not pervasively
   inlined.
3. **Live frequency tap of those 7 emitters** (new `NHL_DRAW_TAP` counters in
   gpu/hooks/d3d9_resources.cpp; 75 s menu boot at 63 PM4 draws/frame): ALL ~0 calls/frame —
   `sub_827FFEC8` = **0 calls**. The menu's 63 draws/frame are DRAW_INDX (0x22) packets whose
   headers are built with no lis/ori immediates anywhere (template-copied or computed), so
   the static scan cannot locate the 0x22 emitter either way.

Corrections to M1's reasoning:
- **The "sub_827FFEC8 is a GAME render function writing packets inline" attribution was
  unsound**: TU index is an address chunk, not a library boundary (0x827FFxxx sits directly
  after the D3D9 lib range), and the live tap shows it is NOT on the menu's hot path at all
  (0 calls) — it was never evidence about the per-frame draw stream. It demonstrates inline
  ring-writes exist in SOME path, not in the path that emits the 63 draws/frame.
- **The "~61 draws/frame" ground truth overcounts game draw calls** (it includes resolve-rect
  draws, clear quads, and EDRAM no-op draws — established by this date's beta work). The
  inference survives anyway: the next-hottest non-bind candidate measured ~7/frame, far below
  any plausible real-draw count.

Standing caveat (how to overturn this later if ever needed): "inlined" remains an inference
from absence — the code that actually writes the DRAW_INDX headers has never been positively
located. If draw emission turns out to live in ONE hot EA-engine submit routine (rather than
truly inlined at every call site), that routine would be hookable and a pure high cut becomes
possible. The decisive experiment is a runtime WATCHPOINT on the command-buffer write pointer
(device+48) capturing the writing PC during a draw-heavy frame. Until someone runs that, the
hybrid (PM4-decoded draws + D3D9-hooked resources/present) stays the correct architecture.
