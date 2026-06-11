"""Map the per-module slot-page system:
- trampolines: lis rX,page / lwz r0,d(rX) / cmpwi r0,0 / mtctr r0 / bnectr
- installers:  lis r10,page / ... / stw r11,d(r10) / b <common>
Group both by page, report coverage gaps (pages trampolines read but no
installer writes).
"""
import struct, sys

data = open(sys.argv[1], "rb").read()
BASE = 0x82000000

def u32(off):
    return struct.unpack_from(">I", data, off)[0]

tramp = {}
for off in range(0, len(data) - 20, 4):
    w0, w1, w2, w3, w4 = (u32(off + 4 * k) for k in range(5))
    if (w0 & 0xFC1F0000) == 0x3C000000:  # lis rD,imm (rA=0)
        rd = (w0 >> 21) & 31
        if (w1 >> 26) == 32 and ((w1 >> 16) & 31) == rd and ((w1 >> 21) & 31) == 0:
            if w2 == 0x2C000000 and w3 == 0x7C0903A6 and w4 == 0x4C820420:
                page = (w0 & 0xFFFF) << 16
                tramp.setdefault(page, []).append((BASE + off, w1 & 0xFFFF))

print("TRAMPOLINE pages:")
for p in sorted(tramp):
    vs = tramp[p]
    print(f"  page {p:08X}: {len(vs):5d} tramps, disp {min(d for _, d in vs):#07x}-"
          f"{max(d for _, d in vs):#07x}, va {min(v for v, _ in vs):08X}-{max(v for v, _ in vs):08X}")
print("total trampolines:", sum(len(v) for v in tramp.values()))

inst = {}
for off in range(0, len(data) - 24, 4):
    w0 = u32(off)
    if (w0 & 0xFC1F0000) != 0x3C000000:
        continue
    rd = (w0 >> 21) & 31
    for k in range(1, 6):
        wk = u32(off + 4 * k)
        if (wk >> 26) == 36 and ((wk >> 16) & 31) == rd:
            wb = u32(off + 4 * (k + 1))
            if (wb >> 26) == 18:  # b
                page = (w0 & 0xFFFF) << 16
                if page >= 0x83000000:
                    inst.setdefault(page, []).append((BASE + off, wk & 0xFFFF))
            break

print("\nINSTALLER pages:")
for p in sorted(inst):
    vs = inst[p]
    print(f"  page {p:08X}: {len(vs):5d} installers, disp {min(d for _, d in vs):#07x}-"
          f"{max(d for _, d in vs):#07x}, va {min(v for v, _ in vs):08X}-{max(v for v, _ in vs):08X}")
print("total installers:", sum(len(v) for v in inst.values()))

tp = set(tramp)
ip = set(inst)
print("\npages with trampolines but NO installers:", [f"{p:08X}" for p in sorted(tp - ip)])
print("pages with installers but NO trampolines:", [f"{p:08X}" for p in sorted(ip - tp)])
# per-page slot coverage for pages in both
for p in sorted(tp & ip):
    tslots = set(d for _, d in tramp[p])
    islots = set(d for _, d in inst[p])
    missing = tslots - islots
    print(f"page {p:08X}: tramp slots {len(tslots)}, installed {len(islots)}, "
          f"tramp-slots-without-installer {len(missing)}"
          + (f" e.g. {sorted(missing)[:5]}" if missing else ""))
