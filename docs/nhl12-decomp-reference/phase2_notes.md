# Phase 2 Notes — Recompilation & First Link

## Codegen gap inventory (first XenonRecomp run, disc XEX v4)

| Issue | Count | Resolution |
|---|---|---|
| Unrecognized instructions (~50 mnemonics) | 6,681 | new emitters in `patches/xenonrecomp-ea-toolchain.patch` → **0** |
| Switch labels outside function bounds | 3,766 (121 sites) | 101 manual function spans in `nhl12.toml` (tools/fix_function_bounds.py) → **0 real** |
| Residual switch errors in dead fragments | 553 (12 sites) | benign — see below |
| "Unable to decode" (data-in-text) | ~180 | benign: float/constant pools inside spans; unreachable |

### Why functions were truncated
EA's toolchain **omits `.pdata` entries for leaf functions**. XenonRecomp's
fallback scans (`bl`-targets + linear gap walk) terminate analysis at any
unannotated `bctr`, so a leaf containing a jump table ends right after the
dispatch and its case bodies fall outside. Manual spans
`[nearest known start ≤ bctr, next known start > max label)` fix all 122
affected switches (known starts = pdata ∪ bl-targets ∪ millicode ∪ entry).

### Residual 553 errors are in unreachable fragments
The linear gap walker re-analyzes regions *overlapping* our manual spans when
a preceding `Function::Analyze` overshoots (it ignores existing symbols and
data-in-text). The resulting fragment functions duplicate code mid-span,
re-encounter the armed switch, and fail its bounds check — but they emit
valid C++ (`return;` per out-of-bounds case) and are never call targets.
Verified: every real span function emits correct `goto loc_*` switches.
Post-MVP: fix the gap walker upstream (clamp Analyze at next symbol).

## The import linking contract (important, non-obvious)

XenonRecomp emits **no definitions** for import thunk functions — exactly 304
`PPC_EXTERN_FUNC(__imp__<Name>)` declarations. Direct call sites invoke
`__imp__<Name>(ctx, base)`; the function table maps each thunk's guest VA to
the same symbol. **The runtime provides these 304 symbols** (strong, extern
"C"). No weak-override or post-processing games needed.

Variable imports (13) have no symbols at all — the runtime must write each
slot with a guest address of backing storage (`runtime/main.cpp` kVarBase).

Slot fixup data is generated from `docs/kernel_imports.csv` by
`tools/gen_runtime_tables.py` → `runtime/gen_import_tables.inc` (X-macros).

## Caveat: `extracted/nhlzf_image.bin` is post-ParseImage

XenonUtils mutates the image while parsing (xex.cpp:331,342): import slot
dwords are byte-swapped in place (read them host-LE) and thunk code is
replaced with `nop/nop/nop/blr`. Harmless at runtime (slots get fixed up,
thunk bytes never execute) but anyone parsing the dump must know.
Long-term: dump pristine via a flag, or load the XEX directly in the runtime.

## Stub runtime design (Phase 2 scope only)

- One 4 GiB VirtualAlloc reservation; guest VA = offset. Function table at
  `base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE` (8 B per code word) per
  `PPC_LOOKUP_FUNC`.
- Committed: image, table, 1 MiB stack (`r1`), fake PCR (`r13`), variable
  block, bump-allocated guest heap for `NtAllocateVirtualMemory`.
- All imports = logging stubs returning 0, except honest `KeTls*`,
  `NtAllocateVirtualMemory` (CRT can't boot without), `KeQuerySystemTime`,
  `KeGetCurrentProcessType` (=2, PROC_TITLE).
- Goal metric: how far EA startup gets, measured by the import-call log
  before first exception. ReXGlue integration replaces this scaffold in
  Phase 3.

## setjmp/longjmp
Not yet located in the binary; TOML fields unset. If EA code setjmps, the
recompiled path will misbehave — find via string refs/Xenia trace when
symptoms appear.
