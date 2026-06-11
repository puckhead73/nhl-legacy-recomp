"""Find stores to field +0x100 of the singleton at 0x83E021E0.

Two addressing patterns:
 A) lis rB,0x83E0 ; addi rB2,rB,0x21E0 ; ... stw rX,0x100(rB2)
 B) any function: stw rX,0x100(rY)  -- list all, with the enclosing function
    start (heuristic: previous mflr) so we can cross-check candidates whose
    rY plausibly holds the singleton (manual review).
Pattern A first; if none, dump pattern-B sites within the module VA range
0x83070000-0x83090000 plus any whose store reg was just loaded from a
lis-83E0-based field.
"""
import struct, sys

data = open(sys.argv[1], "rb").read()
BASE = 0x82000000

def u32(off):
    return struct.unpack_from(">I", data, off)[0]

print("-- pattern A: lis/addi 0x83E021E0 then stw rX,0x100(obj) --")
hits = 0
for off in range(0, len(data) - 4, 4):
    w = u32(off)
    if (w & 0xFC1F0000) != 0x3C000000 or (w & 0xFFFF) != 0x83E0:
        continue
    rlis = (w >> 21) & 31
    objreg = None
    for k in range(1, 13):
        wk = u32(off + 4 * k)
        if (wk >> 26) == 14 and ((wk >> 16) & 31) == rlis and (wk & 0xFFFF) == 0x21E0:
            objreg = (wk >> 21) & 31
            astart = k
            break
    if objreg is None:
        continue
    for k in range(astart + 1, astart + 24):
        wk = u32(off + 4 * k)
        op = wk >> 26
        if op in (36, 37) and ((wk >> 16) & 31) == objreg and (wk & 0xFFFF) == 0x100:
            print(f"  stw r{(wk>>21)&31},0x100(r{objreg}) @ {BASE+off+4*k:08X} (lis @ {BASE+off:08X})")
            hits += 1
        if wk == 0x4E800020:  # blr
            break
print(hits, "pattern-A hits")

print("\n-- pattern B: all stw rX,0x100(rY) in 0x83070000-0x83090000 --")
for off in range(0x1070000, 0x1090000, 4):
    w = u32(off)
    if (w >> 26) == 36 and (w & 0xFFFF) == 0x100:
        print(f"  stw r{(w>>21)&31},0x100(r{(w>>16)&31}) @ {BASE+off:08X}")

print("\n-- pattern C: all stw rX,0x100(rY) anywhere (count + first 40) --")
sites = []
for off in range(0, len(data) - 4, 4):
    w = u32(off)
    if (w >> 26) == 36 and (w & 0xFFFF) == 0x100:
        sites.append((off, w))
print(len(sites), "total")
for off, w in sites[:40]:
    print(f"  stw r{(w>>21)&31},0x100(r{(w>>16)&31}) @ {BASE+off:08X}")
