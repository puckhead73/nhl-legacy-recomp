#!/usr/bin/env python3
"""Locate the franchise/season-sim engine (GmModeData.cpp, NameGenerator.cpp,
InjuryGen.cpp, draft/progression/sim code) inside the recompiled NHL Legacy image.

Functions in this recomp are named purely by address (sub_XXXXXXXX); there are no
symbols. The handle on a *named* module is the assert/__FILE__ path strings EA left
in .rodata. Pipeline:

  Stage 1 - scan the decompressed guest image for ASCII C-strings whose text names
            franchise modules/concepts, and report each string's guest VA.
            (VA = IMAGE_BASE + file_offset; the dump is a flat image.)
  Stage 2 - find the functions that load each interesting string VA. Addresses are
            NOT inline hex in the translated C++; xenos-recomp emits them as DECIMAL
            lis/addi/ori pairs, e.g.
                // lis r11,-32205         ctx.r11.s64 = -2110586880;   (= 0x82330000)
                // addi r9,r11,-9012      ctx.r9.s64  = ctx.r11.s64 + -9012;
            So we reconstruct per-register address arithmetic and match the result
            against the target VAs, attributing each match to its enclosing
            DEFINE_REX_FUNC(sub_XXXX). addi/addic may target a different register
            than the lis, so src and dest regs are tracked independently.

Two image sources work (both flat, base 0x82000000):
  - the offline decompressed exe: H:/Emulators/games/XBOX/default.decompressed.exe
  - a runtime dump: NHL_DUMP_IMAGE=1 <game exe>  -> guest_image_dump.bin

Usage:
    python tools/franchise_re/find_franchise_code.py <image> \
        --generated generated/default --out docs/franchise_re_hits.txt
"""
import argparse
import glob
import re
from collections import defaultdict
from pathlib import Path

IMAGE_BASE = 0x82000000

KEYWORDS = [
    "gmmodedata", "leaguelogic", "injurygen", "injurybrain", "namegenerator",
    "statsbrain", "draft", "rookie", "prospect", "scout", "potential", "growth",
    "progress", "regress", "develop", "retire", "freeagent", "schedule",
    "standings", "simgame", "simulate", "playergen", "namepool", "season",
    "dynasty", "beapro", "franchise",
]
KW_RE = re.compile("|".join(re.escape(k) for k in KEYWORDS), re.IGNORECASE)
MIN_LEN = 6
PRINTABLE = re.compile(rb"[\x20-\x7e]{%d,}" % MIN_LEN)

# Reconstruct DECIMAL lis/addi/ori address loads from xenos-recomp output.
FUNC_RE = re.compile(r"DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]{8})\)")
SET_RE = re.compile(r"ctx\.r(\d+)\.[su]64 = (-?\d+);")
ADD_RE = re.compile(r"ctx\.r(\d+)\.s64 = ctx\.r(\d+)\.s64 \+ (-?\d+);")
OR_RE = re.compile(r"ctx\.r(\d+)\.u64 = ctx\.r(\d+)\.u64 \| (\d+);")


def scan_strings(data: bytes):
    """Yield (va, text) for printable ASCII runs that match a franchise keyword.

    A C-string literal starts after the preceding NUL, so trim leading chars that
    belong to a neighbouring string is left to the caller; we report the matched
    run start. For __FILE__ paths the match begins at the path, which is what the
    compiler passes, so this is exact in practice.
    """
    for m in PRINTABLE.finditer(data):
        text = m.group().decode("ascii", "replace")
        mk = KW_RE.search(text)
        if mk:
            # If the keyword sits mid-run (e.g. "...\\leaguelogic\\GmModeData.cpp"),
            # the C literal really starts at the path root; emit the run start.
            yield IMAGE_BASE + m.start(), text


def crossref(generated: Path, targets: dict, out_lines):
    """targets: {va:int -> label:str}. Find functions whose reconstructed lis/addi
    address arithmetic equals a target VA."""
    hits = defaultdict(set)
    for path in sorted(glob.glob(str(generated / "*.cpp"))):
        cur = None
        reg = {}
        for line in open(path, "r", encoding="utf-8", errors="ignore"):
            m = FUNC_RE.search(line)
            if m:
                cur, reg = m.group(1), {}
                continue
            m = ADD_RE.search(line)
            if m:
                d, s, imm = int(m.group(1)), int(m.group(2)), int(m.group(3))
                if s in reg:
                    reg[d] = (reg[s] + imm) & 0xFFFFFFFF
                    if reg[d] in targets:
                        hits[targets[reg[d]]].add((cur, Path(path).name))
                continue
            m = OR_RE.search(line)
            if m:
                d, s, imm = int(m.group(1)), int(m.group(2)), int(m.group(3))
                if s in reg:
                    reg[d] = (reg[s] | imm) & 0xFFFFFFFF
                    if reg[d] in targets:
                        hits[targets[reg[d]]].add((cur, Path(path).name))
                continue
            m = SET_RE.search(line)
            if m:
                v = int(m.group(2)) & 0xFFFFFFFF
                reg[int(m.group(1))] = v
                if v in targets:
                    hits[targets[v]].add((cur, Path(path).name))
    for label in sorted(set(targets.values())):
        fns = sorted(hits.get(label, []))
        out_lines.append(f"\n  {label}: {len(fns)} func(s)")
        for fn, fname in fns:
            out_lines.append(f"      <- {fn}  ({fname})")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="decompressed flat image (base 0x82000000)")
    ap.add_argument("--generated", default=None,
                    help="generated/default dir for stage-2 cross-ref")
    ap.add_argument("--out", default=None, help="report path (default stdout)")
    args = ap.parse_args()

    data = Path(args.image).read_bytes()
    hits = sorted(set(scan_strings(data)))

    lines = [
        f"# franchise/sim string hits in {args.image}",
        f"# base 0x{IMAGE_BASE:08X}, {len(data):,} bytes, {len(hits)} strings",
        "",
    ]
    for va, text in hits:
        lines.append(f"0x{va:08X}  {text}")

    if args.generated:
        # Only cross-ref .cpp path strings (the __FILE__ anchors), not every match.
        targets = {va: text.rsplit("\\", 1)[-1]
                   for va, text in hits if text.rstrip().endswith(".cpp")}
        lines.append("\n# === stage 2: functions referencing __FILE__ anchors ===")
        crossref(Path(args.generated), targets, lines)

    report = "\n".join(lines) + "\n"
    if args.out:
        Path(args.out).write_text(report, "utf-8")
        print(f"wrote {args.out}  ({len(hits)} string hits)")
    else:
        print(report)


if __name__ == "__main__":
    main()
