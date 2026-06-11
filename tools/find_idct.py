#!/usr/bin/env python3
"""From the VP6 decode proc, walk the static call graph and score each
reachable function for IDCT-like structure (dense integer shift/add butterfly
arithmetic). The per-block inverse transform is the hottest such function."""
import struct
import sys
import bisect
import re

IMAGE_BASE = 0x82000000
CODE_LO, CODE_HI = 0x82450000, 0x8398E530

data = open(sys.argv[1] if len(sys.argv) > 1
            else 'C:/Users/puckh/AppData/Local/Temp/nhl_legacy_image.bin',
            'rb').read()
reg = open('E:/Repositories/nhl-legacy-recomp/generated/default/'
           'nhllegacy_register.cpp').read()
addrs = sorted({int(m, 16) for m in
                re.findall(r'SetFunction\(0x([0-9A-Fa-f]+)', reg)})


def fn_start(va):
    i = bisect.bisect_right(addrs, va) - 1
    return addrs[i]


def fn_end(va):
    s = fn_start(va)
    i = bisect.bisect_right(addrs, s)
    return addrs[i] if i < len(addrs) else CODE_HI


def callees(fn):
    """direct bl targets within a function."""
    out = set()
    s, e = fn, fn_end(fn)
    for va in range(s, e, 4):
        insn = struct.unpack_from('>I', data, va - IMAGE_BASE)[0]
        if (insn >> 26) == 18 and (insn & 1):
            li = insn & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            dest = va + li
            if CODE_LO <= dest < CODE_HI:
                out.add(fn_start(dest))
    return out


def score(fn):
    """IDCT-likeness: shift instructions, no calls, tight, has 8/64 const."""
    s, e = fn, fn_end(fn)
    if e - s > 0x800:
        return None  # IDCT kernels are small
    shifts = adds = loads = stores = calls = 0
    consts = set()
    for va in range(s, e, 4):
        insn = struct.unpack_from('>I', data, va - IMAGE_BASE)[0]
        op = insn >> 26
        imm = insn & 0xFFFF
        if op == 21 or op == 23 or op == 30:  # rlwinm/rlwnm/rld
            shifts += 1
        elif op == 31 and ((insn >> 1) & 0x3FF) in (
                824, 792, 536, 27, 28, 412):  # srawi/sraw/srw/slw...
            shifts += 1
        elif op in (14, 12, 13):  # addi/addic
            adds += 1
            if imm in (8, 64, 128, 4, 0x40):
                consts.add(imm)
        elif op in (32, 40, 34, 42):  # lwz/lhz/lbz
            loads += 1
        elif op in (36, 44, 38):  # stw/sth/stb
            stores += 1
        elif op == 18 and (insn & 1):
            calls += 1
    if calls > 2 or shifts < 4:
        return None
    return {'fn': fn, 'sz': e - s, 'shifts': shifts, 'adds': adds,
            'loads': loads, 'stores': stores, 'calls': calls,
            'consts': sorted(consts)}


def main():
    roots = [int(x, 16) for x in sys.argv[2:]] or [0x8277ABB8]
    seen = set()
    work = list(roots)
    while work:
        fn = work.pop()
        if fn in seen:
            continue
        seen.add(fn)
        for c in callees(fn):
            if c not in seen and 0x82760000 <= c < 0x82790000:
                work.append(c)
    print(f'{len(seen)} functions reachable in codec module')
    scored = [s for s in (score(f) for f in sorted(seen)) if s]
    scored.sort(key=lambda d: -(d['shifts'] + d['adds']))
    for d in scored[:18]:
        print('sub_%08X sz=%4d sh=%2d add=%2d ld=%2d st=%2d call=%d c=%s'
              % (d['fn'], d['sz'], d['shifts'], d['adds'], d['loads'],
                 d['stores'], d['calls'], d['consts']))


if __name__ == '__main__':
    main()
