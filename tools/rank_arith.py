#!/usr/bin/env python3
"""Rank all functions in given generated .cpp files by fixed-point arithmetic
density (mullw + shifts), to locate the VP6 inverse-transform/dequant kernel."""
import os, re, sys, glob

ARITH = re.compile(r'//\s*(mullw|srawi|sraw|slw|srw|rlwinm|rlwimi|mulli|mulhw)\b')
SHIFT = re.compile(r'//\s*(srawi|sraw|slw|srw)\b')
MUL = re.compile(r'//\s*(mullw|mulli|mulhw)\b')
ADD = re.compile(r'//\s*(add|subf)\b')
DEFRE = re.compile(r'DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]+)\)')

rows = []
for f in sys.argv[1:]:
    lines = open(f, encoding='utf-8', errors='ignore').read().splitlines()
    cur = None
    buf = []
    for l in lines:
        m = DEFRE.search(l)
        if m:
            if cur:
                t = '\n'.join(buf)
                rows.append((cur, os.path.basename(f), len(buf),
                             len(MUL.findall(t)), len(SHIFT.findall(t)),
                             len(ADD.findall(t))))
            cur = m.group(1); buf = []
        if cur is not None:
            buf.append(l)
    if cur:
        t = '\n'.join(buf)
        rows.append((cur, os.path.basename(f), len(buf),
                     len(MUL.findall(t)), len(SHIFT.findall(t)), len(ADD.findall(t))))

rows.sort(key=lambda r: (r[3] + r[4]), reverse=True)
print(f"{'func':18s} {'file':28s} {'lines':>6} {'mul':>5} {'shift':>6} {'add':>5}  score")
for name, fn, nl, mul, sh, add in rows[:25]:
    print(f"{name:18s} {fn:28s} {nl:6d} {mul:5d} {sh:6d} {add:5d}  {mul+sh}")
