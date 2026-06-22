#!/usr/bin/env python3
"""Parse EA RenderWare-4 "GameData" .cba animation bundles (NHL Legacy / NHL 12).

The .cba is a chunked, self-describing, little-endian stream:

    [GD.STRM]  container chunk   (spans the whole file)
    [GD.REFL]  reflection schema (type definitions: names + fields + type hash)
    [GD.DATA]* N records back-to-back, each typed by a 32-bit type hash

Chunk header (all three tags): 8-byte tag "GD.xxxxl" + u32 total_size (incl. the
16-byte header) + u32 payload_size. The next chunk is at offset + total_size.

GD.DATA payload layout (observed):
    +0  u32  0x030e6205   format magic (constant)
    +4  12 bytes          zero
    +16 u32  type_hash    -> indexes a GD.REFL type definition
    +20 ...               type-specific fields (may embed __name / __guid strings)

The GD.REFL block opens with a u64 offset table (one entry per structural type
definition) and holds the schema vocabulary (struct/field names like
"TagCollectionSetAsset", "AccelBlendAsset"). Records, however, are authored
"virtual asset" instances whose human names (e.g. "ToolTagCollectionSetVirtualAsset",
"SEQ_DK_TAP_...") live inline in the DATA payload, NOT in REFL. So a record-type's
label is recovered empirically: the dominant authored name across all records that
share a type hash (reported with a confidence %), which is more accurate than any
REFL lookup and needs no cracking of EA's name-hash function.

Usage:
    python cba_parse.py anim.cba                         # summary report
    python cba_parse.py anim.cba --schema schema.json    # dump 469-type schema
    python cba_parse.py anim.cba --records records.csv   # per-record manifest
    python cba_parse.py anim.cba --names                 # list named entities
"""

from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

STRM_TAG = b"GD.STRMl"
REFL_TAG = b"GD.REFLl"
DATA_TAG = b"GD.DATAl"
DATA_MAGIC = 0x030E6205
TYPE_HASH_OFFSET = 16  # within a GD.DATA payload

_ASCII_RUN = re.compile(rb"[\x20-\x7e]{3,}")
_NAME_RUN = re.compile(rb"[\x20-\x7e]{4,}")


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


@dataclass(frozen=True)
class Chunk:
    tag: bytes
    offset: int
    total_size: int
    payload_size: int

    @property
    def payload_offset(self) -> int:
        return self.offset + 16

    @property
    def end(self) -> int:
        return self.offset + self.total_size


@dataclass
class Record:
    index: int
    offset: int
    total_size: int
    payload_size: int
    type_hash: int
    names: list[str] = field(default_factory=list)


@dataclass
class TypeLabel:
    hash: int
    label: str          # REFL type name if recovered, else dominant authored name
    confidence: float   # fraction of this type's records bearing the empirical name
    count: int
    refl_name: str = ""        # structural type name from REFL (e.g. DctAnimationAsset)
    fields: list[str] = field(default_factory=list)  # REFL field order


@dataclass
class Cba:
    path: Path
    size: int
    strm: Chunk
    refl: Chunk
    records: list[Record]
    type_labels: dict[int, TypeLabel]   # hash -> empirical label
    refl_strings: list[str]             # REFL schema vocabulary


def read_chunk(data: bytes, off: int) -> Chunk:
    tag = data[off : off + 8]
    total, payload = struct.unpack_from("<II", data, off + 8)
    return Chunk(tag, off, total, payload)


def def_strings(data: bytes, beg: int, end: int) -> list[str]:
    return [m.group().decode("latin1") for m in _ASCII_RUN.finditer(data[beg:end])]


_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*$")


def refl_typedefs(data: bytes, refl: Chunk, hashes: set[int]) -> dict[int, tuple[str, list[str]]]:
    """Recover (type_name, [field_names]) per type hash from the REFL schema.

    REFL opens with an ascending u64 table of absolute offsets, one per structural
    type definition. Each def lists its strings as: [base-type tail...] type_name
    "__guid" "__name" "__base" field0 field1 ...  — so the type's own name is the
    string just before "__guid" and its fields follow "__base". A type's hash is
    stored in its own def header; we find the def region containing that hash word.
    """
    table: list[int] = []
    off = refl.payload_offset
    while off + 8 <= refl.end:
        v = struct.unpack_from("<Q", data, off)[0]
        if v < refl.payload_offset or v >= refl.end or (table and v < table[-1]):
            break
        table.append(v)
        off += 8
    if not table:
        return {}

    out: dict[int, tuple[str, list[str]]] = {}
    for h in hashes:
        needle = struct.pack("<I", h)
        # A hash can appear both in its own def header and as a field type-ref in
        # other defs. Scan every occurrence; accept the first that yields a clean
        # "<ident> __guid ... __base <fields>" shape.
        start = refl.payload_offset
        while True:
            loc = data.find(needle, start, refl.end)
            if loc < 0:
                break
            start = loc + 4
            i = bisect.bisect_right(table, loc) - 1
            if i < 0:
                continue
            beg, end = table[i], (table[i + 1] if i + 1 < len(table) else refl.end)
            s = def_strings(data, beg, end)
            if "__guid" not in s or "__base" not in s:
                continue
            name = s[s.index("__guid") - 1]
            if not _IDENT.match(name):
                continue
            fields = [x for x in s[s.index("__base") + 1:] if not x.startswith("__")]
            out[h] = (name, fields)
            break
    return out


