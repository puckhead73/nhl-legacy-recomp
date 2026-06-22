import struct

data = open("extracted/nhlzf_image.bin", "rb").read()
BASE = 0x82000000
PDATA_VA, PDATA_SZ = 0x824EE200, 0x83560
dups = [int(l, 16) for l in open("docs/dup_bases.txt")]

pd = {}
for off in range(0, PDATA_SZ, 8):
    b, d = struct.unpack_from(">II", data, PDATA_VA - BASE + off)
    pd.setdefault(b, []).append(((d >> 8) & 0x3FFFFF) * 4)

TVA, TSZ = 0x82580000, 0x01158BD0
bl = set()
for off in range(TVA - BASE, TVA - BASE + TSZ, 4):
    w = struct.unpack_from(">I", data, off)[0]
    if (w >> 26) == 18 and (w & 1) and not (w & 2):
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        bl.add(BASE + off + li)

for a in dups:
    word = struct.unpack_from(">I", data, a - BASE)[0]
    print(f"{a:08X}: pdata={pd.get(a)} bl_target={a in bl} word={word:08X}")
