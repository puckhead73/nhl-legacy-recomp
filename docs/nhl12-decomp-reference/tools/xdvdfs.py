#!/usr/bin/env python3
"""XDVDFS (Xbox 360 GDFX) ISO inspector/extractor for nhl12-recomp.

Supports raw XGD2/XGD3 dumps and bare game-partition images. The game
partition base is auto-detected by probing for the volume-descriptor magic.

Usage:
  python xdvdfs.py list    <iso> [--csv out.csv]
  python xdvdfs.py extract <iso> <outdir> [--only GLOB ...]
"""

import argparse
import fnmatch
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Known game-partition base offsets: bare partition, XGD2, XGD3, XGD1 video.
PARTITION_BASES = (0x0, 0x02080000, 0xFD90000, 0x89D80000)
ATTR_DIRECTORY = 0x10


class Entry:
    __slots__ = ("path", "sector", "size", "attrs")

    def __init__(self, path, sector, size, attrs):
        self.path = path
        self.sector = sector
        self.size = size
        self.attrs = attrs

    @property
    def is_dir(self):
        return bool(self.attrs & ATTR_DIRECTORY)


def detect_base(f):
    for base in PARTITION_BASES:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("error: XDVDFS volume descriptor not found at any known offset")


def read_dir_table(f, base, sector, size, prefix, out):
    """Parse one directory table (a serialized binary tree) iteratively."""
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    visited = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in visited or off + 14 > len(data):
            continue
        visited.add(off)
        left, right, start, fsize, attrs, namelen = struct.unpack_from(
            "<HHIIBB", data, off
        )
        # 0xFFFF dwords of padding mark unused tail space in the table.
        if left == 0xFFFF:
            continue
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
        name = data[off + 14 : off + 14 + namelen].decode("ascii", "replace")
        path = prefix + name
        e = Entry(path, start, fsize, attrs)
        out.append(e)
        if e.is_dir:
            read_dir_table(f, base, start, fsize, path + "/", out)


def walk(f):
    base = detect_base(f)
    f.seek(base + 32 * SECTOR + 20)
    root_sector, root_size = struct.unpack("<II", f.read(8))
    entries = []
    read_dir_table(f, base, root_sector, root_size, "", entries)
    return base, entries


def cmd_list(args):
    with open(args.iso, "rb") as f:
        base, entries = walk(f)
    print(f"partition base: 0x{base:X}")
    files = [e for e in entries if not e.is_dir]
    dirs = [e for e in entries if e.is_dir]
    for e in sorted(entries, key=lambda e: e.path.lower()):
        kind = "<dir> " if e.is_dir else f"{e.size:>12,}"
        print(f"{kind}  {e.path}")
    print(f"\n{len(dirs)} dirs, {len(files)} files, "
          f"{sum(e.size for e in files):,} bytes total")
    if args.csv:
        with open(args.csv, "w", encoding="utf-8") as c:
            c.write("path,type,sector,size\n")
            for e in sorted(entries, key=lambda e: e.path.lower()):
                t = "dir" if e.is_dir else "file"
                c.write(f'"{e.path}",{t},{e.sector},{e.size}\n')
        print(f"wrote {args.csv}")


def cmd_extract(args):
    with open(args.iso, "rb") as f:
        base, entries = walk(f)
        todo = [e for e in entries if not e.is_dir]
        if args.only:
            todo = [
                e for e in todo
                if any(fnmatch.fnmatch(e.path.lower(), pat.lower())
                       for pat in args.only)
            ]
        total = sum(e.size for e in todo)
        done = 0
        for e in todo:
            dest = os.path.join(args.outdir, e.path.replace("/", os.sep))
            os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
            f.seek(base + e.sector * SECTOR)
            with open(dest, "wb") as o:
                remaining = e.size
                while remaining:
                    chunk = f.read(min(remaining, 1 << 22))
                    if not chunk:
                        raise IOError(f"short read extracting {e.path}")
                    o.write(chunk)
                    remaining -= len(chunk)
            done += e.size
            print(f"[{done * 100 // max(total, 1):3d}%] {e.path} ({e.size:,})")
        print(f"extracted {len(todo)} files, {done:,} bytes")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("list")
    pl.add_argument("iso")
    pl.add_argument("--csv")
    pl.set_defaults(fn=cmd_list)
    pe = sub.add_parser("extract")
    pe.add_argument("iso")
    pe.add_argument("outdir")
    pe.add_argument("--only", nargs="*", help="glob patterns, e.g. *.xex")
    pe.set_defaults(fn=cmd_extract)
    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
