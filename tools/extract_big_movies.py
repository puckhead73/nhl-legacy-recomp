"""Extract uncompressed .vp6/.sub movie files from EA "EB" v3 .big archives.

Usage: python tools/extract_big_movies.py <game_dir> <out_dir> [--dry-run]
Layout (big-endian), inferred from NHL Legacy's cachemovie.big:
  0x00 'EB' u16 version(3)   0x04 u32 count   0x08 u32 flags/align
  0x0C u32 names_off  0x10 u32 names_size
  0x14 u8 name_rec_len  0x15 u8 folder_rec_len  0x16 u16 folder_count
  0x1C u32 archive_size
  0x30 entries[count] x16: u32 offset>>4, u32 packed(0 = stored), u32 size, u32 hash
  names_off: count x name_rec_len records: u16 folder_idx + NUL-padded name
  align16(names_off + count*name_rec_len): folder_count x folder_rec_len paths
"""
import os
import struct
import sys

WANT = (".vp6", ".sub")


def cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def process(path, out_dir, dry):
    with open(path, "rb") as f:
        hdr = f.read(0x30)
        if hdr[:2] != b"EB" or struct.unpack(">H", hdr[2:4])[0] != 3:
            return 0
        count, = struct.unpack(">I", hdr[4:8])
        names_off, names_size = struct.unpack(">II", hdr[0x0C:0x14])
        name_len, folder_len = hdr[0x14], hdr[0x15]
        folder_count, = struct.unpack(">H", hdr[0x16:0x18])
        total, = struct.unpack(">I", hdr[0x1C:0x20])
        if total != os.path.getsize(path):
            print(f"  skip {path}: size field mismatch")
            return 0
        f.seek(0x30)
        entries = [struct.unpack(">IIII", f.read(16)) for _ in range(count)]
        f.seek(names_off)
        names = f.read(names_size)
        folders_off = (count * name_len + 15) & ~15
        folders = [cstr(names[folders_off + i * folder_len:folders_off + (i + 1) * folder_len])
                   for i in range(folder_count)]
        n = 0
        for i, (off, packed, size, _h) in enumerate(entries):
            rec = names[i * name_len:(i + 1) * name_len]
            fidx, = struct.unpack(">H", rec[:2])
            name = cstr(rec[2:])
            if not name.lower().endswith(WANT):
                continue
            if packed != 0 or fidx >= folder_count:
                print(f"  skip {name}: packed={packed:#x} folder={fidx}")
                continue
            rel = os.path.join(*folders[fidx].split("/"), name)
            f.seek(off << 4)
            data = f.read(size)
            if name.lower().endswith(".vp6") and data[:4] != b"MVhd":
                print(f"  skip {rel}: no MVhd header")
                continue
            n += 1
            print(f"  {os.path.basename(path)} -> {rel} ({size} bytes)")
            if not dry:
                dst = os.path.join(out_dir, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as o:
                    o.write(data)
        return n


def main():
    game, out = sys.argv[1], sys.argv[2]
    dry = "--dry-run" in sys.argv
    total = 0
    for fn in sorted(os.listdir(game)):
        if fn.lower().endswith(".big"):
            total += process(os.path.join(game, fn), out, dry)
    print(f"{'would extract' if dry else 'extracted'} {total} files")


if __name__ == "__main__":
    main()
