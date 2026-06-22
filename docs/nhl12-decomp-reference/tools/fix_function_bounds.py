#!/usr/bin/env python3
"""Compute manual function-boundary entries for switches XenonRecomp truncates.

EA's toolchain omits .pdata entries for leaf functions; XenonRecomp's fallback
scan terminates such functions at the first (unannotated-at-analysis-time)
bctr, so switch case bodies land outside function bounds (the 3,766
"Switch case ... outside function" errors).

For every switch in switch_tables.toml whose bctr+labels aren't covered by a
single .pdata function, emit a manual entry spanning from the nearest known
function start at-or-below the bctr to the nearest known function start above
the highest label. Known starts = .pdata begins + bl targets + millicode +
entry point. Overlapping spans are merged.

Output: TOML `functions` array lines, written into config/functions_fix.toml
(spliced into nhl12.toml by hand/sed).
"""

import bisect
import re
import struct

IMAGE = "extracted/nhlzf_image.bin"
SWITCHES = "config/switch_tables.toml"
OUT = "config/functions_fix.toml"

BASE = 0x82000000
TEXT_VA, TEXT_SIZE = 0x82580000, 0x01158BD0
TEXT_END = TEXT_VA + TEXT_SIZE
PDATA_VA, PDATA_SZ = 0x824EE200, 0x83560
ENTRY = 0x828588A8
MILLICODE = [0x8285C610, 0x8285C660, 0x8335C9A0, 0x8335C9EC,
             0x8335CA40, 0x8335CCD8, 0x8335CAD4, 0x8335CD6C]

data = open(IMAGE, "rb").read()

# --- .pdata map ---
pdata = []
for off in range(0, PDATA_SZ, 8):
    begin, d = struct.unpack_from(">II", data, PDATA_VA - BASE + off)
    flen = ((d >> 8) & 0x3FFFFF) * 4
    if begin:
        pdata.append((begin, flen))
pdata.sort()
pdata_begins = [b for b, _ in pdata]

# --- all bl targets in .text ---
starts = set(pdata_begins) | set(MILLICODE) | {ENTRY}
for off in range(TEXT_VA - BASE, TEXT_END - BASE, 4):
    w = struct.unpack_from(">I", data, off)[0]
    if (w >> 26) == 18 and (w & 1) and not (w & 2):  # bl, relative
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        tgt = BASE + off + li
        if TEXT_VA <= tgt < TEXT_END:
            starts.add(tgt)
F = sorted(starts)

# --- parse switch tables ---
switches = []
cur = None
for line in open(SWITCHES):
    if line.startswith("[[switch]]"):
        cur = {"labels": []}
        switches.append(cur)
    elif cur is not None:
        m = re.match(r"base = (0x[0-9A-Fa-f]+)", line)
        if m:
            cur["base"] = int(m.group(1), 16)
        m = re.match(r"default = (0x[0-9A-Fa-f]+)", line)
        if m:
            cur["default"] = int(m.group(1), 16)
        m = re.match(r"\s+(0x[0-9A-Fa-f]+),", line)
        if m:
            cur["labels"].append(int(m.group(1), 16))


def pdata_covering(addr):
    i = bisect.bisect_right(pdata_begins, addr) - 1
    if i >= 0:
        b, l = pdata[i]
        if b <= addr < b + l:
            return b, l
    return None


spans = []
uncovered = 0
for sw in switches:
    site = sw["base"]
    hi = max(sw["labels"] + ([sw["default"]] if sw.get("default") else []))
    cov = pdata_covering(site)
    if cov and site < cov[0] + cov[1] and hi < cov[0] + cov[1]:
        continue  # fully inside a pdata function — fine
    uncovered += 1
    i = bisect.bisect_right(F, site) - 1
    start = F[i]
    j = bisect.bisect_right(F, hi)
    end = F[j] if j < len(F) else TEXT_END
    inner = [f for f in F[i + 1 : j] if f > site]
    if inner:
        print(f"WARN: known starts inside span of switch {site:08X}: "
              f"{[hex(x) for x in inner[:4]]} — spanning past them")
    spans.append((start, end))

# merge overlaps
spans.sort()
merged = []
for s, e in spans:
    if merged and s <= merged[-1][1]:
        merged[-1] = (merged[-1][0], max(merged[-1][1], e))
    else:
        merged.append((s, e))

with open(OUT, "w") as f:
    f.write("functions = [\n")
    for s, e in merged:
        f.write(f"    {{ address = 0x{s:X}, size = 0x{e - s:X} }},\n")
    f.write("]\n")

print(f"switches needing bounds fix: {uncovered}/{len(switches)}")
print(f"manual function spans (merged): {len(merged)}")
print(f"wrote {OUT}")
