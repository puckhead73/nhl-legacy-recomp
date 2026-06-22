#!/usr/bin/env python3
"""Stage-failure histogram for ea_jumptables.analyze_site."""
import collections
import struct
import sys

sys.path.insert(0, "tools")
import ea_jumptables as jt

data = open("extracted/nhlzf_image.bin", "rb").read()

sites = []
for off in range(jt.TEXT_VA - jt.IMAGE_BASE, jt.TEXT_END - jt.IMAGE_BASE, 4):
    if struct.unpack_from(">I", data, off)[0] == jt.BCTR:
        sites.append(jt.IMAGE_BASE + off)

# Re-run analyze, but classify failure stage by probing structure manually.
stages = collections.Counter()
fail_examples = collections.defaultdict(list)

for va in sites:
    insns = jt.walk_back(data, va, jt.MAX_WALK)
    i = 0
    while i < len(insns) and jt.is_nop(insns[i].w):
        i += 1
    if i >= len(insns) or not jt.is_mtctr(insns[i].w):
        stages["1:no-mtctr"] += 1
        continue
    rS = jt.rd(insns[i].w)
    i += 1
    # find add
    add_idx = None
    for j in range(i, len(insns)):
        w = insns[j].w
        if jt.is_add(w) and jt.rd(w) == rS:
            add_idx = j
            break
        if not jt.is_nop(w) and jt.writes_gpr(w) == rS:
            break
    if add_idx is None:
        stages["2:no-add"] += 1
        continue
    a, b = jt.ra(insns[add_idx].w), jt.rb(insns[add_idx].w)
    # find load
    load_idx = None
    for j in range(add_idx + 1, len(insns)):
        w = insns[j].w
        if (jt.is_lbzx(w) or jt.is_lhzx(w)) and jt.rd(w) in (a, b):
            load_idx = j
            break
    if load_idx is None:
        stages["3:no-load"] += 1
        if len(fail_examples["3:no-load"]) < 3:
            fail_examples["3:no-load"].append(va)
        continue
    r = jt.analyze_site(data, va)
    if r is None:
        stages["4-8:late-fail"] += 1
        if len(fail_examples["4-8:late-fail"]) < 10:
            fail_examples["4-8:late-fail"].append(va)
    else:
        stages["ok"] += 1

for k, v in sorted(stages.items()):
    print(f"{k:>15}: {v}")
for k, vs in fail_examples.items():
    print(f"{k} examples: {['%X' % v for v in vs]}")