# An authored name: starts with a letter, mostly identifier-ish characters.
_AUTHORED = re.compile(rb"[A-Za-z][A-Za-z0-9_ .\[\]()/-]{3,63}")


def label_types(records: list["Record"]) -> dict[int, TypeLabel]:
    """Label each type hash by the dominant authored name across its records.

    Record type-names are not in REFL; they are authored strings inline in the
    payload. The first authored-looking name in a record is, in the common case,
    that record's type/collection name, so the per-hash mode is a reliable label.
    """
    by_hash: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    total: collections.Counter = collections.Counter()
    for r in records:
        total[r.type_hash] += 1
        name = next((n for n in r.names if _AUTHORED.fullmatch(n.encode("latin1"))), None)
        if name:
            by_hash[r.type_hash][name] += 1

    labels: dict[int, TypeLabel] = {}
    for h, n in total.items():
        votes = by_hash.get(h)
        if votes:
            label, lc = votes.most_common(1)[0]
            labels[h] = TypeLabel(h, label, lc / n, n)
        else:
            labels[h] = TypeLabel(h, f"type_{h:08x}", 0.0, n)
    return labels


def parse(data: bytes, path: Path, collect_names: bool = True) -> Cba:
    if data[:8] != STRM_TAG:
        raise ValueError(f"{path}: not a GD.STRM .cba (magic={data[:8]!r})")
    strm = read_chunk(data, 0)
    refl = read_chunk(data, 16)
    if refl.tag != REFL_TAG:
        raise ValueError(f"{path}: expected GD.REFL at 0x10, got {refl.tag!r}")

    # Records begin right after the REFL block.
    data_start = refl.end if refl.end > refl.payload_offset else refl.payload_offset
    # REFL.total_size is relative to its own header; recompute the true data start
    # as the first GD.DATA tag at/after the REFL payload.
    first_data = data.find(DATA_TAG, refl.payload_offset)
    if first_data < 0:
        raise ValueError(f"{path}: no GD.DATA records found")
    data_start = first_data

    records: list[Record] = []
    off = data_start
    idx = 0
    while off + 16 <= len(data):
        if data[off : off + 8] != DATA_TAG:
            break
        total, payload = struct.unpack_from("<II", data, off + 8)
        if total < 16 or off + total > len(data):
            break
        pay = data[off + 16 : off + total]
        th = u32(pay, TYPE_HASH_OFFSET) if len(pay) >= TYPE_HASH_OFFSET + 4 else 0
        names: list[str] = []
        if collect_names:
            names = [m.group().decode("latin1")
                     for m in _NAME_RUN.finditer(pay[TYPE_HASH_OFFSET + 4 :])]
        records.append(Record(idx, off, total, payload, th, names))
        off += total
        idx += 1

    type_labels = label_types(records)
    typedefs = refl_typedefs(data, refl, set(type_labels))
    for h, lbl in type_labels.items():
        name, fields = typedefs.get(h, ("", []))
        lbl.refl_name = name
        lbl.fields = fields
        if name:                       # prefer the structural REFL name as the label
            lbl.label = name
    refl_strings = sorted(set(def_strings(data, refl.payload_offset, refl.end)))
    return Cba(path, len(data), strm, refl, records, type_labels, refl_strings)


# --- name heuristics for the report -----------------------------------------

# strings that look like authored asset/clip/sequence names vs. schema tokens
_CLIP_HINT = re.compile(
    r"(SEQ_|tsblend|psk\d|_psk|deke|Deke|skat|Skat|shoot|Shoot|pass|Pass|"
    r"check|Check|celebrat|Celebrat|fight|Fight|goalie|Goalie|save|Save|"
    r"hit|Hit|dangle|loop|Loop|idle|Idle|stick|Stick)", re.IGNORECASE)


