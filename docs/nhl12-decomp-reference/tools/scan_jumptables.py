#!/usr/bin/env python3
"""Characterize EA-toolchain jump-table dispatch patterns in nhlzf.

Scans .text of the decrypted flat image for `bctr` (0x4E800420), disassembles
a window of preceding instructions, and histograms the mnemonic sequences so
we can derive detection masks for XenonAnalyse.

Usage: python scan_jumptables.py extracted/nhlzf_image.bin [--dump N] [--win N]
"""

import argparse
import collections
import struct
import sys

from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

IMAGE_BASE = 0x82000000
TEXT_VA = 0x82580000
TEXT_SIZE = 0x01158BD0
BCTR = 0x4E800420
BCTRL = 0x4E800421

md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)


def disasm_window(data, va, count):
    """Disassemble `count` instructions ending just before va (the bctr)."""
    start_va = va - count * 4
    off = start_va - IMAGE_BASE
    insns = []
    for i in range(count):
        word = data[off + i * 4 : off + i * 4 + 4]
        dec = list(md.disasm(word, start_va + i * 4))
        if dec:
            insns.append((dec[0].mnemonic, dec[0].op_str))
        else:
            insns.append((f".long_{struct.unpack('>I', word)[0]:08X}", ""))
    return insns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--win", type=int, default=8)
    ap.add_argument("--dump", type=int, default=0,
                    help="print N example windows for the top patterns")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    text_off = TEXT_VA - IMAGE_BASE

    sites = []
    for off in range(text_off, text_off + TEXT_SIZE, 4):
        w = struct.unpack_from(">I", data, off)[0]
        if w == BCTR:
            sites.append(IMAGE_BASE + off)

    print(f"bctr sites in .text: {len(sites)}")

    # Histogram the preceding mnemonic sequences.
    patterns = collections.Counter()
    examples = collections.defaultdict(list)
    for va in sites:
        win = disasm_window(data, va, args.win)
        key = " | ".join(m for m, _ in win)
        patterns[key] += 1
        if len(examples[key]) < 3:
            examples[key].append((va, win))

    print(f"distinct {args.win}-insn windows: {len(patterns)}\n")
    for i, (key, n) in enumerate(patterns.most_common(25)):
        print(f"[{n:5d}x] {key}")
        if i < args.dump:
            va, win = examples[key][0]
            print(f"    example @ {va:08X} (bctr at end):")
            for j, (m, ops) in enumerate(win):
                print(f"      {va - (args.win - j) * 4:08X}  {m:<10} {ops}")
        print()


if __name__ == "__main__":
    main()
