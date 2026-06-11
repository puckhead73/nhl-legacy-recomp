"""Find code addresses referenced from data sections (vtables/callback tables)
that the ReXGlue scanner did not register as functions.

Inputs:
  - flat decompressed image (xex extract output)
  - generated nhllegacy_register.cpp (registrar->SetFunction(0xADDR, ...) lines)

Output: TOML [functions] lines for candidates whose first instruction looks
like a plausible function start and whose preceding word looks like a
function boundary (blr/b/bctr/padding) — conservative filter.
"""
import re
import struct
import sys

IMAGE = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\puckh\AppData\Local\Temp\nhl_legacy_image.bin"
REGISTER = sys.argv[2] if len(sys.argv) > 2 else r"E:\Repositories\nhl-legacy-recomp\generated\default\nhllegacy_register.cpp"

BASE = 0x82000000
CODE_LO, CODE_HI = 0x82450000, 0x8398E530
# Data ranges from ReXGlue BinaryView log (va, size)
DATA_RANGES = [
    (0x82000400, 0x3B03CC),  # .rdata
    (0x83990000, 0x472800),  # .data
]

img = open(IMAGE, "rb").read()

known = set()
for m in re.finditer(rb"SetFunction\(0x([0-9A-Fa-f]{8})", open(REGISTER, "rb").read()):
    known.add(int(m.group(1), 16))
print(f"known functions: {len(known)}", file=sys.stderr)

def word(va):
    off = va - BASE
    if off < 0 or off + 4 > len(img):
        return None
    return struct.unpack(">I", img[off:off + 4])[0]

def plausible_start(w):
    if w is None or w == 0:
        return False
    op = w >> 26
    if w == 0x7D8802A6:  # mflr r12
        return True
    if (w & 0xFFFF0000) == 0x94210000:  # stwu r1,-X(r1)
        return True
    if op in (14, 15):  # addi/lis (li/lis)
        return True
    if op in (32, 33, 34, 35, 40, 42):  # lwz/lwzu/lbz/lbzu/lhz/lha
        return True
    if op in (36, 38, 44):  # stw/stb/sth
        return True
    if w == 0x4E800020:  # blr (stub fn)
        return True
    if op == 18:  # b/bl
        return True
    if op in (10, 11):  # cmpli/cmpi
        return True
    if op == 31 and ((w >> 1) & 0x3FF) in (444, 266, 40):  # or(mr)/add/subf
        return True
    return False

def boundary_before(va):
    w = word(va - 4)
    if w is None:
        return False
    if w == 0:
        return True
    if w == 0x4E800020 or w == 0x4E800420:  # blr / bctr
        return True
    op = w >> 26
    if op == 18 and (w & 1) == 0:  # unconditional b (tail), no link
        return True
    if op == 19 and ((w >> 1) & 0x3FF) == 16:  # bclr
        return True
    return False

cands = {}
for (lo, size) in DATA_RANGES:
    for va in range(lo & ~3, (lo + size) & ~3, 4):
        w = word(va)
        if w is None:
            continue
        if CODE_LO <= w < CODE_HI and (w & 3) == 0 and w not in known:
            if plausible_start(word(w)) and boundary_before(w):
                cands.setdefault(w, []).append(va)

print(f"candidates: {len(cands)}", file=sys.stderr)
for t in sorted(cands):
    refs = ",".join(f"{r:08X}" for r in cands[t][:3])
    print(f'0x{t:08X} = {{ name = "sub_{t:08X}" }}  # data refs: {refs}')