def type_name(cba: Cba, h: int) -> str:
    lbl = cba.type_labels.get(h)
    return lbl.label if lbl else f"type_{h:08x}"


def primary_name(r: "Record") -> str:
    """A record's own authored name = its longest clean inline token (the trailing
    name field), as opposed to short field/tag tokens it may also embed."""
    clean = [n for n in r.names if _AUTHORED.fullmatch(n.encode("latin1"))]
    return max(clean, key=len) if clean else (r.names[0] if r.names else "")


def report(cba: Cba) -> None:
    print(f"{cba.path.name}: {cba.size:,} bytes")
    print(f"  GD.STRM  total=0x{cba.strm.total_size:x}")
    print(f"  GD.REFL  payload=0x{cba.refl.payload_size:x}  "
          f"({len(cba.refl_strings)} schema strings)")
    print(f"  GD.DATA  records={len(cba.records):,}  "
          f"({len(cba.type_labels)} distinct types)")

    print("\n  Top 25 record types (label @ confidence):")
    for lbl in sorted(cba.type_labels.values(), key=lambda t: -t.count)[:25]:
        print(f"    {lbl.hash:#010x}  x{lbl.count:<6} {lbl.confidence*100:3.0f}%  {lbl.label}")

    # named-entity census: distinct authored names that look clip-like
    named = collections.Counter()
    for r in cba.records:
        for nm in r.names:
            if _CLIP_HINT.search(nm) and 3 < len(nm) < 80:
                named[nm] += 1
    print(f"\n  {len(named):,} distinct clip/sequence-like names. Sample 30:")
    for nm, n in named.most_common(30):
        print(f"    x{n:<4} {nm}")


def write_schema(cba: Cba, out: Path) -> None:
    payload = {
        "file": cba.path.name,
        "record_types": [
            {"hash": f"{lbl.hash:#010x}", "label": lbl.label,
             "count": lbl.count, "confidence": round(lbl.confidence, 3)}
            for lbl in sorted(cba.type_labels.values(), key=lambda t: -t.count)
        ],
        "refl_schema_strings": cba.refl_strings,
    }
    out.write_text(json.dumps(payload, indent=2))
    print(f"wrote schema: {out} ({len(cba.type_labels)} record types, "
          f"{len(cba.refl_strings)} schema strings)")


def write_records(cba: Cba, out: Path) -> None:
    with out.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["index", "offset", "total_size", "payload_size",
                    "type_hash", "type_name", "first_names"])
        for r in cba.records:
            w.writerow([r.index, f"0x{r.offset:x}", r.total_size, r.payload_size,
                        f"0x{r.type_hash:08x}", type_name(cba, r.type_hash),
                        " | ".join(r.names[:3])])
    print(f"wrote records: {out} ({len(cba.records):,} rows)")


@dataclass
class Field:
    desc_at: int        # offset of the descriptor within the payload
    count: int          # element count
    capacity: int       # element capacity (== count in practice)
    data_at: int        # offset of the array data within the payload
    span: int           # bytes from data_at to the next field's data
    stride: float       # span / count
    kind: str           # inferred element kind
    preview: str        # short rendered preview


# A GD.DATA payload is a reflected struct: after the 16-byte common preamble and
# the u32 type hash + u32 pad, it carries a table of 16-byte field descriptors
# (count, capacity, data_offset, 0) followed by the field data they point at.
FIELD_TABLE_START = 0x20


def decode_fields(pay: bytes) -> list[Field]:
    descs: list[tuple[int, int, int, int]] = []
    i = FIELD_TABLE_START
    n = len(pay)
    while i + 16 <= n:
        count, cap, off, z = struct.unpack_from("<IIII", pay, i)
        if z != 0 or off == 0 or off >= n or off < i:
            break
        descs.append((i, count, cap, off))
        i += 16

    data_offsets = sorted({d[3] for d in descs} | {n})

    def end_of(off: int) -> int:
        return min(x for x in data_offsets if x > off)

    fields: list[Field] = []
    for desc_at, count, cap, off in descs:
        end = end_of(off)
        span = end - off
        stride = span / count if count else 0.0
        seg = pay[off:end]
        kind, preview = _interpret(seg, count, stride)
        fields.append(Field(desc_at, count, cap, off, span, stride, kind, preview))
    return fields


