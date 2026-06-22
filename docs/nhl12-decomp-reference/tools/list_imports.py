#!/usr/bin/env python3
"""Resolve every kernel/XAM import of nhlzf.exe to ordinal + name.

Reads the import-library record arrays from the plaintext XEX header, then the
record values from the decrypted image (type = value>>24: 0 = data slot,
1 = code thunk; ordinal = value & 0xFFFF). Names from Xenia's export tables.

Usage: python list_imports.py extracted/default.xex extracted/nhlzf_image.bin \
           docs/kernel_imports.csv
"""

import re
import struct
import sys

IMAGE_BASE = 0x82000000


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def load_names(path, libname):
    names = {}
    rx = re.compile(r"XE_EXPORT\(\s*\w+\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(\w+)\s*,")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = rx.search(line)
        if m:
            names[int(m.group(1), 0)] = m.group(2)
    return names


def main():
    xex_path, img_path, out_path = sys.argv[1:4]
    xex = open(xex_path, "rb").read()
    img = open(img_path, "rb").read()

    opt_count = be32(xex, 0x14)
    opts = {be32(xex, 0x18 + i * 8): be32(xex, 0x18 + i * 8 + 4)
            for i in range(opt_count)}
    o = opts[0x000103FF]  # IMPORT_LIBRARIES
    str_size, str_count = struct.unpack_from(">II", xex, o + 4)
    strings = [s.decode() for s in xex[o + 12 : o + 12 + str_size].split(b"\0") if s]

    tables = {
        "xboxkrnl.exe": load_names("tools/data/xboxkrnl_table.inc", "xboxkrnl"),
        "xam.xex": load_names("tools/data/xam_table.inc", "xam"),
    }

    lib_off = o + 12 + str_size
    rows = []
    for _ in range(str_count):
        size = be32(xex, lib_off)
        name_idx, count = struct.unpack_from(">HH", xex, lib_off + 0x24)
        lib = strings[name_idx] if name_idx < len(strings) else f"#{name_idx}"
        recs = [be32(xex, lib_off + 0x28 + i * 4) for i in range(count)]
        for va in recs:
            # NOTE: nhlzf_image.bin comes from XenonUtils ParseImage, which
            # byte-swaps import records in place (xex.cpp:331) and replaces
            # function thunks with synthetic code (xex.cpp:342). Therefore:
            #  - slot records (.rdata): host-LE read == original BE value
            #  - thunk records (.text): contents synthetic; classify by VA
            in_text = 0x82580000 <= va < 0x836D8BD0
            if in_text:
                rows.append((lib, None, None, 1, va))
            else:
                value = struct.unpack_from("<I", img, va - IMAGE_BASE)[0]
                ordinal = value & 0xFFFF
                name = tables.get(lib, {}).get(ordinal, f"ord_{ordinal}")
                rows.append((lib, ordinal, name, 0, va))
        lib_off += size

    # records come in order: slot, then (for functions) its thunk
    uniq = {}
    last_key = None
    for lib, ordinal, name, rtype, va in rows:
        if rtype == 0:
            key = (lib, ordinal)
            e = uniq.setdefault(key, {"name": name, "slot": va, "thunk": None})
            last_key = key
        else:
            if last_key is not None:
                uniq[last_key]["thunk"] = va

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("library,ordinal,name,kind,slot_va,thunk_va\n")
        for (lib, ordinal), e in sorted(uniq.items()):
            kind = "function" if e["thunk"] else "variable"
            f.write(f"{lib},{ordinal},{e['name']},{kind},"
                    f"{e['slot'] and f'0x{e['slot']:08X}' or ''},"
                    f"{e['thunk'] and f'0x{e['thunk']:08X}' or ''}\n")

    n_fn = sum(1 for e in uniq.values() if e["thunk"])
    n_var = len(uniq) - n_fn
    n_unk = sum(1 for e in uniq.values() if e["name"].startswith("ord_"))
    print(f"records: {len(rows)}  unique imports: {len(uniq)} "
          f"({n_fn} functions, {n_var} variables, {n_unk} unresolved names)")
    by_lib = {}
    for (lib, _), e in uniq.items():
        by_lib.setdefault(lib, []).append(e)
    for lib, es in by_lib.items():
        print(f"  {lib}: {len(es)}")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
