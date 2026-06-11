# Phase 0 — D3D9 hookability gate (high-cut engine)

> Investigation for the approved plan `~/.claude/plans/write-up-a-plan-lovely-whistle.md`. **Read-only RE; no
> engine code written.** Answers the make-or-break question: are NHL Legacy's D3D9 calls **out-of-line,
> hookable functions** (→ Approach A, true Unleashed-style D3D9 hook is viable) or **pervasively inlined**
> (→ fall back to a PM4 flat-RT approach)?

## Verdict: **GO — Approach A is feasible.** The D3D9 surface is out-of-line and hookable.

The game invokes the Xbox 360 D3D9/graphics runtime through **real out-of-line function calls**, not inlined
push-buffer writes. The surface is **bounded** (~181 entry points in one contiguous statically-linked library)
and every entry is directly `REX_HOOK`-able by guest address.

## Method (reusable — no flat image required)

The rexglue recompiler emits the **full static call graph in greppable form**: every guest→guest call is a
direct `sub_XXXXXXXX(ctx, base);`, every function is `DEFINE_REX_FUNC(sub_XXXXXXXX) {`, and the **original PPC
disassembly is preserved as `//` comments**. So the call graph + per-function behavior can be analyzed straight
out of `generated/default/nhllegacy_recomp.*.cpp` — no XEX disassembly or flat image needed. Tooling written
this session: `tools/d3d9_probe.py` (callers / characterize / enclosing-function). The legacy flat-image tools
(`tools/find_callers.py` etc.) need `nhl_legacy_image.bin`, which is a deleted temp file — the call-graph method
supersedes them.

## Evidence

1. **The graphics/D3D9 runtime is one contiguous, isolated library TU.** All graphics functions compile into
   `nhllegacy_recomp.31.cpp`: **473 functions**, guest range **`sub_827DC1A0` … `sub_827F9DC8`**. (Statically
   linked from the XDK `d3d9.lib`/`xgraphics.lib`; clustered because same-.lib functions are contiguous.)

2. **181 of the 473 are out-of-line entry points called from game code.** Cross-file call-graph analysis: 181
   library functions have at least one caller *outside* the library TU (i.e. from game code); 292 are
   library-internal helpers. The 181 are the **REX_HOOK-able D3D9 API surface** — full list with caller counts in
   `docs/phase0-d3d9-entry-points.txt`. This directly proves the game calls D3D9 as functions; if D3D9 were
   inlined there would be no such entry points.

3. **The present/submit path is a clean out-of-line chain.** game frame loop `sub_8272A798` →
   **`sub_827FA878` (D3D9 Present)** → **`sub_827F1C88` (swap/submit)** which calls
   `VdGetSystemCommandBuffer` + `VdSwap` + `VdPersistDisplay`. Hookable end to end.

4. **The objects are COM-style with out-of-line ref-counting.** The single most-called entry (`sub_827EB558`, 36
   distinct game callers) is an atomic refcount **Release** (`lwarx`/`stwcx.` decrement + zero-check + type tag
   in low 4 bits). Confirms `IDirect3D*9` objects with out-of-line AddRef/Release — a function-based API.

5. **The hook mechanism is proven and ready.** `REX_HOOK` / `REX_HOOK_RAW` / `REX_IMPORT` (`rex/hook.h`, BSD-3)
   auto-marshal PPC↔native; already used for kernel stubs (`src/kernel_stubs.cpp`). Declaring the 181 addresses
   host-implemented in `nhllegacy_functions.toml` + providing hook bodies replaces the guest D3D9 with our host
   engine. (The CPU recomp `PPCFuncMappings` table resolves calls to our hooks.)

## Caveats / bounded follow-up (none gate-blocking)

- **XG resource-header helpers are inlined**, as expected (`XGSetTextureHeader`/`XGSetSurfaceHeader` are inline
  in `xgraphics.h`; zero `XG*` import refs in the library). This does **not** block Approach A: the **D3D9
  `CreateTexture`/`CreateVertexBuffer`/`CreateRenderTarget` wrappers that call them are out-of-line entry
  points**, so resource creation is hookable at the D3D9 level — we just reimplement the header logic host-side.
- **Per-entry identification is the first M1 task.** We know the 181 addresses and that they're hookable; we have
  not yet labeled each (which is `DrawIndexedPrimitive`, `SetTexture`, `CreateTexture`, `Present`, `Resolve`…).
  Each body carries full PPC disasm — identify by signature + runtime correlation (RenderDoc on the base path /
  the existing oracle, or breakpoint-by-address against known call counts). Bounded, well-supported.
