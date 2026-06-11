"""Find the registration thunk that installs the handler at 0x83DA22E0.

The trampoline sub_830801C0 loads [base 0x83DA0000 + 8928] and errors when
zero. Installer thunks (12,407 of them at end of .text) materialize a module
base via lis/addis and store a handler pointer at +8928, then b sub_8256A238.

Scan the flat image (xex extract output, VA = 0x82000000 + offset) for:
  lis rN, 0x83DA   (addis rN, r0, 0x83DA  -> 0x3C00_0000 | N<<21 | 0x83DA... )
followed within a window by a stw with displacement 0x22E0 using rN (or a
reg derived from it).
"""
import struct, sys

IMG = sys.argv[1] if len(sys.argv) > 1 else None
data = open(IMG, "rb").read()
BASE = 0x82000000

def u32(off):
    return struct.unpack_from(">I", data, off)[0]

hits = []
for off in range(0, len(data) - 4, 4):
    w = u32(off)
    # addis rD, r0, 0x83DA  => opcode 15 (0x3C000000), rA=0, imm=0x83DA
    if (w & 0xFC1FFFFF) == (0x3C000000 | 0x000083DA & 0xFFFF):
        rd = (w >> 21) & 31
        # window: next 8 instructions, look for stw rS, 0x22E0(rd)
        for k in range(1, 9):
            w2 = u32(off + 4 * k)
            if (w2 >> 26) == 36 and ((w2 >> 16) & 31) == rd and (w2 & 0xFFFF) == 0x22E0:
                hits.append((BASE + off, rd, BASE + off + 4 * k, w2))
                break

for va, rd, sva, w2 in hits:
    rs = (w2 >> 21) & 31
    print(f"lis r{rd},0x83DA @ {va:08X}  ...  stw r{rs},0x22E0(r{rd}) @ {sva:08X}")
print(f"{len(hits)} hit(s)")

# Also: any instruction with displacement 0x22E0 at all (loads or stores),
# to catch addi-based or different-base encodings.
print("\n-- all d-form ops with disp 0x22E0 touching a reg lis'd 0x83DA nearby --")
cnt = 0
for off in range(0, len(data) - 4, 4):
    w = u32(off)
    op = w >> 26
    if (w & 0xFFFF) == 0x22E0 and op in (32, 33, 36, 37, 14, 15):  # lwz/lwzu/stw/stwu/addi/addis
        # check previous 8 instructions for lis rA, 0x83DA
        ra = (w >> 16) & 31
        ctx = []
        found = False
        for k in range(1, 9):
            if off - 4 * k < 0: break
            wp = u32(off - 4 * k)
            if (wp & 0xFC1FFFFF) == 0x3C0083DA and ((wp >> 21) & 31) == ra:
                found = True
                break
        if found:
            cnt += 1
            print(f"  {['','','','','','','','','','','','','','','addi','addis'][op] if op in (14,15) else {32:'lwz',33:'lwzu',36:'stw',37:'stwu'}[op]}"
                  f" disp 0x22E0 base r{ra} @ {BASE+off:08X} (word {w:08X})")
print(f"{cnt} contextual hit(s)")
