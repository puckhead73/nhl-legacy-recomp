#!/usr/bin/env python3
"""Unpack EA Canada BigEB v3 archives used by NHL 12.

The container header is big-endian. File offsets are stored as 16-byte block
numbers, and file/directory names live in fixed-width tables.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Iterable


BIGEB_MAGIC = b"EB\x00\x03"
COPY_CHUNK_SIZE = 8 * 1024 * 1024


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def u16be(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def u32be(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def decode_cstr(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("latin1")


@dataclass(frozen=True)
class BigEntry:
    archive: Path
    index: int
    path: str
    offset: int
    packed_size: int
    unpacked_size: int
    checksum: int

    @property
    def stored_size(self) -> int:
        return self.packed_size or self.unpacked_size

    @property
    def is_packed(self) -> bool:
        return self.packed_size != 0


@dataclass(frozen=True)
class BigArchive:
    path: Path
    total_size: int
    entries: list[BigEntry]


def parse_bigeb(path: Path) -> BigArchive:
    with path.open("rb") as f:
        header = f.read(0x30)
        if len(header) < 0x30 or header[:4] != BIGEB_MAGIC:
            raise ValueError(f"{path} is not a BigEB v3 archive")

        count = u32be(header, 0x04)
        name_table_offset = u32be(header, 0x0C)
        name_stride = header[0x14]
        dir_stride = header[0x15]
        dir_count = u16be(header, 0x16)
        total_size = u32be(header, 0x1C)

        if name_stride < 3:
            raise ValueError(f"{path}: invalid name stride {name_stride}")
        if dir_count and dir_stride == 0:
            raise ValueError(f"{path}: invalid directory stride 0")

        f.seek(0x30)
        raw_entries = f.read(count * 16)
        if len(raw_entries) != count * 16:
            raise ValueError(f"{path}: truncated entry table")

        f.seek(name_table_offset)
        raw_names = f.read(count * name_stride)
        if len(raw_names) != count * name_stride:
            raise ValueError(f"{path}: truncated name table")

        dir_table_offset = align(name_table_offset + count * name_stride, 16)
        f.seek(dir_table_offset)
        raw_dirs = f.read(dir_count * dir_stride)
        if len(raw_dirs) != dir_count * dir_stride:
            raise ValueError(f"{path}: truncated directory table")

    dirs = [
        decode_cstr(raw_dirs[i * dir_stride : (i + 1) * dir_stride])
        for i in range(dir_count)
    ]

    entries: list[BigEntry] = []
    for index in range(count):
        entry_offset = index * 16
        offset_blocks = u32be(raw_entries, entry_offset)
        packed_size = u32be(raw_entries, entry_offset + 4)
        unpacked_size = u32be(raw_entries, entry_offset + 8)
        checksum = u32be(raw_entries, entry_offset + 12)

        name_offset = index * name_stride
        name_record = raw_names[name_offset : name_offset + name_stride]
        dir_index = u16be(name_record, 0)
        name = decode_cstr(name_record[2:])
        if not name:
            raise ValueError(f"{path}: empty filename at entry {index}")

        directory = dirs[dir_index] if dir_index < len(dirs) else ""
        member_path = f"{directory}/{name}" if directory and directory != "." else name

        offset = offset_blocks * 16
        stored_size = packed_size or unpacked_size
        if offset + stored_size > total_size:
            raise ValueError(
                f"{path}: entry {member_path} exceeds archive size "
                f"(offset=0x{offset:X}, size=0x{stored_size:X}, total=0x{total_size:X})"
            )

        entries.append(
            BigEntry(
                archive=path,
                index=index,
                path=member_path,
                offset=offset,
                packed_size=packed_size,
                unpacked_size=unpacked_size,
                checksum=checksum,
            )
        )

    return BigArchive(path=path, total_size=total_size, entries=entries)


def safe_output_path(root: Path, member_path: str) -> Path:
    normalized = member_path.replace("\\", "/")
    posix = PurePosixPath(normalized)
    if posix.is_absolute() or any(part in ("..", "") for part in posix.parts):
        raise ValueError(f"unsafe archive path: {member_path!r}")
    if any(":" in part for part in posix.parts):
        raise ValueError(f"unsafe archive path: {member_path!r}")
    return root.joinpath(*posix.parts)


def read_exact(f: BinaryIO, size: int) -> bytes:
    data = f.read(size)
    if len(data) != size:
        raise EOFError(f"expected {size} bytes, got {len(data)}")
    return data


def copy_exact(src: BinaryIO, dst: BinaryIO, size: int) -> None:
    remaining = size
    while remaining:
        chunk = src.read(min(COPY_CHUNK_SIZE, remaining))
        if not chunk:
            raise EOFError(f"unexpected EOF with {remaining} bytes remaining")
        dst.write(chunk)
        remaining -= len(chunk)


def decompress_chunkzip(blob: bytes, expected_size: int) -> bytes:
    if len(blob) < 0x30 or blob[:8] != b"chunkzip":
        raise ValueError("not a chunkzip member")

    version = u32be(blob, 0x08)
    total_size = u32be(blob, 0x0C)
    chunk_unpacked_size = u32be(blob, 0x10)
    chunk_count = u32be(blob, 0x14)
    descriptor_size = u32be(blob, 0x18)

    if version != 2:
        raise ValueError(f"unsupported chunkzip version {version}")
    if expected_size and total_size != expected_size:
        raise ValueError(
            f"chunkzip size mismatch: header=0x{total_size:X}, entry=0x{expected_size:X}"
        )
    if descriptor_size < 0x10:
        raise ValueError(f"invalid chunkzip descriptor size {descriptor_size}")

    pos = 0x20
    output = bytearray()
    for chunk_index in range(chunk_count):
        pos = align(pos, 16)
        if pos + descriptor_size > len(blob):
            raise ValueError(f"truncated chunkzip descriptor {chunk_index}")

        descriptor = blob[pos : pos + descriptor_size]
        compressed_size = u32be(descriptor, 0x08)
        flags = u32be(descriptor, 0x0C)
        pos += descriptor_size

        if pos + compressed_size > len(blob):
            raise ValueError(f"truncated chunkzip payload {chunk_index}")
        payload = blob[pos : pos + compressed_size]
        pos += compressed_size

        remaining = total_size - len(output)
        target_size = min(chunk_unpacked_size, remaining)
        if flags == 0:
            chunk = payload
        elif flags == 1:
            chunk = zlib.decompress(payload, wbits=-15)
        else:
            raise ValueError(f"unsupported chunkzip flags 0x{flags:X}")

        if len(chunk) != target_size:
            raise ValueError(
                f"chunkzip chunk {chunk_index} size mismatch: "
                f"got 0x{len(chunk):X}, expected 0x{target_size:X}"
            )
        output.extend(chunk)

    if len(output) != total_size:
        raise ValueError(
            f"chunkzip output size mismatch: got 0x{len(output):X}, "
            f"expected 0x{total_size:X}"
        )

    return bytes(output)


def decompress_packed_member(blob: bytes, expected_size: int) -> bytes:
    if blob.startswith(b"chunkzip"):
        return decompress_chunkzip(blob, expected_size)
    raise ValueError(f"unsupported packed member header: {blob[:16].hex(' ')}")


def extract_entry(entry: BigEntry, archive_file: BinaryIO, output_root: Path) -> None:
    output_path = safe_output_path(output_root, entry.path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    archive_file.seek(entry.offset)
    if entry.is_packed:
        packed = read_exact(archive_file, entry.packed_size)
        data = decompress_packed_member(packed, entry.unpacked_size)
        output_path.write_bytes(data)
    else:
        with output_path.open("wb") as out_file:
            copy_exact(archive_file, out_file, entry.unpacked_size)


def hardlink_or_copy(source: Path, dest: Path) -> str:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        return "exists"
    try:
        os.link(source, dest)
        return "linked"
    except OSError:
        shutil.copy2(source, dest)
        return "copied"


def matching_entries(entries: Iterable[BigEntry], pattern: re.Pattern[str] | None) -> list[BigEntry]:
    if pattern is None:
        return list(entries)
    return [entry for entry in entries if pattern.search(entry.path)]


def list_archive(archive: BigArchive, pattern: re.Pattern[str] | None) -> None:
    for entry in matching_entries(archive.entries, pattern):
        packed = f" packed=0x{entry.packed_size:X}" if entry.packed_size else ""
        print(
            f"{archive.path.name}:{entry.index:05d} "
            f"off=0x{entry.offset:X} size=0x{entry.unpacked_size:X}{packed} "
            f"{entry.path}"
        )


def extract_archive(
    archive: BigArchive,
    output_root: Path,
    pattern: re.Pattern[str] | None,
    mirror_archive: bool,
) -> tuple[int, int]:
    entries = matching_entries(archive.entries, pattern)
    with archive.path.open("rb") as archive_file:
        for entry in entries:
            extract_entry(entry, archive_file, output_root)

    if mirror_archive:
        state = hardlink_or_copy(archive.path, output_root / archive.path.name)
        print(f"{state}: {output_root / archive.path.name}")

    return len(entries), sum(entry.unpacked_size for entry in entries)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", nargs="+", type=Path, help="BigEB .big archive(s)")
    parser.add_argument("-o", "--output", type=Path, help="output directory")
    parser.add_argument("--list", action="store_true", help="list entries instead of extracting")
    parser.add_argument(
        "--pattern",
        help="only include entries whose normalized archive path matches this regex",
    )
    parser.add_argument(
        "--mirror-archives",
        action="store_true",
        help="hard-link or copy each input .big into the output root",
    )
    return parser


def main(argv: list[str]) -> int:
    args = build_arg_parser().parse_args(argv)
    pattern = re.compile(args.pattern, re.IGNORECASE) if args.pattern else None

    archives = [parse_bigeb(path) for path in args.archives]
    if args.list:
        for archive in archives:
            list_archive(archive, pattern)
        return 0

    if args.output is None:
        raise SystemExit("--output is required when extracting")

    output_root = args.output
    output_root.mkdir(parents=True, exist_ok=True)

    total_entries = 0
    total_bytes = 0
    for archive in archives:
        count, size = extract_archive(
            archive,
            output_root,
            pattern=pattern,
            mirror_archive=args.mirror_archives,
        )
        total_entries += count
        total_bytes += size
        print(f"extracted {count} entries from {archive.path} ({size:,} bytes)")

    print(f"done: {total_entries} entries, {total_bytes:,} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