- **Confirm the hottest *per-execution* calls aren't inlined** (`DrawIndexedPrimitive`,
  `SetVertexShaderConstantF`). Initial evidence favors out-of-line (181 entry points; clean out-of-line
  Present/swap + COM Release; no inlined state-table writes found while sampling game functions). If a few hot
  setters turn out inlined, the **hybrid path** (hook resource creation for logical RT sizes — which already
  kills the fold — and take draws/constants from a thin PM4 tap) covers them. Pure-A remains the target.

## Entry-point labeling (initial pass, `tools/d3d9_label.py`)

Bucketed the 181 entry points by direct kernel-import anchor + structure (labeled map:
`docs/phase0-d3d9-entry-points.txt`):

| Bucket | Count | Notes |
|---|---|---|
| **PRESENT/SWAP** | 1 | `sub_827F1C88` (→ VdSwap/VdGetSystemCommandBuffer/VdPersistDisplay). Public wrapper `sub_827FA878`. |
| **COM-REFCOUNT** | 5 | small atomic `lwarx`/`stwcx.` AddRef/Release (`sub_827EB558` Release = 36 callers, the hottest entry). |
| **COM-DTOR/QI?** | 4 | larger atomic functions (destructors / QueryInterface). |
| **MISC (other imports)** | 15 | device-init / alloc / query-video paths (`Vd*`/`Mm*`/`Ex*`). Device creation lives here. |
| **DATA-PLANE** | 156 | **no kernel imports — Set*/Draw/state/resource-create**, write via guest pointers. The engine's core work surface. |

**Caveat #3 (hot data-plane calls inlined?) is RESOLVED — they are out-of-line.** The 156 data-plane functions
are real out-of-line functions called from game code, so Draw/state/constant-setters are hookable. The prime
**Draw / state-flush** function is **`sub_827EF8E0`** (1079 insns, 172 stores, 7 game callers incl. the frame
loop): it writes **133 command-buffer stores** to a computed effective address with a type-3 packet header — the
DrawIndexedPrimitive flush-and-emit path. Tiny data-plane functions (≤25 insns, 1–2 stores) are the
`SetRenderState`/`SetSamplerState`/`SetTexture` setters; zero-store/load-only ones are `Get*`.

Precise per-function identity for the 156 data-plane entries (which exact `Set*`/`Create*`/`Draw*`) is the M1
labeling task — best finished with runtime correlation (RenderDoc on the base/oracle path, or address
breakpoints matched against known D3D9 call counts), since they carry no import fingerprint. The statics above
already give the buckets and the high-value anchors.

## ⚠️ UPDATE (M1 runtime tap): the per-draw path is INLINED — hybrid, not pure-A

The live tap (`docs/highcut-m1-tap-correlation.md`) **partially walks back caveat #3 above.** Runtime
frequency correlation shows the out-of-line surface is cleanly hookable for **resource binding, present/swap,
device creation, refcounting, and setup** — but the **per-draw `DrawIndexedPrimitive` packet emission is
INLINED** (no out-of-line function matches ~61 draws/frame; the hot per-draw functions are fetch-constant
builders; reserve-space is 1/frame; the out-of-line packet verbs are cold). So a *pure* D3D9-verb high cut
**cannot capture draws** for NHL Legacy — the realistic shape is a **hybrid** (D3D9 hooks for resources/RTs/
present + PM4 for draws/clear/resolve). See the M1 doc for the strategic options. This does not change that the
*hookable surface is real and proven on the live game*; it changes what that surface covers.

## Decision

**Proceed with Approach A (true D3D9-API hook)** — *as later refined to a hybrid by the M1 tap (above).* Present, resource creation, ref-counting, and the entire
data-plane (incl. Draw `sub_827EF8E0`) are out-of-line and hookable; the surface is bounded at 181 functions in
one library TU; all three Phase-0 caveats are addressed. The next concrete step (M1) is to pin the core entry
points (Present, Clear, CreateDevice, Create{Texture,VertexBuffer,IndexBuffer,RenderTarget}, SetRenderTarget,
DrawIndexedPrimitive `sub_827EF8E0`, Resolve) from `docs/phase0-d3d9-entry-points.txt` via runtime correlation,
and stand up the first hooked, plume-backed cleared frame.

## Reproduce
```
python tools/d3d9_probe.py callers 0x827F1C88     # present-chain callers
python tools/d3d9_probe.py char    0x827EB558 ...  # characterize an entry point
# entry-point list: docs/phase0-d3d9-entry-points.txt (regenerated by the inline script in this session)
```
