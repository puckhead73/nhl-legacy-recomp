"""Find address-of computations near 0x83E022E0: lis rD,0x83E0 followed by
addi rX,rD,imm with imm in [0x2000,0x2400] — i.e. code taking the address of
a global object whose fields could include the 0x22E0 slot. Also list every
d-form load/store displacement used with a lis-0x83E0 base in [0x2200,0x2400]
to sketch the object layout.
"""
import struct, sys

data = open(sys.argv[1], "rb").read()
BASE = 0x82000000

def u32(off):
    return struct.unpack_from(">I", data, off)[0]

print("-- addi rX, rLis83E0, imm in [0x1F00,0x2400] (address-of) --")
for off in range(0, len(data) - 4, 4):
    w = u32(off)
    if (w & 0xFC1F0000) != 0x3C000000 or (w & 0xFFFF) != 0x83E0:
        continue
    rd = (w >> 21) & 31
    for k in range(1, 12):
        wk = u32(off + 4 * k)
        if (wk >> 26) == 14 and ((wk >> 16) & 31) == rd:  # addi rX, rd, imm
            imm = wk & 0xFFFF
            if imm >= 0x8000:
                imm -= 0x10000
            if 0x1F00 <= imm <= 0x2400:
                print(f"  addi r{(wk>>21)&31}, r{rd}, {imm:#x} -> EA {0x83E00000+imm:08X} @ {BASE+off+4*k:08X}")
        # stop window at a branch
        if (wk >> 26) in (18, 16) or wk == 0x4E800020:
            break

print("\n-- d-form refs with lis-0x83E0 base, disp in [0x2200,0x2400] --")
opn = {32:'lwz',33:'lwzu',34:'lbz',36:'stw',37:'stwu',38:'stb',40:'lhz',44:'sth',14:'addi',48:'lfs',50:'lfd',52:'stfs',54:'stfd'}
for off in range(0, len(data) - 4, 4):
    w = u32(off)
    op = w >> 26
    if op not in opn:
        continue
    d = w & 0xFFFF
    if not (0x2200 <= d <= 0x2400):
        continue
    ra = (w >> 16) & 31
    if ra == 0:
        continue
    for k in range(1, 17):
        if off - 4 * k < 0:
            break
        wp = u32(off - 4 * k)
        if (wp & 0xFC1F0000) == 0x3C000000 and ((wp >> 21) & 31) == ra:
            if (wp & 0xFFFF) == 0x83E0:
                print(f"  {opn[op]} r{(w>>21)&31}, {d:#x}(r{ra}) -> EA {0x83E00000+d:08X} @ {BASE+off:08X}")
            break
