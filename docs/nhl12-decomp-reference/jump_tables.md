# EA Toolchain Jump-Table Patterns in `nhlzf.exe`

*Outcome of plan §Phase 1.3. Stock XenonAnalyse found **0** tables (its masks
require exactly-consecutive Sonic-toolchain sequences). `tools/ea_jumptables.py`
does a structural, nop-tolerant backward walk and finds **505 tables /
14,718 labels** — matching the dispatch-pattern census exactly.*

## Census of all 1,911 `bctr` sites in `.text`

| Class | Count | Handling |
|---|---|---|
| Switch dispatch (byte-offset table) | 387 | `[[switch]]` TOML |
| Switch dispatch (halfword-offset table) | 118 | `[[switch]]` TOML |
| C++ vtable / function-pointer dispatch | 1,370 | recompiler indirect call (no annotation) |
| Indirect tail calls (arg setup between `mtctr`/`bctr`) | 36 | recompiler indirect call |

## The EA dispatch shape (vs. Sonic-era)

```asm
cmplwi  cr6, rIDX, N          ; bounds check, sometimes far above or in a
bgt     cr6, default          ;   *different block* (see below)
...
lis     r12, tbl@ha
addi    r12, r12, tbl@l       ; nops interleaved anywhere in here —
lbzx    r0, r12, rIDX         ;   this is what breaks XenonAnalyse's
[slwi   r0, r0, 2]            ;   exact-sequence SearchMask
lis     r12, base@ha
nop
addi    r12, r12, base@l
nop
add     r12, r12, r0
mtctr   r12
bctr                          ; labels[i] = base + (table[i] << shift)
```

Halfword variant: `slwi rX, rIDX, 1` + `lhzx`, no post-load shift.
No absolute (`lwzx`) tables exist in this binary.

## Edge cases found

1. **Shared bounds check, branched-to dispatch** (1 site, `0x82D66EB8`):
   one `cmplwi/bgt` guards two dispatch copies; a `beq` jumps into the second,
   so no bounds check exists linearly above its `bctr`. Handled by
   `bounds_via_xref()` — find the conditional branch into the dispatch window,
   read the `cmplwi` at the branch source.
2. **Branch prediction hints**: EA emits `bgt+`/`ble-` (BO hint bit) — masks
   must test `BO & ~1`.

## TOML contract with XenonRecomp (verified in source)

- `recompiler_config.cpp:90` — entries keyed by `base`; only `r` and `labels`
  are consumed (`default` ignored).
- `recompiler.cpp:2412` — entry is armed when the instruction cursor reaches
  `base`; we emit `base = <address of the bctr>` which is always inside the
  function.
- `r` must hold the **raw case index** at the `bctr` — for halfword tables we
  unwind the `slwi rX, rIDX, 1` pre-scale back to `rIDX`.
- `recompiler.cpp:2427` warns at recompile time about any `bctr` that looks
  like a switch but has no entry — our Phase 2 safety net.

## Upstreaming

The structural detector should be ported into XenonAnalyse (C++) as a PR:
nop-tolerant SearchMask + role-based operand extraction. Tracked as post-MVP;
the Python tool is authoritative for this project until then.

## Confidence

- High: pattern coverage (505/505 of the census; label sanity-checked to be
  4-aligned, inside `.text`).
- Residual risk: tables whose dispatch uses registers other than the
  `lis/addi` materialization idiom (e.g. table address from a struct field).
  None observed; XenonRecomp's recompile-time warning will catch any.
