"""Find lis/addi- and lis/ori-materialized code addresses that are not in the
generated SetFunction registry — runtime-computed function pointers the
ReXGlue scanner cannot see (the class behind boot fatals like 0x827747D0).

Same plausibility filters as find_missing_indirect_targets.py:
  - target 4-aligned, inside the code range
  - word before target ends a function (blr / b / bctr / 0) OR target has a
    classic prologue (mflr/stwu/...)
Usage:
  python find_missing_materialized_targets.py <flat_image> <register_cpp>
Emits TOML lines for nhllegacy_functions.toml.
"""
import re
import struct
import sys

IMG, REG = sys.argv[1], sys.argv[2]
data = open(IMG, "rb").read()
BASE = 0x82000000
CODE_LO, CODE_HI = 0x82450000, 0x8398E530

def u32(off):
    return struct.unpack_from(">I", data, off)[0]

registered = set(
    int(m, 16)
    for m in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+)", open(REG).read())
)
print(f"# registry: {len(registered)} functions", file=sys.stderr)

def plausible_start(va):
    off = va - BASE
    prev = u32(off - 4)
    w = u32(off)
    # boundary before: blr, bctr, b, or padding zero
    boundary = (
        prev == 0x4E800020
        or prev == 0x4E800420
        or (prev >> 26) == 18
        or prev == 0
    )
    op = w >> 26
    # prologue-ish first instruction: mflr r12 (7D8802A6) / mflr r0 (7C0802A6),
    # stwu r1, addi r12/r1, or common register moves/compares of args
    prologue = w in (0x7D8802A6, 0x7C0802A6) or op in (37,) or (
        op == 14 and ((w >> 16) & 31) == 1
    )
    return boundary or prologue

cands = {}
for off in range(0, len(data) - 8, 4):
    w = u32(off)
    if (w & 0xFC1F0000) != 0x3C000000:  # lis rD (addis rD,r0,imm)
        continue
    rd = (w >> 21) & 31
    hi = w & 0xFFFF
    for k in range(1, 10):
        wk = u32(off + 4 * k)
        op = wk >> 26
        # addi rY, rd, lo  (signed)
        if op == 14 and ((wk >> 16) & 31) == rd:
            lo = wk & 0xFFFF
            if lo >= 0x8000:
                lo -= 0x10000
            tgt = ((hi << 16) + lo) & 0xFFFFFFFF
        # ori rY, rd, lo  (unsigned)
        elif op == 24 and ((wk >> 21) & 31) == rd:
            lo = wk & 0xFFFF
            tgt = ((hi << 16) | lo) & 0xFFFFFFFF
        else:
            # stop tracking once rd is overwritten by another lis
            if (wk & 0xFC1F0000) == 0x3C000000 and ((wk >> 21) & 31) == rd:
                break
            continue
        if CODE_LO <= tgt < CODE_HI and tgt % 4 == 0 and tgt not in registered:
            # Exclude switch-table bases: materialized reg feeds an
            # add rD,rD,rX / mtctr / bctr dispatch within a few instructions
            # (classic Xenon jump-table pattern; belongs to [[switch_tables]],
            # not [functions]).
            ry = (wk >> 21) & 31 if op == 14 else (wk >> 16) & 31
            is_switch = False
            for j in range(k + 1, k + 6):
                wj = u32(off + 4 * j)
                if wj == 0x4E800420:  # bctr terminates the window
                    is_switch = True
                    break
                if wj == 0x4E800021 or (wj >> 26) in (16, 18):  # bctrl/bc/b
                    break
            if not is_switch and plausible_start(tgt):
                cands.setdefault(tgt, []).append(BASE + off)
        break  # only the first derived constant per lis

print(f"# {len(cands)} candidate targets", file=sys.stderr)
for tgt in sorted(cands):
    refs = ",".join(f"{r:08X}" for r in cands[tgt][:3])
    print(f'0x{tgt:08X} = {{ name = "sub_{tgt:08X}" }}  # materialized at: {refs}')
