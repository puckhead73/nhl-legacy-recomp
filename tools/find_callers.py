#!/usr/bin/env python3
"""Find bl/b call sites of a target VA in the flat NHL Legacy image."""
import struct, sys

IMAGE_BASE = 0x82000000
CODE_LO, CODE_HI = 0x82450000, 0x8398E530

def main():
    path, target = sys.argv[1], int(sys.argv[2], 16)
    data = open(path, 'rb').read()
    lo_off = CODE_LO - IMAGE_BASE
    hi_off = min(CODE_HI - IMAGE_BASE, len(data))
    hits = []
    for off in range(lo_off, hi_off, 4):
        insn = struct.unpack_from('>I', data, off)[0]
        if (insn >> 26) != 18:  # b/bl (I-form)
            continue
        li = insn & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        aa = insn & 2
        dest = li if aa else (IMAGE_BASE + off + li)
        if dest == target:
            va = IMAGE_BASE + off
            lk = insn & 1
            hits.append((va, 'bl' if lk else 'b'))
    for va, kind in hits:
        print(f"{va:08X}  {kind} -> {target:08X}")
    print(f"total: {len(hits)}")

if __name__ == '__main__':
    main()