def _interpret(seg: bytes, count: int, stride: float) -> tuple[str, str]:
    def near(x: float) -> bool:
        return abs(stride - x) < 0.02
    # String fields: the first `count` bytes are a (null-terminated) printable run.
    # Detect by content, not stride — the last field's span includes trailing pad.
    head = seg[:count] if 0 < count <= len(seg) else seg
    run = head.split(b"\0", 1)[0]
    if run and len(run) >= max(3, count - 2) and all(0x20 <= b < 0x7F for b in run):
        return "string", run.decode("latin1")[:60]
    if near(1):
        printable = sum(1 for b in seg[:64] if 0x20 <= b < 0x7F)
        if printable >= min(len(seg), 64) * 0.8:
            return "string", seg.split(b"\0", 1)[0].decode("latin1")[:60]
        return "bytes", seg[:24].hex(" ")
    if len(seg) >= 4 and struct.unpack_from("<I", seg, 0)[0] == DATA_MAGIC:
        return "nested", f"{count} embedded sub-record(s)"
    if near(2):
        vals = struct.unpack_from("<%dh" % min(count, 12), seg)
        # Tracks that ride the full int16 range look like quantized rotations
        # (unit quaternion components scaled to +/-32767).
        peak = max((abs(v) for v in struct.unpack_from("<%dh" % count, seg)), default=0)
        kind = "quat16?" if peak > 26000 else "int16"
        return kind, ",".join(map(str, vals)) + ("..." if count > 12 else "")
    if near(4):
        vals = struct.unpack_from("<%df" % min(count, 8), seg)
        return "f32", ",".join(f"{v:.4g}" for v in vals) + ("..." if count > 8 else "")
    if near(12):
        v = struct.unpack_from("<3f", seg)
        return "vec3f", f"({v[0]:.4g},{v[1]:.4g},{v[2]:.4g})..."
    if near(16):
        v = struct.unpack_from("<4f", seg)
        return "vec4f", f"({v[0]:.4g},{v[1]:.4g},{v[2]:.4g},{v[3]:.4g})..."
    return f"stride{stride:.3g}", seg[:24].hex(" ")


def decode_record(cba: Cba, data: bytes, index: int) -> None:
    if not (0 <= index < len(cba.records)):
        raise SystemExit(f"record index out of range (0..{len(cba.records)-1})")
    r = cba.records[index]
    pay = data[r.offset + 16 : r.offset + r.total_size]
    fields = decode_fields(pay)
    name = primary_name(r) or next((f.preview for f in fields if f.kind == "string"), "?")
    lbl = cba.type_labels.get(r.type_hash)
    schema = lbl.fields if lbl else []
    print(f"record #{index} @0x{r.offset:x}  type={type_name(cba, r.type_hash)} "
          f"({r.type_hash:#010x})  payload={r.payload_size}B  name={name!r}")
    if schema:
        print(f"  REFL schema ({lbl.refl_name}): {', '.join(schema)}")
    print(f"  {len(fields)} decoded arrays (count/element/preview):")
    for f in fields:
        print(f"    f@{f.desc_at:#04x} cnt={f.count:<6} off={f.data_at:#06x} "
              f"span={f.span:<6} {f.kind:<8} {f.preview}")


def write_names(cba: Cba) -> None:
    names = collections.Counter()
    for r in cba.records:
        for nm in r.names:
            if 3 < len(nm) < 80 and not nm.startswith("__"):
                names[nm] += 1
    for nm, n in sorted(names.items()):
        print(f"x{n:<5} {nm}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cba", type=Path, help="path to a .cba bundle")
    ap.add_argument("--schema", type=Path, help="dump REFL type schema to JSON")
    ap.add_argument("--records", type=Path, help="dump per-record manifest to CSV")
    ap.add_argument("--names", action="store_true",
                    help="list all authored names (sorted)")
    ap.add_argument("--decode", type=int, metavar="INDEX",
                    help="structurally decode one record's reflected fields")
    ap.add_argument("--find", metavar="NAME",
                    help="decode the first record whose name contains NAME")
    args = ap.parse_args(argv)

    data = args.cba.read_bytes()
    cba = parse(data, args.cba)

    if args.names:
        write_names(cba)
        return 0
    if args.find:
        q = args.find.lower()
        matches = [r for r in cba.records if q in primary_name(r).lower()]
        if not matches:
            raise SystemExit(f"no record name contains {args.find!r}")
        # Prefer the heaviest match (the clip's own curve record over light references).
        decode_record(cba, data, max(matches, key=lambda r: r.payload_size).index)
        return 0
    if args.decode is not None:
        decode_record(cba, data, args.decode)
        return 0
    report(cba)
    if args.schema:
        write_schema(cba, args.schema)
    if args.records:
        write_records(cba, args.records)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
