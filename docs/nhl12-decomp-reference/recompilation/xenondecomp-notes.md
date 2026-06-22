# Recompilation Notes — PPC → C++ Translation

How the NHL 12 PowerPC executable becomes the C++ that this documentation studies.
This is the **highest-confidence** part of the docs: it describes the project's own
tooling and is corroborated by `PLAN.md`, `HANDOFF.md`, the project memory, and the
generated code itself.

> Two recompiler paths exist in the repo. **Phase 2** used **XenonRecomp** →
> `recompiled/` (593 files). **Phase 4** adopted **RexGlue's own codegen** →
> `app/generated/default/` (154 files) and is the live path. Concepts below apply
> to both; ABI/runtime specifics differ (see [`rexglue-runtime.md`](rexglue-runtime.md)).

---

## 1. The translation model

Each PPC function is emitted as:
```cpp
PPC_FUNC(sub_<addr>) {            // void(PPCContext& ctx, uint8_t* base)
    // <ppc mnemonic>
    <C++ statement operating on ctx + base>
    ...
}
```
- **`ctx`** — the emulated register file (`PPCContext`): GPRs `r0..r31` (as a union
  `.u64/.s64/.u32/...`), FPRs `f0..f31`, VMX vectors `v0..v127`, and `cr*`, `xer`,
  `lr`, `ctr`, `fpscr`. (RexGlue's `PPCContext` additionally has `vscr_sat` and
  `last_indirect_target` — the ABI fork, see [`rexglue-runtime.md`](rexglue-runtime.md).)
- **`base`** — pointer to guest RAM. Guest address `A` ↦ `base + (uint32_t)A`.
- **Memory access macros** byte-swap (PPC is big-endian):
  `PPC_LOAD_U16(x) = bswap16(*(u16*)(base+x))`, etc. In RexGlue output the macros are
  `REX_LOAD_*`/`REX_STORE_*`. Loads/stores are `volatile` to defeat host reordering.
- **Control flow** — basic blocks become `loc_<addr>:` labels with `goto`; calls are
  direct invocations of `sub_<target>`; indirect calls/`bctr` dispatch through a
  function table.

The original mnemonic is kept as a comment, which is the only "documentation" inside
the translated code — see the sample in [`../README.md`](../README.md) §1.

## 2. What is safe to recompile directly
The vast majority. Plain ALU/FP/branch/load-store code translates 1:1. The hard
parts are the boundaries:

| Category | Disposition | Why |
|---|---|---|
| Ordinary code (ALU, FP, VMX, loads/stores, calls) | **Recompile** | Pure register/memory transforms. |
| Kernel/XAM imports (317) | **Runtime-provided** | Replaced by host implementations — see [`rexglue-runtime.md`](rexglue-runtime.md) and `docs/kernel_imports.md`. |
| `Vd*` GPU ring (25) | **Override** | The GPU command boundary; never recompiled. |
| `XMA*` audio (17) | **Override** | MMIO-adjacent decoder registers. |
| MMIO / PPC hardware exceptions | **Override / N/A** | XenonRecomp does not support MMIO or PPC exceptions. NHL 12 reaches XMA via kernel imports, not MMIO, so this costs nothing here. |
| `setjmp`/`longjmp` | **Special-cased** | Must emit host non-local return — see §5. |

## 3. Function discovery — the hard part on EA's binary
EA's compiler/linker defeats stock analysis. Three problems, three solutions
(all CONFIRMED, from project memory + tooling):

1. **Jump tables are invisible to stock detectors.** EA codegen interleaves nops in
   switch dispatch, so XenonAnalyse finds **0** tables. A structural detector
   (`tools/ea_jumptables.py`) recovers **505 tables / 14,718 labels**
   (`config/switch_tables.toml`). RexGlue consumes these as `[[switch_tables]]`.
2. **`.pdata`-less leaf switches.** 101 manual function spans fix EA's leaf functions
   that lack unwind data (`config/functions_fix.toml` / `app/nhl12_fixups.toml`).
3. **Indirect-only ("gap") functions.** 860 functions are reached *only* through
   runtime-built pointers — invisible to static analysis. `tools/find_gap_functions.py`
   finds them heuristically: code that is (after a terminator `blr`/`b`/`bctr`) AND
   (not already registered) AND (not padding) AND (not a static branch target) AND
   (not a switch label). Emit them with **no size** so the analyzer CF-scans the real
   extent (`app/nhl12_gapfuncs.toml`). Their bodies branch into shared tails not in
   any function → `tools/add_unresolved.py` adds those (`app/nhl12_extrafuncs.toml`),
   iterated until codegen validates 0 errors.

> RexGlue's analyzer is markedly better than stock XenonAnalyse on this binary:
> a full analyze leaves only **5** unresolved-call sites (vs thousands), fixed by the
> manual boundaries above.

## 4. EA toolchain instruction set
EA's compiler emits PPC instructions XenonRecomp didn't originally handle. Phase 2
fixed **6,681 missing-instruction errors** by adding **~50 new emitters**
(`patches/xenonrecomp-ea-toolchain.patch`, pinned to XenonRecomp `ddd128bc` — reapply
after a fresh clone). Also needed: `roundeven` shims and `PPC_FUNC`-macro-signature
import stubs (MSVC mangles `__restrict` otherwise).

## 5. setjmp/longjmp — the front-end gate (CONFIRMED root cause)
EA's engine wraps initialisation in `setjmp`/`longjmp` for error handling. The
recompiler emits host `ppc_setjmp`/`ppc_longjmp` **only for explicitly configured
addresses**; an unconfigured pair is recompiled as ordinary PPC, so the non-local
return is broken and protected init regions return garbage → null-object AV in the
front end.

**Fix (verified by disassembly):**
```
setjmp_address  = 0x83366050   # saves f14-f31 / r13-r31 / CR / LR / v64-v127 to jmp_buf@r3
longjmp_address = 0x833643B0   # restores the same
```
Set in `app/nhl12_fixups.toml`. This single fix unblocked the entire front-end boot.

## 6. Numeric behaviour to preserve
- **Big-endian** memory — every access byte-swaps (§1).
- **Floating-point:** PPC FP semantics (incl. `fpscr` rounding/flush modes) are
  modelled; `disableFlushMode()` calls appear around FP-store sequences. Divergence
  here causes subtle gameplay/physics drift.
- **VMX/AltiVec (`v0..v127`)** — the 360's vector unit, used by `rw::math` and physics.
  Translated to SSE/AVX via simde (`<x86/sse*.h>`). Lane order and saturation
  (`vscr_sat`) must match.
- **`randomd0`** — deterministic RNG; bit-exactness matters for replay/sim fidelity.

## 7. Output partitioning
RexGlue splits the 103,714 functions into 154 `nhl12_recomp.N.cpp` by address. The
writer content-hashes (XXH3_128) and skips unchanged files, so a config tweak only
rewrites affected files — **but** adding/removing functions reshuffles the
address→file partition, changing essentially all 154 files → full rebuild. Batch
config changes, validate on the Release CLI (fails fast at Validate, ~2.5 min), then
one full rebuild.

---
See [`rexglue-runtime.md`](rexglue-runtime.md) for the host side (imports, GPU,
audio, ABI) and [`../quirks/gotchas.md`](../quirks/gotchas.md) for the failure modes.
