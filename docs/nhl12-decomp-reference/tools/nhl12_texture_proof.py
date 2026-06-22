#!/usr/bin/env python3
"""Offline proof harness for NHL12 RX2 equipment textures.

This reads real NHL12 RX2/RW4 arena texture files, decodes the embedded Xenos
texture fetch constant, mirrors the RexGlue texture-layout decisions that matter
for 2D equipment maps, and optionally writes linear DDS/PNG previews.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import math
import re
import struct
import sys
import zlib
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


OBJECT_TYPES = {
    0x00010031: "BASERESOURCE_1",
    0x00010032: "BASERESOURCE_2",
    0x00010033: "BASERESOURCE_3",
    0x00020003: "RASTER",
    0x00EC0010: "ARENADICTIONARY",
}

DIMENSIONS = {
    0: "1D",
    1: "2D_OR_STACKED",
    2: "3D",
    3: "CUBE",
}

ENDIANS = {
    0: "none",
    1: "8in16",
    2: "8in32",
    3: "16in32",
}

FORMAT_NAMES = {
    6: "k_8_8_8_8",
    18: "k_DXT1",
    19: "k_DXT2_3",
    20: "k_DXT4_5",
    49: "k_DXN",
    50: "k_8_8_8_8_AS_16_16_16_16",
    51: "k_DXT1_AS_16_16_16_16",
    52: "k_DXT2_3_AS_16_16_16_16",
    53: "k_DXT4_5_AS_16_16_16_16",
    58: "k_DXT3A",
    59: "k_DXT5A",
    60: "k_CTX1",
}

BASE_FORMAT = {
    50: 6,
    51: 18,
    52: 19,
    53: 20,
}

# format -> (block_width, block_height, bits_per_pixel, dds_fourcc)
FORMAT_INFO = {
    6: (1, 1, 32, b"RGBA"),
    18: (4, 4, 4, b"DXT1"),
    19: (4, 4, 8, b"DXT3"),
    20: (4, 4, 8, b"DXT5"),
    49: (4, 4, 8, b"ATI2"),
    58: (4, 4, 4, b"ATI1"),
    59: (4, 4, 4, b"ATI1"),
    60: (4, 4, 4, b"ATI1"),
}

NHL_BASE_ONLY_PACKED_FIX_FORMATS = {18, 19, 20}
NHL_MIPPED_MATERIAL_FORMATS = {6, 18, 19, 20, 49, 50, 51, 52, 53, 58, 59, 60}


class NonTextureRx2(ValueError):
    pass


@dataclass
class ArenaObject:
    index: int
    offset: int
    size: int
    unknown1: int
    unknown2: int
    type_id: int
    type_name: str


@dataclass
class FetchInfo:
    dwords: list[int]
    type: int
    format_raw: int
    format_raw_name: str
    format_base: int
    format_base_name: str
    endianness: int
    endianness_name: str
    pitch_texels_div_32: int
    tiled: bool
    stacked: bool
    base_address_pages: int
    mip_address_pages: int
    width: int
    height: int
    depth_or_array_size: int
    dimension: int
    dimension_name: str
    packed_mips: bool
    mip_min_level_fetch: int
    mip_max_level_fetch: int
    mip_min_level_effective: int
    mip_max_level_effective: int
    swizzle: int
    signs: list[int]


@dataclass
class LevelProof:
    level: int
    width: int
    height: int
    stored_level: int
    packed: bool
    packed_offset_blocks: list[int]
    source_offset: int
    source_end_exact: int
    source_end_estimated: int
    input_pitch_blocks: int
    output_blocks: list[int]
    status: str


@dataclass
class RuntimeUploadLevelProof:
    level: int
    stored_level: int
    packed: bool
    host_row_pitch_bytes: int
    source_box_texels: list[int]
    direct_sha256: str
    runtime_sha256: str
    status: str


@dataclass
class RuntimeUploadProof:
    status: str
    levels: list[RuntimeUploadLevelProof]
    notes: list[str]


@dataclass
class RendererKeyProof:
    status: str
    format: str
    dimension: str
    width: int
    height: int
    mip_count: int
    raw_packed_mips: bool
    normalized_packed_mips: bool
    packed_level_raw: int | None
    packed_level_normalized: int | None
    base_only_packed_fix_applied: bool
    mipped_material_mips_preserved: bool
    tiny_base_packed_preserved: bool
    notes: list[str]


@dataclass
class RendererViewProof:
    status: str
    format: str
    dxgi_resource: str
    dxgi_unorm: str
    dxgi_snorm: str
    load_path: str
    decompressed: bool
    signed_view_available: bool
    signed_separate_resource: bool
    host_format_swizzle: str
    host_swizzle: str
    host_swizzle_hex: str
    swizzled_signs: list[str]
    swizzled_signs_hex: str
    notes: list[str]


@dataclass
class TextureProof:
    path: str
    file_size: int
    resource_offset: int
    resource_size: int
    fetch: FetchInfo
    packed_level: int | None
    base_required_bytes: int
    mips_required_bytes: int
    levels: list[LevelProof]
    runtime_upload: RuntimeUploadProof
    renderer_key: RendererKeyProof
    renderer_view: RendererViewProof
    dds_path: str | None
    png_path: str | None
    mip_png_paths: list[str]
    status: str
    notes: list[str]


@dataclass
class MaterialLayerProof:
    name: str
    path: str
    format: str
    width: int
    height: int
    mip_count: int
    packed_mips: bool
    status: str


@dataclass
class MaterialCompositeMetric:
    path: str
    lod: int
    width: int
    height: int
    neon_green_ratio: float
    purple_ratio: float
    transparent_ratio: float
    near_black_ratio: float
    status: str
    notes: list[str]


@dataclass
class MaterialColorSource:
    source: str
    table: str | None
    bit_order: str | None
    equipment_kind: str | None
    equipment_id: int | None
    record_count: int
    matched_record_count: int
    tint_colors: list[list[int]]
    notes: list[str]


@dataclass
class MaterialProof:
    spec: str
    directory: str
    asset_id: str
    prefix: str
    status: str
    layers: list[MaterialLayerProof]
    composite_png_paths: list[str]
    composite_metrics: list[MaterialCompositeMetric]
    color_source: MaterialColorSource | None
    notes: list[str]


@dataclass
class MaterialContractEntry:
    path: str
    directory: str
    layer_kind: str
    role: str
    format: str
    width: int
    height: int
    mip_count: int
    packed_mips: bool
    renderer_key_status: str
    renderer_view_status: str
    status: str
    notes: list[str]


@dataclass
class MaterialContractReport:
    root: str
    directories: list[str]
    count: int
    failed: int
    warnings: int
    by_layer_kind: dict[str, dict[str, int]]
    by_format: dict[str, int]
    proofs: list[MaterialContractEntry]


@dataclass
class LogAnalysisIssue:
    severity: str
    fetch: int | None
    message: str
    entry: dict


@dataclass
class LogAnalysis:
    path: str
    texture_entries: int
    sampler_entries: int
    unique_texture_fetches: int
    unique_sampler_fetches: int
    catalog_reports: list[str]
    catalog_entries: int
    catalog_strict_matches: int
    catalog_loose_matches: int
    catalog_fetch_shape_matches: int
    catalog_unmatched_texture_entries: int
    catalog_match_summaries: list[dict]
    issues: list[LogAnalysisIssue]
    notes: list[str]


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def round_up(value: int, alignment: int) -> int:
    return align(value, alignment)


def next_pow2(value: int) -> int:
    if value <= 1:
        return 1
    return 1 << (value - 1).bit_length()


def log2_floor(value: int) -> int:
    return value.bit_length() - 1


def log2_ceil(value: int) -> int:
    if value <= 1:
        return 0
    return (value - 1).bit_length()


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def be64(data: bytes, offset: int) -> int:
    return struct.unpack_from(">Q", data, offset)[0]


def parse_db_meta_table(meta_path: Path, table_name: str) -> dict:
    import xml.etree.ElementTree as ET

    root = ET.parse(meta_path).getroot()
    for table in root.iter("table"):
        if table.attrib.get("name") != table_name:
            continue
        fields = []
        fields_node = table.find("fields")
        if fields_node is None:
            raise ValueError(f"DB meta table {table_name} has no fields")
        for field in fields_node.iter("field"):
            fields.append(
                {
                    "name": field.attrib["name"],
                    "type": field.attrib.get("type", ""),
                    "depth": int(field.attrib.get("depth", "0")),
                    "rangehigh": int(field.attrib.get("rangehigh", "0")),
                    "rangelow": int(field.attrib.get("rangelow", "0")),
                }
            )
        return {
            "name": table_name,
            "shortname": table.attrib["shortname"],
            "fields": fields,
        }
    raise ValueError(f"DB meta table not found: {table_name}")


def parse_db_directory(data: bytes) -> dict[str, int]:
    if len(data) < 0x20 or data[:2] != b"DB":
        raise ValueError("not an NHL12 DB file")
    table_count = be32(data, 0x10)
    entries: dict[str, int] = {}
    entry_base = 0x20
    for index in range(table_count):
        entry = entry_base + index * 8
        if entry + 8 > len(data):
            break
        shortname = data[entry : entry + 4].decode("ascii", errors="replace")
        offset = be32(data, entry + 4)
        if 0 < offset < len(data):
            entries[shortname] = offset
    return entries


def read_bits(buf: bytes, bit_offset: int, width: int, bit_order: str) -> int:
    if bit_order == "msb":
        value = 0
        for i in range(width):
            byte = buf[(bit_offset + i) // 8]
            shift = 7 - ((bit_offset + i) % 8)
            value = (value << 1) | ((byte >> shift) & 1)
        return value
    value = 0
    for i in range(width):
        byte = buf[(bit_offset + i) // 8]
        shift = (bit_offset + i) % 8
        value |= ((byte >> shift) & 1) << i
    return value


def decode_db_table_records(meta_path: Path, db_path: Path, table_name: str,
                            bit_order: str) -> tuple[dict, list[dict]]:
    table_meta = parse_db_meta_table(meta_path, table_name)
    data = db_path.read_bytes()
    directory = parse_db_directory(data)
    shortname = table_meta["shortname"]
    if shortname not in directory:
        raise ValueError(f"DB table shortname {shortname} for {table_name} not found")
    offset = directory[shortname]
    following_offsets = sorted(value for value in directory.values() if value > offset)
    end = following_offsets[0] if following_offsets else len(data)
    bit_count = sum(field["depth"] for field in table_meta["fields"])
    row_bytes = (bit_count + 7) // 8
    if row_bytes == 0:
        raise ValueError(f"DB table {table_name} has zero row size")
    record_count = max((end - offset) // row_bytes, 0)
    records: list[dict] = []
    for record_index in range(record_count):
        row = data[offset + record_index * row_bytes : offset + (record_index + 1) * row_bytes]
        bit = 0
        values = {}
        for field in table_meta["fields"]:
            depth = field["depth"]
            if field["type"] == "DBOFIELDTYPE_INTEGER":
                values[field["name"]] = read_bits(row, bit, depth, bit_order)
            bit += depth
        records.append(values)
    table_meta = dict(table_meta)
    table_meta.update(
        {
            "offset": offset,
            "end": end,
            "row_bits": bit_count,
            "row_bytes": row_bytes,
            "record_count": record_count,
            "bit_order": bit_order,
        }
    )
    return table_meta, records


def parse_arena_objects(data: bytes) -> list[ArenaObject]:
    if len(data) < 0x40 or data[:8] != b"\x89RW4xb2\x00":
        raise ValueError("not an Xbox 360 RX2/RW4 arena")
    count = be32(data, 0x20)
    table_offset = be32(data, 0x30)
    if table_offset + count * 24 > len(data):
        raise ValueError("arena object table points outside file")
    first_object_offset = be32(data, 0x44) if len(data) >= 0x48 else 0
    objects: list[ArenaObject] = []
    for index in range(count):
        entry = table_offset + index * 24
        offset = be32(data, entry)
        size = be64(data, entry + 4)
        unknown1 = be32(data, entry + 12)
        unknown2 = be32(data, entry + 16)
        type_id = be32(data, entry + 20)
        if offset == 0:
            offset = first_object_offset
        if offset + size > len(data):
            raise ValueError(
                f"arena object {index} exceeds file: off=0x{offset:X} size=0x{size:X}"
            )
        objects.append(
            ArenaObject(
                index=index,
                offset=offset,
                size=size,
                unknown1=unknown1,
                unknown2=unknown2,
                type_id=type_id,
                type_name=OBJECT_TYPES.get(type_id, f"UNKNOWN_0x{type_id:08X}"),
            )
        )
    return objects


def decode_fetch(dwords: list[int], base_page_is_present: bool) -> FetchInfo:
    d0, d1, d2, d3, d4, d5 = dwords
    dimension = (d5 >> 9) & 0x3
    stacked = bool((d1 >> 10) & 1)
    if dimension in (1, 3):
        width = (d2 & 0x1FFF) + 1
        height = ((d2 >> 13) & 0x1FFF) + 1
        depth_or_array_size = (((d2 >> 26) & 0x3F) + 1) if stacked else (6 if dimension == 3 else 1)
    elif dimension == 2:
        width = (d2 & 0x7FF) + 1
        height = ((d2 >> 11) & 0x7FF) + 1
        depth_or_array_size = ((d2 >> 22) & 0x3FF) + 1
    else:
        width = (d2 & 0xFFFFFF) + 1
        height = 1
        depth_or_array_size = 1

    longest_axis = max(width, height, depth_or_array_size if dimension == 2 else 1)
    size_mip_max_level = log2_floor(longest_axis)
    mip_page = d5 >> 12
    if mip_page == 0:
        mip_min_level = 0
        mip_max_level = 0
    else:
        mip_min_level = min((d4 >> 2) & 0xF, size_mip_max_level)
        mip_max_level = max(min((d4 >> 6) & 0xF, size_mip_max_level), mip_min_level)

    # RX2 arenas store texture bytes relative to the base resource object, so
    # base_address can be zero even though the game will relocate it to a real
    # guest page at runtime. Emulate runtime relocation when a resource exists.
    effective_base_page = (d1 >> 12) & 0xFFFFF
    if effective_base_page == 0 and base_page_is_present:
        effective_base_page = 1

    if mip_max_level != 0:
        if effective_base_page == 0:
            mip_min_level = max(mip_min_level, 1)
        if mip_min_level != 0:
            effective_base_page = 0
    else:
        mip_page = 0

    format_raw = d1 & 0x3F
    format_base = BASE_FORMAT.get(format_raw, format_raw)
    signs = [(d0 >> 2) & 3, (d0 >> 4) & 3, (d0 >> 6) & 3, (d0 >> 8) & 3]
    return FetchInfo(
        dwords=dwords,
        type=d0 & 0x3,
        format_raw=format_raw,
        format_raw_name=FORMAT_NAMES.get(format_raw, f"format_{format_raw}"),
        format_base=format_base,
        format_base_name=FORMAT_NAMES.get(format_base, f"format_{format_base}"),
        endianness=(d1 >> 6) & 0x3,
        endianness_name=ENDIANS.get((d1 >> 6) & 0x3, "unknown"),
        pitch_texels_div_32=(d0 >> 22) & 0x1FF,
        tiled=bool((d0 >> 31) & 1),
        stacked=stacked,
        base_address_pages=(d1 >> 12) & 0xFFFFF,
        mip_address_pages=mip_page,
        width=width,
        height=height,
        depth_or_array_size=depth_or_array_size,
        dimension=dimension,
        dimension_name=DIMENSIONS.get(dimension, "unknown"),
        packed_mips=bool((d5 >> 11) & 1),
        mip_min_level_fetch=(d4 >> 2) & 0xF,
        mip_max_level_fetch=(d4 >> 6) & 0xF,
        mip_min_level_effective=mip_min_level,
        mip_max_level_effective=mip_max_level,
        swizzle=(d3 >> 1) & 0xFFF,
        signs=signs,
    )


def get_packed_mip_level(width: int, height: int) -> int:
    log2_size = log2_ceil(min(width, height))
    return log2_size - 4 if log2_size > 4 else 0


SWIZZLE_COMPONENT_NAMES = {
    0: "R",
    1: "G",
    2: "B",
    3: "A",
    4: "0",
    5: "1",
    6: "0",
    7: "1",
}

TEXTURE_SIGN_NAMES = {
    0: "unsigned",
    1: "signed",
    2: "unsigned_biased",
    3: "gamma",
}


def make_swizzle(text: str) -> int:
    values = {"R": 0, "G": 1, "B": 2, "A": 3, "0": 4, "1": 5}
    result = 0
    for index, char in enumerate(text):
        result |= values[char] << (index * 3)
    return result


D3D12_HOST_FORMATS = {
    6: {
        "dxgi_resource": "R8G8B8A8_TYPELESS",
        "dxgi_unorm": "R8G8B8A8_UNORM",
        "dxgi_snorm": "R8G8B8A8_SNORM",
        "dxgi_uncompressed": "UNKNOWN",
        "load_path": "native_32bpb",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RGBA"),
    },
    18: {
        "dxgi_resource": "BC1_UNORM",
        "dxgi_unorm": "BC1_UNORM",
        "dxgi_snorm": "UNKNOWN",
        "dxgi_uncompressed": "R8G8B8A8_UNORM",
        "load_path": "native_64bpb_or_dxt1_rgba8_decompress",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RGBA"),
    },
    19: {
        "dxgi_resource": "BC2_UNORM",
        "dxgi_unorm": "BC2_UNORM",
        "dxgi_snorm": "UNKNOWN",
        "dxgi_uncompressed": "R8G8B8A8_UNORM",
        "load_path": "native_128bpb_or_dxt3_rgba8_decompress",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RGBA"),
    },
    20: {
        "dxgi_resource": "BC3_UNORM",
        "dxgi_unorm": "BC3_UNORM",
        "dxgi_snorm": "UNKNOWN",
        "dxgi_uncompressed": "R8G8B8A8_UNORM",
        "load_path": "native_128bpb_or_dxt5_rgba8_decompress",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RGBA"),
    },
    49: {
        "dxgi_resource": "BC5_TYPELESS",
        "dxgi_unorm": "BC5_UNORM",
        "dxgi_snorm": "BC5_SNORM",
        "dxgi_uncompressed": "R8G8_UNORM",
        "load_path": "native_128bpb_or_dxn_rg8_decompress",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RGGG"),
    },
    51: {
        "dxgi_resource": "BC1_UNORM",
        "dxgi_unorm": "BC1_UNORM",
        "dxgi_snorm": "UNKNOWN",
        "dxgi_uncompressed": "R8G8B8A8_UNORM",
        "load_path": "native_64bpb_or_dxt1_rgba8_decompress",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RGBA"),
    },
    52: {
        "dxgi_resource": "BC2_UNORM",
        "dxgi_unorm": "BC2_UNORM",
        "dxgi_snorm": "UNKNOWN",
        "dxgi_uncompressed": "R8G8B8A8_UNORM",
        "load_path": "native_128bpb_or_dxt3_rgba8_decompress",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RGBA"),
    },
    53: {
        "dxgi_resource": "BC3_UNORM",
        "dxgi_unorm": "BC3_UNORM",
        "dxgi_snorm": "UNKNOWN",
        "dxgi_uncompressed": "R8G8B8A8_UNORM",
        "load_path": "native_128bpb_or_dxt5_rgba8_decompress",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RGBA"),
    },
    58: {
        "dxgi_resource": "R8_UNORM",
        "dxgi_unorm": "R8_UNORM",
        "dxgi_snorm": "UNKNOWN",
        "dxgi_uncompressed": "UNKNOWN",
        "load_path": "dxt3a_r8",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RRRR"),
    },
    59: {
        "dxgi_resource": "BC4_TYPELESS",
        "dxgi_unorm": "BC4_UNORM",
        "dxgi_snorm": "BC4_SNORM",
        "dxgi_uncompressed": "R8_UNORM",
        "load_path": "native_64bpb_or_dxt5a_r8_decompress",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RRRR"),
    },
    60: {
        "dxgi_resource": "R8G8_UNORM",
        "dxgi_unorm": "R8G8_UNORM",
        "dxgi_snorm": "UNKNOWN",
        "dxgi_uncompressed": "UNKNOWN",
        "load_path": "ctx1_rg8",
        "signed_separate_resource": False,
        "host_format_swizzle": make_swizzle("RGGG"),
    },
}


def swizzle_to_text(swizzle: int) -> str:
    return "".join(
        SWIZZLE_COMPONENT_NAMES[(swizzle >> (index * 3)) & 0b111] for index in range(4)
    )


def guest_to_host_swizzle(guest_swizzle: int, host_format_swizzle: int) -> int:
    host_swizzle = 0
    for index in range(4):
        guest_component = (guest_swizzle >> (3 * index)) & 0b111
        if guest_component >= 4:
            host_component = guest_component & 0b101
        else:
            host_component = (host_format_swizzle >> (3 * guest_component)) & 0b111
        host_swizzle |= host_component << (3 * index)
    return host_swizzle


def swizzle_signs(fetch: FetchInfo) -> int:
    signs = 0
    any_not_signed = False
    any_signed = False
    constant_mask = 0
    for index in range(4):
        swizzle = (fetch.swizzle >> (index * 3)) & 0b111
        if swizzle & 0b100:
            constant_mask |= 1 << (index * 2)
        else:
            sign = fetch.signs[swizzle]
            signs |= sign << (index * 2)
            if sign == 1:
                any_signed = True
            else:
                any_not_signed = True
    constants_sign = 0
    if constant_mask == 0b01010101:
        if fetch.signs == [1, 1, 1, 1]:
            constants_sign = 1
    elif any_signed and not any_not_signed:
        constants_sign = 1
    signs |= constants_sign * constant_mask
    return signs


def signs_to_names(packed_signs: int) -> list[str]:
    return [
        TEXTURE_SIGN_NAMES.get((packed_signs >> (index * 2)) & 0b11, "unknown")
        for index in range(4)
    ]


def packed_signs_has_signed(packed_signs: int) -> bool:
    return any(((packed_signs >> (index * 2)) & 0b11) == 1 for index in range(4))


def packed_signs_has_not_signed(packed_signs: int) -> bool:
    return any(((packed_signs >> (index * 2)) & 0b11) != 1 for index in range(4))


def is_d3d12_decompression_needed(fetch: FetchInfo) -> bool:
    host_format = D3D12_HOST_FORMATS.get(fetch.format_base)
    if not host_format or host_format["dxgi_uncompressed"] == "UNKNOWN":
        return False
    block_width, block_height, _, _ = FORMAT_INFO[fetch.format_base]
    return bool(fetch.width & (block_width - 1) or fetch.height & (block_height - 1))


def prove_renderer_view(fetch: FetchInfo) -> RendererViewProof:
    status = "PASS"
    notes: list[str] = []
    host_format = D3D12_HOST_FORMATS.get(fetch.format_base)
    if not host_format:
        return RendererViewProof(
            status="SKIP",
            format=fetch.format_base_name,
            dxgi_resource="UNKNOWN",
            dxgi_unorm="UNKNOWN",
            dxgi_snorm="UNKNOWN",
            load_path="unknown",
            decompressed=False,
            signed_view_available=False,
            signed_separate_resource=False,
            host_format_swizzle="0000",
            host_swizzle="0000",
            host_swizzle_hex="0x000",
            swizzled_signs=[],
            swizzled_signs_hex="0x00",
            notes=["format is outside the NHL12 equipment/jersey material proof set"],
        )

    decompressed = is_d3d12_decompression_needed(fetch)
    dxgi_resource = (
        host_format["dxgi_uncompressed"] if decompressed else host_format["dxgi_resource"]
    )
    dxgi_unorm = host_format["dxgi_uncompressed"] if decompressed else host_format["dxgi_unorm"]
    dxgi_snorm = "UNKNOWN" if decompressed else host_format["dxgi_snorm"]
    host_format_swizzle = int(host_format["host_format_swizzle"])
    host_swizzle = guest_to_host_swizzle(fetch.swizzle, host_format_swizzle)
    packed_signs = swizzle_signs(fetch)
    signed_view_available = dxgi_snorm != "UNKNOWN"

    if fetch.format_base == 49:
        if dxgi_resource != "BC5_TYPELESS" or dxgi_unorm != "BC5_UNORM":
            status = "FAIL"
            notes.append("DXN normal map is not using native BC5 UNORM resource/view path.")
        if not signed_view_available:
            status = "FAIL"
            notes.append("DXN normal map lost its SNORM SRV option.")
        notes.append("DXN uses UNORM/SNORM SRV views over one typeless BC5 resource.")
    if fetch.format_base in (18, 19, 20) and fetch.mip_max_level_effective > 0:
        if decompressed:
            status = "FAIL"
            notes.append("Mipped DXT material unexpectedly falls onto decompression path.")
        if not dxgi_resource.startswith("BC"):
            status = "FAIL"
            notes.append("Mipped DXT material is not using native BC resource path.")
    if fetch.format_base in (18, 19, 20) and fetch.mip_max_level_effective == 0:
        notes.append("Base-only DXT material keeps native BC view unless dimensions require decompression.")
    if decompressed:
        notes.append("Unaligned dimensions require D3D12 decompression path.")

    return RendererViewProof(
        status=status,
        format=fetch.format_base_name,
        dxgi_resource=dxgi_resource,
        dxgi_unorm=dxgi_unorm,
        dxgi_snorm=dxgi_snorm,
        load_path=str(host_format["load_path"]),
        decompressed=decompressed,
        signed_view_available=signed_view_available,
        signed_separate_resource=bool(host_format["signed_separate_resource"]),
        host_format_swizzle=swizzle_to_text(host_format_swizzle),
        host_swizzle=swizzle_to_text(host_swizzle),
        host_swizzle_hex=f"0x{host_swizzle:03X}",
        swizzled_signs=signs_to_names(packed_signs),
        swizzled_signs_hex=f"0x{packed_signs:02X}",
        notes=notes,
    )


def prove_renderer_key_normalization(fetch: FetchInfo) -> RendererKeyProof:
    """Mirror the NHL12 texture-key rules that can corrupt equipment materials."""
    mip_count = fetch.mip_max_level_effective + 1
    raw_packed = fetch.packed_mips
    raw_packed_level = get_packed_mip_level(fetch.width, fetch.height) if raw_packed else None
    normalized_packed = raw_packed
    base_only_packed_fix_applied = False
    tiny_base_packed_preserved = False
    notes: list[str] = []
    status = "PASS"

    if (
        raw_packed
        and fetch.dimension == 1
        and fetch.mip_max_level_effective == 0
        and fetch.format_base in NHL_BASE_ONLY_PACKED_FIX_FORMATS
    ):
        if get_packed_mip_level(fetch.width, fetch.height) != 0:
            normalized_packed = False
            base_only_packed_fix_applied = True
            notes.append("NHL12 strips stale packed state from non-tiny base-only DXT material fetch.")
        else:
            tiny_base_packed_preserved = True
            notes.append("NHL12 preserves packed state because level 0 itself is in the packed tail.")

    normalized_packed_level = (
        get_packed_mip_level(fetch.width, fetch.height) if normalized_packed else None
    )
    is_mipped_material = (
        fetch.dimension == 1
        and fetch.format_base in NHL_MIPPED_MATERIAL_FORMATS
        and mip_count > 1
    )
    mipped_material_mips_preserved = (not is_mipped_material) or normalized_packed
    if is_mipped_material and not normalized_packed:
        status = "FAIL"
        notes.append("Mipped NHL12 equipment/jersey material lost packed mip state.")

    if (
        fetch.dimension == 1
        and fetch.format_base in NHL_BASE_ONLY_PACKED_FIX_FORMATS
        and fetch.mip_max_level_effective == 0
        and raw_packed
        and get_packed_mip_level(fetch.width, fetch.height) != 0
        and normalized_packed
    ):
        status = "FAIL"
        notes.append("Non-tiny base-only DXT material kept stale packed state.")

    if (
        fetch.dimension == 1
        and fetch.format_base in NHL_BASE_ONLY_PACKED_FIX_FORMATS
        and fetch.mip_max_level_effective == 0
        and raw_packed
        and get_packed_mip_level(fetch.width, fetch.height) == 0
        and not normalized_packed
    ):
        status = "FAIL"
        notes.append("Tiny base-only DXT material lost its required packed-base layout.")

    if is_mipped_material and normalized_packed:
        notes.append("Real mipped NHL12 material preserves packed mip state for runtime upload.")

    return RendererKeyProof(
        status=status,
        format=fetch.format_base_name,
        dimension=fetch.dimension_name,
        width=fetch.width,
        height=fetch.height,
        mip_count=mip_count,
        raw_packed_mips=raw_packed,
        normalized_packed_mips=normalized_packed,
        packed_level_raw=raw_packed_level,
        packed_level_normalized=normalized_packed_level,
        base_only_packed_fix_applied=base_only_packed_fix_applied,
        mipped_material_mips_preserved=mipped_material_mips_preserved,
        tiny_base_packed_preserved=tiny_base_packed_preserved,
        notes=notes,
    )


def make_renderer_key_test_fetch(name: str, fmt: int, width: int, height: int, mip_count: int,
                                 packed_mips: bool) -> FetchInfo:
    mip_max = max(mip_count - 1, 0)
    return FetchInfo(
        dwords=[],
        type=0,
        format_raw=fmt,
        format_raw_name=FORMAT_NAMES.get(fmt, f"format_{fmt}"),
        format_base=BASE_FORMAT.get(fmt, fmt),
        format_base_name=FORMAT_NAMES.get(BASE_FORMAT.get(fmt, fmt), f"format_{fmt}"),
        endianness=3,
        endianness_name=ENDIANS[3],
        pitch_texels_div_32=max(align(width, 32) // 32, 1),
        tiled=True,
        stacked=False,
        base_address_pages=1,
        mip_address_pages=1 if mip_max else 0,
        width=width,
        height=height,
        depth_or_array_size=1,
        dimension=1,
        dimension_name=DIMENSIONS[1],
        packed_mips=packed_mips,
        mip_min_level_fetch=0,
        mip_max_level_fetch=mip_max,
        mip_min_level_effective=0,
        mip_max_level_effective=mip_max,
        swizzle=0,
        signs=[0, 0, 0, 0],
    )


def make_renderer_key_test_fetch_from_dwords(dwords: list[int]) -> FetchInfo:
    return decode_fetch(dwords, base_page_is_present=True)


def run_renderer_key_self_tests() -> dict:
    cases = [
        {
            "name": "non_tiny_base_only_dxt1_stale_packed",
            "fetch": make_renderer_key_test_fetch(
                "non_tiny_base_only_dxt1_stale_packed", 18, 512, 512, 1, True
            ),
            "expect": {
                "normalized_packed_mips": False,
                "base_only_packed_fix_applied": True,
            },
        },
        {
            "name": "tiny_base_only_dxt1_real_packed_base",
            "fetch": make_renderer_key_test_fetch(
                "tiny_base_only_dxt1_real_packed_base", 18, 16, 16, 1, True
            ),
            "expect": {
                "normalized_packed_mips": True,
                "tiny_base_packed_preserved": True,
            },
        },
        {
            "name": "mipped_dxt1_shine_preserves_packed_mips",
            "fetch": make_renderer_key_test_fetch(
                "mipped_dxt1_shine_preserves_packed_mips", 18, 128, 128, 8, True
            ),
            "expect": {
                "normalized_packed_mips": True,
                "mipped_material_mips_preserved": True,
            },
        },
        {
            "name": "mipped_dxn_normal_preserves_packed_mips",
            "fetch": make_renderer_key_test_fetch(
                "mipped_dxn_normal_preserves_packed_mips", 49, 512, 512, 10, True
            ),
            "expect": {
                "normalized_packed_mips": True,
                "mipped_material_mips_preserved": True,
            },
        },
        {
            "name": "mipped_dxt5_normal_preserves_packed_mips",
            "fetch": make_renderer_key_test_fetch(
                "mipped_dxt5_normal_preserves_packed_mips", 20, 512, 512, 10, True
            ),
            "expect": {
                "normalized_packed_mips": True,
                "mipped_material_mips_preserved": True,
            },
        },
        {
            "name": "mipped_k8888_support_map_preserves_packed_mips",
            "fetch": make_renderer_key_test_fetch(
                "mipped_k8888_support_map_preserves_packed_mips", 6, 256, 256, 9, True
            ),
            "expect": {
                "normalized_packed_mips": True,
                "mipped_material_mips_preserved": True,
            },
        },
        {
            "name": "normalplay_generated_dxt1_1280x720_strips_stale_packed",
            "fetch": make_renderer_key_test_fetch_from_dwords(
                [0x8A000002, 0x11405052, 0x0059E4FF, 0x00A80D10, 0x00000003, 0x00000A00]
            ),
            "expect": {
                "normalized_packed_mips": False,
                "base_only_packed_fix_applied": True,
            },
        },
        {
            "name": "normalplay_generated_dxt5_32x32_strips_stale_packed",
            "fetch": make_renderer_key_test_fetch_from_dwords(
                [0x81024802, 0x14B1C054, 0x0003E01F, 0x00A80D10, 0x00000003, 0x00000A00]
            ),
            "expect": {
                "normalized_packed_mips": False,
                "base_only_packed_fix_applied": True,
            },
        },
        {
            "name": "normalplay_generated_dxt5_2048x1024_strips_stale_packed",
            "fetch": make_renderer_key_test_fetch_from_dwords(
                [0x90024802, 0x127A3054, 0x007FE7FF, 0x00A80D10, 0x00000003, 0x00000A01]
            ),
            "expect": {
                "normalized_packed_mips": False,
                "base_only_packed_fix_applied": True,
            },
        },
        {
            "name": "normalplay_generated_dxt3_64x160_strips_stale_packed",
            "fetch": make_renderer_key_test_fetch_from_dwords(
                [0x81000002, 0x114CF053, 0x0013E03F, 0x00A80D10, 0x00000003, 0x00000A00]
            ),
            "expect": {
                "normalized_packed_mips": False,
                "base_only_packed_fix_applied": True,
            },
        },
    ]

    results = []
    failed = 0
    for case in cases:
        proof = prove_renderer_key_normalization(case["fetch"])
        failures = []
        for key, expected_value in case["expect"].items():
            actual_value = getattr(proof, key)
            if actual_value != expected_value:
                failures.append(
                    f"{key}: expected {expected_value!r}, got {actual_value!r}"
                )
        if proof.status != "PASS":
            failures.append(f"proof status was {proof.status}")
        status = "PASS" if not failures else "FAIL"
        if failures:
            failed += 1
        results.append(
            {
                "name": case["name"],
                "status": status,
                "failures": failures,
                "proof": asdict(proof),
            }
        )

    return {
        "count": len(results),
        "failed": failed,
        "results": results,
    }


def get_packed_mip_offset(width: int, height: int, depth: int, fmt: int, mip: int) -> tuple[int, int, int, bool]:
    block_width, block_height, _, _ = FORMAT_INFO[fmt]
    log2_width = log2_ceil(width)
    log2_height = log2_ceil(height)
    log2_size = min(log2_width, log2_height)
    if log2_size > 4 + mip:
        return 0, 0, 0, False
    packed_mip_base = log2_size - 4 if log2_size > 4 else 0
    packed_mip = mip - packed_mip_base
    if packed_mip < 3:
        if log2_width > log2_height:
            x_blocks = 0
            y_blocks = 16 >> packed_mip
        else:
            x_blocks = 16 >> packed_mip
            y_blocks = 0
        z_blocks = 0
    else:
        if log2_width > log2_height:
            offset = (1 << (log2_width - packed_mip_base)) >> (packed_mip - 2)
            x_blocks = offset
            y_blocks = 0
        else:
            x_blocks = 0
            offset = (1 << (log2_height - packed_mip_base)) >> (packed_mip - 2)
            y_blocks = offset
        if offset < 4:
            log2_depth = log2_ceil(depth)
            z_blocks = (log2_depth - packed_mip) * 4 if log2_depth > 1 + packed_mip else 4
        else:
            z_blocks = 0
    return x_blocks // block_width, y_blocks // block_height, z_blocks, True


def tiled_offset_2d(x: int, y: int, pitch: int, bytes_per_block_log2: int) -> int:
    pitch = align(pitch, 32)
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bytes_per_block_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (
        ((offset & ~0x1FF) << 3)
        + ((y & 16) << 7)
        + ((offset & 0x1C0) << 2)
        + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)
        + (offset & 0x3F)
    )


def tiled_upper_bound_2d(width: int, height: int, pitch: int, bytes_per_block_log2: int) -> int:
    if width == 0 or height == 0:
        return 0
    bottom = height
    pitch_aligned = align(pitch, 32)
    upper = tiled_offset_2d(width - 1, bottom - 1, pitch, bytes_per_block_log2)
    upper = max(
        upper,
        tiled_offset_2d(width - 1, (bottom - 1) & ~(32 - 1), pitch, bytes_per_block_log2),
    )
    upper = max(
        upper,
        tiled_offset_2d(0, bottom - 1, pitch, bytes_per_block_log2),
    )
    upper = max(
        upper,
        tiled_offset_2d(0, (bottom - 1) & ~(32 - 1), pitch, bytes_per_block_log2),
    )
    if bytes_per_block_log2 == 0:
        upper += 0x1000 if pitch_aligned & (128 - 1) else 0xC00
    elif bytes_per_block_log2 == 1:
        upper += 0x1000 if pitch_aligned & (64 - 1) else 0xC00
    else:
        upper += 0x400 << bytes_per_block_log2
    return upper


def texture_layout(fetch: FetchInfo) -> dict:
    if fetch.dimension != 1:
        raise ValueError(f"only 2D RX2 textures are supported, got {fetch.dimension_name}")
    fmt = fetch.format_base
    if fmt not in FORMAT_INFO:
        raise ValueError(f"unsupported texture format {fetch.format_base_name}")

    block_width, block_height, bpp, _ = FORMAT_INFO[fmt]
    bytes_per_block = block_width * block_height * bpp // 8
    bytes_log2 = log2_floor(bytes_per_block)
    width = fetch.width
    height = fetch.height
    depth = 1
    max_level = min(fetch.mip_max_level_effective, log2_floor(max(width, height, depth)))
    packed_level = get_packed_mip_level(width, height) if fetch.packed_mips else None
    packed_for_calc = packed_level if packed_level is not None else 0xFFFFFFFF

    def make_level(level: int, is_base: bool) -> dict:
        if is_base:
            row_pitch_texels = fetch.pitch_texels_div_32 << 5
            row_texel_rows = height
        else:
            row_pitch_texels = max(next_pow2(width) >> level, 1)
            row_texel_rows = max(next_pow2(height) >> level, 1)
        row_pitch_blocks = align(align(row_pitch_texels, block_width) // block_width, 32)
        row_pitch_bytes = row_pitch_blocks * bytes_per_block
        if not fetch.tiled and not is_base:
            row_pitch_bytes = align(row_pitch_bytes, 256)
            row_pitch_blocks = row_pitch_bytes // bytes_per_block
        z_stride_rows = align(align(row_texel_rows, block_height) // block_height, 32)
        array_stride = align(row_pitch_bytes * z_stride_rows, 4096)
        if level == packed_for_calc:
            x_extent = 0
            y_extent = 0
            z_extent = 0
            packed_last = 0 if is_base else max_level
            for sub in range(packed_for_calc, packed_last + 1):
                ox, oy, oz, _ = get_packed_mip_offset(width, height, depth, fmt, sub)
                sub_w_blocks = align(max(width >> sub, 1), block_width) // block_width
                sub_h_blocks = align(max(height >> sub, 1), block_height) // block_height
                x_extent = max(x_extent, ox + sub_w_blocks)
                y_extent = max(y_extent, oy + sub_h_blocks)
                z_extent = max(z_extent, oz + max(depth >> sub, 1))
        else:
            x_extent = align(max(width >> level, 1), block_width) // block_width
            y_extent = align(max(height >> level, 1), block_height) // block_height
            z_extent = max(depth >> level, 1)
        if fetch.tiled:
            data_extent = tiled_upper_bound_2d(x_extent, y_extent, row_pitch_blocks, bytes_log2)
        else:
            data_extent = row_pitch_bytes * (y_extent - 1) + bytes_per_block * x_extent
        return {
            "row_pitch_bytes": row_pitch_bytes,
            "row_pitch_blocks": row_pitch_blocks,
            "array_slice_stride_bytes": array_stride,
            "x_extent_blocks": x_extent,
            "y_extent_blocks": y_extent,
            "z_extent": z_extent,
            "level_data_extent_bytes": data_extent,
        }

    layout = {
        "base": make_level(0, True),
        "mips": {},
        "mip_offsets": {},
        "packed_level": packed_level,
        "mips_total_extent_bytes": 0,
        "max_level": max_level,
    }
    if max_level > 0:
        max_stored = min(max_level, packed_for_calc)
        offset = 0
        for level in range(1, max_stored + 1):
            level_layout = make_level(level, False)
            layout["mips"][level] = level_layout
            layout["mip_offsets"][level] = offset
            layout["mips_total_extent_bytes"] = max(
                layout["mips_total_extent_bytes"], offset + level_layout["level_data_extent_bytes"]
            )
            offset += level_layout["array_slice_stride_bytes"]
    return layout


def copy_swap_block(block: bytes, endian: int) -> bytes:
    b = bytearray(block)
    if endian == 1:
        out = bytearray(len(b))
        for i in range(0, len(b), 2):
            out[i : i + 2] = b[i : i + 2][::-1]
        return bytes(out)
    if endian == 2:
        out = bytearray(len(b))
        for i in range(0, len(b), 4):
            out[i : i + 4] = b[i : i + 4][::-1]
        return bytes(out)
    if endian == 3:
        out = bytearray(len(b))
        for i in range(0, len(b), 4):
            out[i : i + 4] = b[i + 2 : i + 4] + b[i : i + 2]
        return bytes(out)
    return bytes(b)


def linearize_level(data: bytes, source_offset: int, source_limit: int, layout_level: dict,
                    fetch: FetchInfo, level: int, packed_offset: tuple[int, int, int]) -> bytes:
    fmt = fetch.format_base
    block_width, block_height, bpp, _ = FORMAT_INFO[fmt]
    bytes_per_block = block_width * block_height * bpp // 8
    bytes_log2 = log2_floor(bytes_per_block)
    out_w_blocks = align(max(fetch.width >> level, 1), block_width) // block_width
    out_h_blocks = align(max(fetch.height >> level, 1), block_height) // block_height
    output = bytearray(out_w_blocks * out_h_blocks * bytes_per_block)
    input_pitch = layout_level["row_pitch_blocks"]
    offset_x, offset_y, _ = packed_offset
    for y in range(out_h_blocks):
        for x in range(out_w_blocks):
            if fetch.tiled:
                src_rel = tiled_offset_2d(x + offset_x, y + offset_y, input_pitch, bytes_log2)
            else:
                src_rel = (y + offset_y) * layout_level["row_pitch_bytes"] + (
                    x + offset_x
                ) * bytes_per_block
            src = source_offset + src_rel
            dst = (y * out_w_blocks + x) * bytes_per_block
            if src < 0 or src + bytes_per_block > source_limit:
                continue
            output[dst : dst + bytes_per_block] = copy_swap_block(
                data[src : src + bytes_per_block], fetch.endianness
            )
    return bytes(output)


def linearize_region_to_d3d12_upload(data: bytes, source_offset: int, source_limit: int,
                                     layout_level: dict, fetch: FetchInfo,
                                     x_blocks: int, y_blocks: int,
                                     host_row_pitch_bytes: int) -> bytes:
    fmt = fetch.format_base
    _, _, bpp, _ = FORMAT_INFO[fmt]
    bytes_per_block = FORMAT_INFO[fmt][0] * FORMAT_INFO[fmt][1] * bpp // 8
    bytes_log2 = log2_floor(bytes_per_block)
    output = bytearray(host_row_pitch_bytes * y_blocks)
    input_pitch = layout_level["row_pitch_blocks"]
    for y in range(y_blocks):
        for x in range(x_blocks):
            if fetch.tiled:
                src_rel = tiled_offset_2d(x, y, input_pitch, bytes_log2)
            else:
                src_rel = y * layout_level["row_pitch_bytes"] + x * bytes_per_block
            src = source_offset + src_rel
            dst = y * host_row_pitch_bytes + x * bytes_per_block
            if src < 0 or src + bytes_per_block > source_limit:
                continue
            output[dst : dst + bytes_per_block] = copy_swap_block(
                data[src : src + bytes_per_block], fetch.endianness
            )
    return bytes(output)


def d3d12_upload_row_pitch_bytes(fetch: FetchInfo, level_width_texels: int,
                                 row_pitch_thread_blocks_log2: int) -> int:
    block_width, block_height, bpp, _ = FORMAT_INFO[fetch.format_base]
    bytes_per_block = block_width * block_height * bpp // 8
    width_rounded = align(level_width_texels, block_width)
    x_blocks = width_rounded // block_width
    x_blocks = align(x_blocks, 1 << row_pitch_thread_blocks_log2)
    return align(x_blocks * bytes_per_block, 256)


def d3d12_row_pitch_thread_blocks_log2(fetch: FetchInfo) -> int:
    # Direct BC copies use RexGlue's k64bpb or k128bpb upload shaders. Those
    # shaders pad rows to the number of guest blocks copied by a single thread.
    if FORMAT_INFO[fetch.format_base][2] == 4:
        return 2
    if FORMAT_INFO[fetch.format_base][2] == 32:
        return 3
    return 1


def sha256_short(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]


def prove_runtime_upload_equivalent(data: bytes, resource: ArenaObject, fetch: FetchInfo,
                                    layout: dict, linear_mips: list[bytes]) -> RuntimeUploadProof:
    fmt = fetch.format_base
    block_width, block_height, bpp, _ = FORMAT_INFO[fmt]
    bytes_per_block = block_width * block_height * bpp // 8
    row_pitch_thread_blocks_log2 = d3d12_row_pitch_thread_blocks_log2(fetch)
    base_data_offset = resource.offset + fetch.base_address_pages * 4096
    mip_data_offset = resource.offset + fetch.mip_address_pages * 4096
    resource_end = resource.offset + resource.size
    packed_level = layout["packed_level"]
    upload_scratch: dict[int, tuple[bytes, int]] = {}
    notes: list[str] = []

    for level in range(0, fetch.mip_max_level_effective + 1):
        if level == 0:
            stored_level = 0
            layout_level = layout["base"]
            source_offset = base_data_offset
        else:
            stored_level = min(level, packed_level) if packed_level is not None else level
            if stored_level in upload_scratch:
                continue
            layout_level = layout["mips"][stored_level]
            source_offset = mip_data_offset + layout["mip_offsets"][stored_level]

        is_packed_tail = packed_level is not None and stored_level == packed_level
        if is_packed_tail:
            region_w_blocks = layout_level["x_extent_blocks"]
            region_h_blocks = layout_level["y_extent_blocks"]
            row_pitch_width_texels = region_w_blocks * block_width
        else:
            region_w_blocks = align(max(fetch.width >> stored_level, 1), block_width) // block_width
            region_h_blocks = align(max(fetch.height >> stored_level, 1), block_height) // block_height
            row_pitch_width_texels = max(fetch.width >> stored_level, 1)

        host_row_pitch = d3d12_upload_row_pitch_bytes(
            fetch, row_pitch_width_texels, row_pitch_thread_blocks_log2
        )
        scratch = linearize_region_to_d3d12_upload(
            data, source_offset, resource_end, layout_level, fetch,
            region_w_blocks, region_h_blocks, host_row_pitch
        )
        upload_scratch[stored_level] = (scratch, host_row_pitch)

    levels: list[RuntimeUploadLevelProof] = []
    status = "PASS"
    for level, direct in enumerate(linear_mips):
        if level == 0:
            stored_level = 0
        else:
            stored_level = min(level, packed_level) if packed_level is not None else level
        scratch, host_row_pitch = upload_scratch[stored_level]
        packed = packed_level is not None and level >= packed_level
        if packed:
            packed_offset = get_packed_mip_offset(fetch.width, fetch.height, 1, fmt, level)[:3]
        else:
            packed_offset = (0, 0, 0)
        out_w_blocks = align(max(fetch.width >> level, 1), block_width) // block_width
        out_h_blocks = align(max(fetch.height >> level, 1), block_height) // block_height
        runtime = bytearray(out_w_blocks * out_h_blocks * bytes_per_block)
        for y in range(out_h_blocks):
            src = ((y + packed_offset[1]) * host_row_pitch +
                   packed_offset[0] * bytes_per_block)
            dst = y * out_w_blocks * bytes_per_block
            count = out_w_blocks * bytes_per_block
            runtime[dst : dst + count] = scratch[src : src + count]
        runtime_bytes = bytes(runtime)
        level_status = "PASS" if runtime_bytes == direct else "MISMATCH"
        if level_status != "PASS":
            status = "FAIL"
        levels.append(
            RuntimeUploadLevelProof(
                level=level,
                stored_level=stored_level,
                packed=packed,
                host_row_pitch_bytes=host_row_pitch,
                source_box_texels=[
                    packed_offset[0] * block_width,
                    packed_offset[1] * block_height,
                    packed_offset[2],
                    (packed_offset[0] + out_w_blocks) * block_width,
                    (packed_offset[1] + out_h_blocks) * block_height,
                    packed_offset[2] + 1,
                ],
                direct_sha256=sha256_short(direct),
                runtime_sha256=sha256_short(runtime_bytes),
                status=level_status,
            )
        )

    notes.append("Simulates RexGlue D3D12 upload padding plus packed-tail CopyTextureRegion boxes.")
    return RuntimeUploadProof(status=status, levels=levels, notes=notes)


def exact_level_read_end(source_offset: int, layout_level: dict, fetch: FetchInfo, level: int,
                         packed_offset: tuple[int, int, int]) -> int:
    fmt = fetch.format_base
    block_width, block_height, bpp, _ = FORMAT_INFO[fmt]
    bytes_per_block = block_width * block_height * bpp // 8
    bytes_log2 = log2_floor(bytes_per_block)
    out_w_blocks = align(max(fetch.width >> level, 1), block_width) // block_width
    out_h_blocks = align(max(fetch.height >> level, 1), block_height) // block_height
    input_pitch = layout_level["row_pitch_blocks"]
    offset_x, offset_y, _ = packed_offset
    max_end = source_offset
    for y in range(out_h_blocks):
        for x in range(out_w_blocks):
            if fetch.tiled:
                src_rel = tiled_offset_2d(x + offset_x, y + offset_y, input_pitch, bytes_log2)
            else:
                src_rel = (y + offset_y) * layout_level["row_pitch_bytes"] + (
                    x + offset_x
                ) * bytes_per_block
            max_end = max(max_end, source_offset + src_rel + bytes_per_block)
    return max_end


def rgb565(value: int) -> tuple[int, int, int]:
    r = ((value >> 11) & 0x1F) * 255 // 31
    g = ((value >> 5) & 0x3F) * 255 // 63
    b = (value & 0x1F) * 255 // 31
    return r, g, b


def decode_bc_color_block(block: bytes, force_four_color: bool) -> list[tuple[int, int, int, int]]:
    c0, c1, bits = struct.unpack_from("<HHI", block, 0)
    c0_rgb = rgb565(c0)
    c1_rgb = rgb565(c1)
    colors: list[tuple[int, int, int, int]] = [
        (*c0_rgb, 255),
        (*c1_rgb, 255),
        (0, 0, 0, 255),
        (0, 0, 0, 255),
    ]
    if force_four_color or c0 > c1:
        colors[2] = tuple((2 * c0_rgb[i] + c1_rgb[i]) // 3 for i in range(3)) + (255,)
        colors[3] = tuple((c0_rgb[i] + 2 * c1_rgb[i]) // 3 for i in range(3)) + (255,)
    else:
        colors[2] = tuple((c0_rgb[i] + c1_rgb[i]) // 2 for i in range(3)) + (255,)
        colors[3] = (0, 0, 0, 0)
    pixels: list[tuple[int, int, int, int]] = []
    for i in range(16):
        pixels.append(colors[(bits >> (i * 2)) & 3])
    return pixels


def decode_bc4_values(block: bytes) -> list[int]:
    a0 = block[0]
    a1 = block[1]
    bits = int.from_bytes(block[2:8], "little")
    table = [a0, a1]
    if a0 > a1:
        table.extend(((7 - i) * a0 + i * a1) // 7 for i in range(1, 7))
    else:
        table.extend(((5 - i) * a0 + i * a1) // 5 for i in range(1, 5))
        table.extend([0, 255])
    return [table[(bits >> (i * 3)) & 7] for i in range(16)]


def decode_preview_rgba(linear_level: bytes, width: int, height: int, fmt: int) -> bytes:
    block_width, block_height, bpp, _ = FORMAT_INFO[fmt]
    bytes_per_block = block_width * block_height * bpp // 8
    if fmt == 6:
        pixels = bytearray(width * height * 4)
        for y in range(height):
            row_src = y * width * bytes_per_block
            row_dst = y * width * 4
            pixels[row_dst : row_dst + width * 4] = linear_level[
                row_src : row_src + width * 4
            ]
        return bytes(pixels)
    blocks_w = align(width, block_width) // block_width
    blocks_h = align(height, block_height) // block_height
    pixels = bytearray(width * height * 4)
    for by in range(blocks_h):
        for bx in range(blocks_w):
            block = linear_level[(by * blocks_w + bx) * bytes_per_block :][:bytes_per_block]
            rgba: list[tuple[int, int, int, int]]
            if fmt == 18:
                rgba = decode_bc_color_block(block, False)
            elif fmt == 19:
                alpha_bits = int.from_bytes(block[:8], "little")
                color = decode_bc_color_block(block[8:16], True)
                rgba = []
                for i, (r, g, b, _) in enumerate(color):
                    a = ((alpha_bits >> (i * 4)) & 0xF) * 17
                    rgba.append((r, g, b, a))
            elif fmt == 20:
                alpha = decode_bc4_values(block[:8])
                color = decode_bc_color_block(block[8:16], True)
                rgba = [(r, g, b, alpha[i]) for i, (r, g, b, _) in enumerate(color)]
            elif fmt == 49:
                red = decode_bc4_values(block[:8])
                green = decode_bc4_values(block[8:16])
                rgba = []
                for r, g in zip(red, green):
                    nx = r / 255.0 * 2.0 - 1.0
                    ny = g / 255.0 * 2.0 - 1.0
                    nz = math.sqrt(max(0.0, 1.0 - nx * nx - ny * ny))
                    rgba.append((r, g, int(nz * 255), 255))
            else:
                rgba = [(0, 0, 0, 255)] * 16
            for py in range(4):
                for px in range(4):
                    x = bx * 4 + px
                    y = by * 4 + py
                    if x >= width or y >= height:
                        continue
                    dst = (y * width + x) * 4
                    pixels[dst : dst + 4] = bytes(rgba[py * 4 + px])
    return bytes(pixels)


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            i = (y * width + x) * 4
            rows.extend(rgba[i : i + 4])
    payload = b"\x89PNG\r\n\x1a\n"
    payload += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    payload += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    payload += chunk(b"IEND", b"")
    path.write_bytes(payload)


def write_dds(path: Path, fetch: FetchInfo, mip_data: list[bytes]) -> None:
    fmt = fetch.format_base
    block_width, block_height, bpp, fourcc = FORMAT_INFO[fmt]
    linear_size = len(mip_data[0])
    if fmt == 6:
        flags = 0x0000100F  # CAPS | HEIGHT | WIDTH | PIXELFORMAT | PITCH
    else:
        flags = 0x0002100F  # CAPS | HEIGHT | WIDTH | PIXELFORMAT | MIPMAPCOUNT | LINEARSIZE
    caps = 0x1000
    if len(mip_data) > 1:
        caps |= 0x400008  # COMPLEX | MIPMAP
    header = bytearray()
    header.extend(b"DDS ")
    header.extend(struct.pack("<I", 124))
    header.extend(struct.pack("<I", flags))
    header.extend(struct.pack("<I", fetch.height))
    header.extend(struct.pack("<I", fetch.width))
    header.extend(struct.pack("<I", linear_size))
    header.extend(struct.pack("<I", 0))
    header.extend(struct.pack("<I", len(mip_data)))
    header.extend(b"\x00" * 44)
    header.extend(struct.pack("<I", 32))
    if fmt == 6:
        header.extend(struct.pack("<I", 0x41))  # RGB | ALPHAPIXELS
        header.extend(struct.pack("<I", 0))
        header.extend(struct.pack("<I", 32))
        header.extend(struct.pack("<I", 0x000000FF))
        header.extend(struct.pack("<I", 0x0000FF00))
        header.extend(struct.pack("<I", 0x00FF0000))
        header.extend(struct.pack("<I", 0xFF000000))
    else:
        header.extend(struct.pack("<I", 0x4))
        header.extend(fourcc)
        header.extend(struct.pack("<I", 0))
        header.extend(struct.pack("<I", 0))
        header.extend(struct.pack("<I", 0))
        header.extend(struct.pack("<I", 0))
        header.extend(struct.pack("<I", 0))
    header.extend(struct.pack("<I", caps))
    header.extend(struct.pack("<I", 0))
    header.extend(struct.pack("<I", 0))
    header.extend(struct.pack("<I", 0))
    header.extend(struct.pack("<I", 0))
    path.write_bytes(bytes(header) + b"".join(mip_data))


def load_texture(path: Path, out_dir: Path, write_images: bool,
                 write_all_mip_pngs: bool) -> tuple[TextureProof, list[bytes]]:
    data = path.read_bytes()
    objects = parse_arena_objects(data)
    resource = next((obj for obj in objects if obj.type_id in (0x00010031, 0x00010032, 0x00010033)), None)
    raster = next((obj for obj in objects if obj.type_id == 0x00020003), None)
    if resource is None or raster is None:
        raise NonTextureRx2("RX2 does not contain a base resource and raster metadata")
    if raster.size < 0x34:
        raise ValueError("raster object too small for Xenos fetch constant")

    dwords = [be32(data, raster.offset + 0x1C + i * 4) for i in range(6)]
    fetch = decode_fetch(dwords, base_page_is_present=resource.size > 0)
    layout = texture_layout(fetch)
    renderer_key = prove_renderer_key_normalization(fetch)
    renderer_view = prove_renderer_view(fetch)
    fmt = fetch.format_base
    _, _, bpp, _ = FORMAT_INFO[fmt]
    bytes_per_block = FORMAT_INFO[fmt][0] * FORMAT_INFO[fmt][1] * bpp // 8

    notes: list[str] = []
    if fetch.format_raw != fetch.format_base:
        notes.append(f"raw format {fetch.format_raw_name} is normalized to {fetch.format_base_name}")
    if fetch.mip_max_level_effective > 0 and not fetch.packed_mips:
        notes.append("mipped texture without packed tail")
    if fetch.packed_mips:
        notes.append(f"packed mip tail begins at level {layout['packed_level']}")
    if fetch.format_base == 49:
        notes.append("DXN normal map; D3D12 must preserve BC5 UNORM/SNORM views")

    base_data_offset = resource.offset + fetch.base_address_pages * 4096
    mip_data_offset = resource.offset + fetch.mip_address_pages * 4096
    resource_end = resource.offset + resource.size
    base_required = layout["base"]["level_data_extent_bytes"]
    mips_required = layout["mips_total_extent_bytes"]
    levels: list[LevelProof] = []
    linear_mips: list[bytes] = []
    status = "PASS"

    for level in range(0, fetch.mip_max_level_effective + 1):
        if level == 0:
            stored_level = 0
            level_layout = layout["base"]
            source_offset = base_data_offset
            if layout["packed_level"] == 0:
                packed_offset = get_packed_mip_offset(fetch.width, fetch.height, 1, fmt, level)[:3]
                is_packed_level = True
            else:
                packed_offset = (0, 0, 0)
                is_packed_level = False
            source_end_estimated = source_offset + level_layout["level_data_extent_bytes"]
            source_end_exact = exact_level_read_end(source_offset, level_layout, fetch, level, packed_offset)
        else:
            packed_level = layout["packed_level"]
            stored_level = min(level, packed_level) if packed_level is not None else level
            level_layout = layout["mips"][stored_level]
            source_offset = mip_data_offset + layout["mip_offsets"][stored_level]
            if packed_level is not None and level >= packed_level:
                packed_offset = get_packed_mip_offset(fetch.width, fetch.height, 1, fmt, level)[:3]
                is_packed_level = True
            else:
                packed_offset = (0, 0, 0)
                is_packed_level = False
            source_end_estimated = source_offset + level_layout["level_data_extent_bytes"]
            source_end_exact = exact_level_read_end(source_offset, level_layout, fetch, level, packed_offset)
        level_status = "PASS"
        if source_offset < resource.offset or source_end_exact > resource_end:
            level_status = "OVERRUN"
            status = "FAIL"
        output_w_blocks = align(max(fetch.width >> level, 1), FORMAT_INFO[fmt][0]) // FORMAT_INFO[fmt][0]
        output_h_blocks = align(max(fetch.height >> level, 1), FORMAT_INFO[fmt][1]) // FORMAT_INFO[fmt][1]
        levels.append(
            LevelProof(
                level=level,
                width=max(fetch.width >> level, 1),
                height=max(fetch.height >> level, 1),
                stored_level=stored_level,
                packed=is_packed_level,
                packed_offset_blocks=list(packed_offset),
                source_offset=source_offset,
                source_end_exact=source_end_exact,
                source_end_estimated=source_end_estimated,
                input_pitch_blocks=level_layout["row_pitch_blocks"],
                output_blocks=[output_w_blocks, output_h_blocks],
                status=level_status,
            )
        )
        linear_mips.append(
            linearize_level(data, source_offset, resource_end, level_layout, fetch, level, packed_offset)
        )

    runtime_upload = prove_runtime_upload_equivalent(data, resource, fetch, layout, linear_mips)
    if runtime_upload.status != "PASS":
        status = "FAIL"
    if renderer_key.status != "PASS":
        status = "FAIL"
    if renderer_view.status == "FAIL":
        status = "FAIL"

    if base_data_offset + base_required > resource_end:
        notes.append("RexGlue residency estimate for base extends past RX2 resource bytes")
    if fetch.mip_max_level_effective > 0 and mip_data_offset + mips_required > resource_end:
        notes.append("RexGlue residency estimate for mips extends past RX2 resource bytes")

    dds_path = None
    png_path = None
    mip_png_paths: list[str] = []
    if write_images:
        safe_name = path.with_suffix("").name
        dds = out_dir / f"{safe_name}.dds"
        png = out_dir / f"{safe_name}_mip0.png"
        write_dds(dds, fetch, linear_mips)
        rgba = decode_preview_rgba(linear_mips[0], fetch.width, fetch.height, fmt)
        write_png(png, fetch.width, fetch.height, rgba)
        dds_path = str(dds)
        png_path = str(png)
        mip_png_paths.append(str(png))
        if write_all_mip_pngs:
            for level, mip in enumerate(linear_mips[1:], start=1):
                mip_width = max(fetch.width >> level, 1)
                mip_height = max(fetch.height >> level, 1)
                mip_png = out_dir / f"{safe_name}_mip{level}.png"
                mip_rgba = decode_preview_rgba(mip, mip_width, mip_height, fmt)
                write_png(mip_png, mip_width, mip_height, mip_rgba)
                mip_png_paths.append(str(mip_png))

    proof = TextureProof(
        path=str(path),
        file_size=len(data),
        resource_offset=resource.offset,
        resource_size=resource.size,
        fetch=fetch,
        packed_level=layout["packed_level"],
        base_required_bytes=base_required,
        mips_required_bytes=mips_required,
        levels=levels,
        runtime_upload=runtime_upload,
        renderer_key=renderer_key,
        renderer_view=renderer_view,
        dds_path=dds_path,
        png_path=png_path,
        mip_png_paths=mip_png_paths,
        status=status,
        notes=notes,
    )
    return proof, linear_mips


def prove_texture(path: Path, out_dir: Path, write_images: bool,
                  write_all_mip_pngs: bool) -> TextureProof:
    proof, _ = load_texture(path, out_dir, write_images, write_all_mip_pngs)
    return proof


def decode_rgba_mips(fetch: FetchInfo, linear_mips: list[bytes]) -> list[tuple[int, int, bytes]]:
    rgba_mips: list[tuple[int, int, bytes]] = []
    for level, mip in enumerate(linear_mips):
        width = max(fetch.width >> level, 1)
        height = max(fetch.height >> level, 1)
        rgba_mips.append((width, height, decode_preview_rgba(mip, width, height, fetch.format_base)))
    return rgba_mips


def sample_rgba(rgba_mips: list[tuple[int, int, bytes]], lod: int, u: float,
                v: float) -> tuple[int, int, int, int]:
    lod = min(max(lod, 0), len(rgba_mips) - 1)
    width, height, rgba = rgba_mips[lod]
    x = min(max(int(u * width), 0), width - 1)
    y = min(max(int(v * height), 0), height - 1)
    offset = (y * width + x) * 4
    return tuple(rgba[offset : offset + 4])  # type: ignore[return-value]


def tint_has_saturated_green(tint_colors: list[list[int]]) -> bool:
    for r, g, b in tint_colors:
        if g >= 190 and g > r * 1.45 and g > b * 1.45:
            return True
    return False


def tint_has_saturated_purple(tint_colors: list[list[int]]) -> bool:
    for r, g, b in tint_colors:
        if r >= 120 and b >= 120 and g * 1.8 < min(r, b):
            return True
    return False


def analyze_material_composite_rgba(path: Path, size: int, lod: int,
                                    rgba: bytes, tint_colors: list[list[int]]) -> MaterialCompositeMetric:
    pixel_count = size * size
    neon_green = 0
    purple = 0
    transparent = 0
    near_black = 0
    for offset in range(0, len(rgba), 4):
        r, g, b, a = rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]
        if a < 16:
            transparent += 1
        if a >= 16 and g >= 190 and g > r * 1.45 and g > b * 1.45:
            neon_green += 1
        if a >= 16 and r >= 120 and b >= 120 and g * 1.8 < min(r, b):
            purple += 1
        if a >= 16 and r + g + b < 36:
            near_black += 1

    neon_green_ratio = neon_green / pixel_count
    purple_ratio = purple / pixel_count
    transparent_ratio = transparent / pixel_count
    near_black_ratio = near_black / pixel_count
    status = "PASS"
    notes: list[str] = []
    expected_green = tint_has_saturated_green(tint_colors)
    expected_purple = tint_has_saturated_purple(tint_colors)
    if neon_green_ratio > 0.005 and not expected_green:
        status = "FAIL"
        notes.append("neon green artifact ratio exceeds 0.5%")
    elif neon_green_ratio > 0.005:
        notes.append("saturated green is present in the selected tint palette")
    if purple_ratio > 0.005 and not expected_purple:
        status = "FAIL"
        notes.append("purple artifact ratio exceeds 0.5%")
    elif purple_ratio > 0.005:
        notes.append("saturated purple is present in the selected tint palette")
    if transparent_ratio > 0.25:
        notes.append("transparent coverage is high; this can be legitimate material cutout/UV space")
    if near_black_ratio > 0.25:
        notes.append("near-black coverage is high; inspect whether this is legitimate equipment color")

    return MaterialCompositeMetric(
        path=str(path),
        lod=lod,
        width=size,
        height=size,
        neon_green_ratio=neon_green_ratio,
        purple_ratio=purple_ratio,
        transparent_ratio=transparent_ratio,
        near_black_ratio=near_black_ratio,
        status=status,
        notes=notes,
    )


def write_material_composite(path: Path, size: int, lod: int,
                             layer_rgba_mips: dict[str, list[tuple[int, int, bytes]]],
                             tint_colors: list[list[int]]) -> MaterialCompositeMetric:
    if not tint_colors:
        tint_colors = [
            [31, 101, 226],
            [210, 38, 48],
            [34, 168, 82],
            [235, 188, 48],
        ]
    out = bytearray(size * size * 4)
    diffuse = layer_rgba_mips["diffuse"]
    normal = layer_rgba_mips["normal"]
    shine = layer_rgba_mips["shine"]
    templates = [
        mips for name, mips in sorted(layer_rgba_mips.items()) if name.startswith("template")
    ]

    for y in range(size):
      v = (y + 0.5) / size
      for x in range(size):
        u = (x + 0.5) / size
        dr, dg, db, da = sample_rgba(diffuse, lod, u, v)
        nr, ng, nb, _ = sample_rgba(normal, lod, u, v)
        sr, sg, sb, _ = sample_rgba(shine, lod, u, v)

        # Use the decoded normal preview's Z channel as a cheap lighting term.
        shade = 0.55 + 0.45 * (nb / 255.0)
        r = dr * shade
        g = dg * shade
        b = db * shade

        # Apply colored template masks only to expose alignment. This is not a
        # full NHL12 material shader; it is an offline proof that the layers can
        # be sampled together coherently at the same UV/mip.
        for template_index, template in enumerate(templates):
            tr, tg, tb, _ = sample_rgba(template, lod, u, v)
            mask = max(tr, tg, tb) / 255.0
            tint = tint_colors[template_index % len(tint_colors)]
            strength = 0.35 * mask
            r = r * (1.0 - strength) + tint[0] * strength
            g = g * (1.0 - strength) + tint[1] * strength
            b = b * (1.0 - strength) + tint[2] * strength

        shine_amount = (sr + sg + sb) / (3.0 * 255.0)
        r = min(255.0, r + 80.0 * shine_amount)
        g = min(255.0, g + 80.0 * shine_amount)
        b = min(255.0, b + 80.0 * shine_amount)

        dst = (y * size + x) * 4
        out[dst : dst + 4] = bytes((int(r), int(g), int(b), da))
    rgba = bytes(out)
    write_png(path, size, size, rgba)
    return analyze_material_composite_rgba(path, size, lod, rgba, tint_colors)


def material_layer_candidates(root: Path, directory: str, asset_id: str,
                              frontend: bool) -> dict[str, Path]:
    folder = root / directory
    material_prefix = f"{'fe' if frontend else ''}{directory}_{asset_id}"
    gameplay_prefix = f"{directory}_{asset_id}"
    candidates: dict[str, Path] = {
        "diffuse": folder / f"{material_prefix}_0_dm.rx2",
        "alpha": folder / f"{material_prefix}_am.rx2",
        "normal": folder / f"{gameplay_prefix}_nm.rx2",
        "shine": folder / f"{gameplay_prefix}_sm.rx2",
    }
    for index in range(4):
        template_path = folder / f"{material_prefix}_{index}_tm.rx2"
        if template_path.exists():
            candidates[f"template{index}"] = template_path
    return candidates


def discover_material_sets(root: Path, directories: list[str],
                           limit: int | None) -> list[str]:
    specs_by_directory: list[list[str]] = []
    for directory in directories:
        folder = root / directory
        if not folder.exists():
            continue
        directory_specs: list[str] = []
        for frontend in (False, True):
            prefix = f"{'fe' if frontend else ''}{directory}_"
            suffix = "_0_dm.rx2"
            for path in sorted(folder.glob(f"{prefix}*{suffix}")):
                name = path.name
                if not name.startswith(prefix) or not name.endswith(suffix):
                    continue
                asset_id = name[len(prefix) : -len(suffix)]
                candidates = material_layer_candidates(root, directory, asset_id, frontend)
                required = ("diffuse", "normal", "shine")
                has_required = all(candidates[name].exists() for name in required)
                has_template = any(
                    name.startswith("template") and candidate.exists()
                    for name, candidate in candidates.items()
                )
                if not has_required or not has_template:
                    continue
                spec = f"{directory}:{asset_id}{':fe' if frontend else ''}"
                directory_specs.append(spec)

        specs_by_directory.append(list(dict.fromkeys(directory_specs)))

    if limit is None:
        specs: list[str] = []
        for directory_specs in specs_by_directory:
            specs.extend(directory_specs)
        return list(dict.fromkeys(specs))

    selected: list[str] = []
    seen: set[str] = set()
    index = 0
    while len(selected) < limit:
        added_this_round = False
        for directory_specs in specs_by_directory:
            if index >= len(directory_specs):
                continue
            added_this_round = True
            spec = directory_specs[index]
            if spec in seen:
                continue
            seen.add(spec)
            selected.append(spec)
            if len(selected) >= limit:
                break
        if not added_this_round:
            break
        index += 1
    return selected


class EquipmentDbColors:
    def __init__(self, meta: dict, records: list[dict], bit_order: str):
        self.meta = meta
        self.records = records
        self.bit_order = bit_order


def load_equipment_db_colors(meta_path: Path, db_path: Path, bit_order: str) -> EquipmentDbColors:
    meta, records = decode_db_table_records(
        meta_path, db_path, "exhibitiongoalieequipment", bit_order
    )
    return EquipmentDbColors(meta, records, bit_order)


def nonzero_color(color: tuple[int, int, int]) -> bool:
    return any(component != 0 for component in color)


def extract_record_zone_colors(record: dict, equipment_kind: str) -> list[tuple[int, int, int]]:
    max_zone = 3 if equipment_kind == "stick" else 9
    colors: list[tuple[int, int, int]] = []
    for zone in range(1, max_zone + 1):
        keys = [
            f"{equipment_kind}zone{zone}color_r",
            f"{equipment_kind}zone{zone}color_g",
            f"{equipment_kind}zone{zone}color_b",
        ]
        if not all(key in record for key in keys):
            continue
        colors.append((record[keys[0]], record[keys[1]], record[keys[2]]))
    return colors


def parse_asset_equipment_id(asset_id: str) -> int | None:
    parts = asset_id.split("_")
    for part in reversed(parts):
        try:
            return int(part)
        except ValueError:
            continue
    return None


def select_material_tint_colors(directory: str, asset_id: str,
                                db_colors: EquipmentDbColors | None) -> MaterialColorSource:
    fallback_colors = [
        [31, 101, 226],
        [210, 38, 48],
        [34, 168, 82],
        [235, 188, 48],
    ]
    if db_colors is None:
        return MaterialColorSource(
            source="synthetic",
            table=None,
            bit_order=None,
            equipment_kind=None,
            equipment_id=None,
            record_count=0,
            matched_record_count=0,
            tint_colors=fallback_colors,
            notes=["DB colors were not requested; using synthetic alignment stress colors."],
        )

    equipment_id = parse_asset_equipment_id(asset_id)
    equipment_kind_by_directory = {
        "goaliepad": ["pads"],
        "blocker": ["blocker"],
        "trapper": ["trapper"],
        "glove": ["trapper", "blocker"],
        "goaliestick": ["stick"],
    }
    equipment_kinds = equipment_kind_by_directory.get(directory, [])

    notes: list[str] = []
    selected_kind: str | None = None
    matched_records: list[dict] = []
    if equipment_id is not None:
        for kind in equipment_kinds:
            records = [record for record in db_colors.records if record.get(kind) == equipment_id]
            if records:
                selected_kind = kind
                matched_records = records
                break

    if matched_records and selected_kind:
        color_counter: Counter[tuple[int, int, int]] = Counter()
        for record in matched_records:
            for color in extract_record_zone_colors(record, selected_kind):
                if nonzero_color(color):
                    color_counter[color] += 1
        tint_colors = [list(color) for color, _ in color_counter.most_common(8)]
        if tint_colors:
            return MaterialColorSource(
                source="db",
                table="exhibitiongoalieequipment",
                bit_order=db_colors.bit_order,
                equipment_kind=selected_kind,
                equipment_id=equipment_id,
                record_count=len(db_colors.records),
                matched_record_count=len(matched_records),
                tint_colors=tint_colors,
                notes=[
                    "Tint colors came from matching exhibition goalie equipment rows.",
                    "DB table parsing is bit-packed and mode-specific; keep bit_order in reports.",
                ],
            )
        notes.append("matching DB rows had no non-zero zone colors")
    else:
        notes.append("no DB rows matched this material asset id")

    global_counter: Counter[tuple[int, int, int]] = Counter()
    for kind in equipment_kinds or ["pads", "trapper", "blocker"]:
        for record in db_colors.records:
            for color in extract_record_zone_colors(record, kind):
                if nonzero_color(color):
                    global_counter[color] += 1
    tint_colors = [list(color) for color, _ in global_counter.most_common(8)] or fallback_colors
    return MaterialColorSource(
        source="db_global_fallback" if global_counter else "synthetic",
        table="exhibitiongoalieequipment" if global_counter else None,
        bit_order=db_colors.bit_order if global_counter else None,
        equipment_kind="+".join(equipment_kinds) if equipment_kinds else None,
        equipment_id=equipment_id,
        record_count=len(db_colors.records),
        matched_record_count=0,
        tint_colors=tint_colors,
        notes=notes
        + [
            "Using common non-zero DB zone colors because no exact material/equipment id match was usable."
        ],
    )


def prove_material_set(spec: str, root: Path, out_dir: Path, size: int,
                       db_colors: EquipmentDbColors | None = None) -> MaterialProof:
    parts = spec.split(":")
    if len(parts) < 2:
        raise ValueError("material set must be DIRECTORY:ID, for example goaliepad:0_11")
    directory = parts[0]
    asset_id = parts[1]
    frontend = len(parts) >= 3 and parts[2].lower() in ("fe", "frontend")
    prefix = f"{'fe' if frontend else ''}{directory}_{asset_id}"
    candidates = material_layer_candidates(root, directory, asset_id, frontend)
    color_source = select_material_tint_colors(directory, asset_id, db_colors)

    required = ("diffuse", "normal", "shine")
    notes: list[str] = []
    missing = [name for name in required if not candidates[name].exists()]
    if not any(name.startswith("template") for name in candidates):
        missing.append("template")
    if missing:
        return MaterialProof(
            spec=spec,
            directory=directory,
            asset_id=asset_id,
            prefix=prefix,
            status="FAIL",
            layers=[],
            composite_png_paths=[],
            composite_metrics=[],
            color_source=color_source,
            notes=[f"missing required layer(s): {', '.join(missing)}"],
        )

    layers: list[MaterialLayerProof] = []
    rgba_mips_by_layer: dict[str, list[tuple[int, int, bytes]]] = {}
    status = "PASS"
    for name, path in sorted(candidates.items()):
        if not path.exists():
            continue
        proof, linear_mips = load_texture(path, out_dir, False, False)
        if proof.status != "PASS":
            status = "FAIL"
        layers.append(
            MaterialLayerProof(
                name=name,
                path=str(path),
                format=proof.fetch.format_base_name,
                width=proof.fetch.width,
                height=proof.fetch.height,
                mip_count=proof.fetch.mip_max_level_effective + 1,
                packed_mips=proof.fetch.packed_mips,
                status=proof.status,
            )
        )
        rgba_mips_by_layer[name] = decode_rgba_mips(proof.fetch, linear_mips)

    for name in ("normal", "shine"):
        layer = next((layer for layer in layers if layer.name == name), None)
        if layer is None or layer.mip_count <= 1:
            status = "FAIL"
            notes.append(f"{name} layer does not expose a real mip chain")

    max_lod = max(len(mips) for mips in rgba_mips_by_layer.values()) - 1
    lods = sorted({0, min(1, max_lod), min(2, max_lod), max_lod})
    composite_paths: list[str] = []
    composite_metrics: list[MaterialCompositeMetric] = []
    for lod in lods:
        composite_path = out_dir / f"material_{prefix}_lod{lod}.png"
        metric = write_material_composite(
            composite_path, size, lod, rgba_mips_by_layer, color_source.tint_colors
        )
        if metric.status != "PASS":
            status = "FAIL"
        composite_paths.append(str(composite_path))
        composite_metrics.append(metric)

    notes.append(
        "Composite previews are a layer-alignment proof, not a full replacement for NHL12 shaders."
    )
    return MaterialProof(
        spec=spec,
        directory=directory,
        asset_id=asset_id,
        prefix=prefix,
        status=status,
        layers=layers,
        composite_png_paths=composite_paths,
        composite_metrics=composite_metrics,
        color_source=color_source,
        notes=notes,
    )


MATERIAL_CONTRACT_SUFFIXES = (
    ("normal", "_nm.rx2"),
    ("shine", "_sm.rx2"),
    ("alpha", "_am.rx2"),
    ("color", "_cm.rx2"),
    ("diffuse", "_dm.rx2"),
    ("template", "_tm.rx2"),
)

MATERIAL_CONTRACT_MIPPED_KINDS = {"normal", "shine", "alpha", "color"}
MATERIAL_CONTRACT_EXPECTED_FORMATS = {
    "normal": {"k_DXN", "k_DXT4_5"},
    "shine": {"k_DXT1"},
    "alpha": {"k_DXT1", "k_DXT3A", "k_DXT5A"},
    "color": {"k_DXT1", "k_8_8_8_8"},
    "diffuse": {"k_DXT1", "k_DXT2_3", "k_DXT4_5"},
    "template": {"k_DXT1"},
}
MATERIAL_CONTRACT_ROLES = {
    "color": "colormap: logos and colors",
    "shine": "shine map: shadows and light reflection",
    "normal": "normal map: wrinkles and shape",
    "alpha": "alpha/support map: cutout or extra material mask",
    "diffuse": "base diffuse/recolor mask",
    "template": "DB recolor template mask",
}


def material_contract_layer_kind(path: Path) -> str | None:
    name = path.name.lower()
    for layer_kind, suffix in MATERIAL_CONTRACT_SUFFIXES:
        if name.endswith(suffix):
            return layer_kind
    return None


def iter_material_contract_inputs(root: Path, directories: list[str],
                                  limit: int | None) -> list[Path]:
    files: list[Path] = []
    for directory in directories:
        folder = root / directory
        if not folder.exists():
            continue
        for path in sorted(folder.glob("*.rx2")):
            if material_contract_layer_kind(path):
                files.append(path)
    unique: list[Path] = []
    seen: set[Path] = set()
    for path in files:
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(path)
    return unique[:limit] if limit is not None else unique


def prove_material_contract_path(path: Path, root: Path, out_dir: Path) -> MaterialContractEntry:
    layer_kind = material_contract_layer_kind(path) or "unknown"
    notes: list[str] = []
    status = "PASS"

    data = path.read_bytes()
    objects = parse_arena_objects(data)
    resource = next((obj for obj in objects if obj.type_id in (0x00010031, 0x00010032, 0x00010033)), None)
    raster = next((obj for obj in objects if obj.type_id == 0x00020003), None)
    if resource is None or raster is None:
        raise NonTextureRx2("RX2 does not contain a base resource and raster metadata")
    if raster.size < 0x34:
        raise ValueError("raster object too small for Xenos fetch constant")

    dwords = [be32(data, raster.offset + 0x1C + i * 4) for i in range(6)]
    fetch = decode_fetch(dwords, base_page_is_present=resource.size > 0)
    layout = texture_layout(fetch)
    renderer_key = prove_renderer_key_normalization(fetch)
    renderer_view = prove_renderer_view(fetch)

    format_name = fetch.format_base_name
    mip_count = fetch.mip_max_level_effective + 1
    if fetch.format_raw != fetch.format_base:
        notes.append(f"raw format {fetch.format_raw_name} is normalized to {fetch.format_base_name}")
    if renderer_key.status != "PASS":
        status = "FAIL"
        notes.append(f"renderer-key proof status is {renderer_key.status}")
    if renderer_view.status == "FAIL":
        status = "FAIL"
        notes.append("renderer-view proof failed")

    base_data_offset = resource.offset + fetch.base_address_pages * 4096
    mip_data_offset = resource.offset + fetch.mip_address_pages * 4096
    resource_end = resource.offset + resource.size

    for level in range(0, fetch.mip_max_level_effective + 1):
        if level == 0:
            level_layout = layout["base"]
            source_offset = base_data_offset
            if layout["packed_level"] == 0:
                packed_offset = get_packed_mip_offset(fetch.width, fetch.height, 1, fetch.format_base, level)[:3]
            else:
                packed_offset = (0, 0, 0)
        else:
            packed_level = layout["packed_level"]
            stored_level = min(level, packed_level) if packed_level is not None else level
            level_layout = layout["mips"][stored_level]
            source_offset = mip_data_offset + layout["mip_offsets"][stored_level]
            if packed_level is not None and level >= packed_level:
                packed_offset = get_packed_mip_offset(fetch.width, fetch.height, 1, fetch.format_base, level)[:3]
            else:
                packed_offset = (0, 0, 0)
        source_end_exact = exact_level_read_end(source_offset, level_layout, fetch, level, packed_offset)
        if source_offset < resource.offset or source_end_exact > resource_end:
            status = "FAIL"
            notes.append(f"level {level} read extent exceeds RX2 resource bytes")
            break

    expected_formats = MATERIAL_CONTRACT_EXPECTED_FORMATS.get(layer_kind, set())
    if expected_formats and format_name not in expected_formats:
        notes.append(
            f"{layer_kind} format {format_name} is outside expected "
            f"{', '.join(sorted(expected_formats))}"
        )

    if layer_kind in MATERIAL_CONTRACT_MIPPED_KINDS:
        if mip_count <= 1:
            status = "FAIL"
            notes.append(f"{layer_kind} map does not expose a real mip chain")
        if not fetch.packed_mips:
            status = "FAIL"
            notes.append(f"{layer_kind} map is not using packed mip storage")
        if not renderer_key.mipped_material_mips_preserved:
            status = "FAIL"
            notes.append(f"{layer_kind} map would not preserve mips in the NHL12 renderer key")
    elif mip_count <= 1:
        notes.append(f"{layer_kind} map is base-only; this is expected for DB recolor masks")

    try:
        directory = str(path.relative_to(root).parts[0])
    except ValueError:
        directory = path.parent.name

    return MaterialContractEntry(
        path=str(path),
        directory=directory,
        layer_kind=layer_kind,
        role=MATERIAL_CONTRACT_ROLES.get(layer_kind, "unknown material layer"),
        format=format_name,
        width=fetch.width,
        height=fetch.height,
        mip_count=mip_count,
        packed_mips=fetch.packed_mips,
        renderer_key_status=renderer_key.status,
        renderer_view_status=renderer_view.status,
        status=status,
        notes=notes,
    )


def prove_material_contract(root: Path, directories: list[str], out_dir: Path,
                            limit: int | None) -> MaterialContractReport:
    inputs = iter_material_contract_inputs(root, directories, limit)
    proofs: list[MaterialContractEntry] = []
    by_layer_kind: dict[str, Counter[str]] = {}
    by_format: Counter[str] = Counter()
    failed = 0
    warnings = 0
    for path in inputs:
        try:
            entry = prove_material_contract_path(path, root, out_dir)
        except NonTextureRx2 as exc:
            entry = MaterialContractEntry(
                path=str(path),
                directory=path.parent.name,
                layer_kind=material_contract_layer_kind(path) or "unknown",
                role=MATERIAL_CONTRACT_ROLES.get(
                    material_contract_layer_kind(path) or "unknown",
                    "unknown material layer",
                ),
                format="unknown",
                width=0,
                height=0,
                mip_count=0,
                packed_mips=False,
                renderer_key_status="SKIP",
                renderer_view_status="SKIP",
                status="SKIP",
                notes=[str(exc)],
            )
        proofs.append(entry)
        by_layer_kind.setdefault(entry.layer_kind, Counter())[entry.status] += 1
        by_format[entry.format] += 1
        if entry.status == "FAIL":
            failed += 1
        if any("outside expected" in note for note in entry.notes):
            warnings += 1

    return MaterialContractReport(
        root=str(root),
        directories=directories,
        count=len(proofs),
        failed=failed,
        warnings=warnings,
        by_layer_kind={kind: dict(counter) for kind, counter in sorted(by_layer_kind.items())},
        by_format=dict(sorted(by_format.items())),
        proofs=proofs,
    )


def summarize_texture_corpus(root: Path, out_dir: Path, patterns: list[str],
                             limit: int | None, fail_non_texture: bool = False) -> dict:
    inputs = iter_inputs(root, patterns, limit)
    summary = {
        "patterns": patterns,
        "limit": limit,
        "count": 0,
        "passed": 0,
        "failed": 0,
        "skipped": 0,
        "renderer_key_failed": 0,
        "renderer_view_failed": 0,
        "renderer_mipped_materials": 0,
        "renderer_base_only_fixes": 0,
        "renderer_tiny_base_preserved": 0,
        "formats": {},
        "examples": [],
    }
    formats: Counter[str] = Counter()
    examples: list[dict] = []
    for path in inputs:
        summary["count"] += 1
        try:
            proof = prove_texture(path, out_dir, False, False)
            formats[proof.fetch.format_base_name] += 1
            if proof.status == "PASS":
                summary["passed"] += 1
            else:
                summary["failed"] += 1
            if proof.renderer_key.status != "PASS":
                summary["renderer_key_failed"] += 1
            if proof.renderer_view.status == "FAIL":
                summary["renderer_view_failed"] += 1
            if proof.renderer_key.mipped_material_mips_preserved and proof.renderer_key.mip_count > 1:
                summary["renderer_mipped_materials"] += 1
            if proof.renderer_key.base_only_packed_fix_applied:
                summary["renderer_base_only_fixes"] += 1
            if proof.renderer_key.tiny_base_packed_preserved:
                summary["renderer_tiny_base_preserved"] += 1
            if proof.status != "PASS" or proof.renderer_key.status != "PASS" or proof.renderer_view.status == "FAIL":
                if len(examples) < 20:
                    examples.append(
                        {
                            "path": str(path),
                            "status": proof.status,
                            "format": proof.fetch.format_base_name,
                            "renderer_key_status": proof.renderer_key.status,
                            "renderer_view_status": proof.renderer_view.status,
                            "notes": proof.notes[:8],
                        }
                    )
        except NonTextureRx2 as exc:
            summary["skipped"] += 1
            if fail_non_texture:
                summary["failed"] += 1
            if fail_non_texture and len(examples) < 20:
                examples.append({"path": str(path), "status": "SKIP", "reason": str(exc)})
        except Exception as exc:
            summary["failed"] += 1
            if len(examples) < 20:
                examples.append({"path": str(path), "status": "ERROR", "error": str(exc)})

    summary["formats"] = dict(sorted(formats.items()))
    summary["examples"] = examples
    return summary


def default_equipment_db_colors(root: Path) -> EquipmentDbColors | None:
    db_dir = root.parent / "db"
    db_file = db_dir / "nhlng.db"
    db_meta = db_dir / "nhlng-meta.xml"
    if not db_file.exists() or not db_meta.exists():
        return None
    return load_equipment_db_colors(db_meta, db_file, "msb")


def summarize_material_stack_composites(root: Path, out_dir: Path, directories: list[str],
                                        limit: int, size: int,
                                        db_colors: EquipmentDbColors | None) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    specs = discover_material_sets(root, directories, limit)
    summary = {
        "directories": directories,
        "limit": limit,
        "size": size,
        "count": 0,
        "passed": 0,
        "failed": 0,
        "color_sources": {},
        "exact_db_color_matches": 0,
        "layer_formats": {},
        "layer_mip_counts": {},
        "max_neon_green_ratio": 0.0,
        "max_purple_ratio": 0.0,
        "max_transparent_ratio": 0.0,
        "max_near_black_ratio": 0.0,
        "examples": [],
    }
    color_sources: Counter[str] = Counter()
    layer_formats: dict[str, Counter[str]] = {}
    layer_mip_counts: dict[str, Counter[int]] = {}
    examples: list[dict] = []

    for spec in specs:
        summary["count"] += 1
        try:
            proof = prove_material_set(spec, root, out_dir, size, db_colors)
            if proof.status == "PASS":
                summary["passed"] += 1
            else:
                summary["failed"] += 1
            if proof.color_source:
                color_sources[proof.color_source.source] += 1
                if (
                    proof.color_source.source == "db"
                    and proof.color_source.matched_record_count > 0
                ):
                    summary["exact_db_color_matches"] += 1
            for layer in proof.layers:
                layer_formats.setdefault(layer.name, Counter())[layer.format] += 1
                layer_mip_counts.setdefault(layer.name, Counter())[layer.mip_count] += 1
            for metric in proof.composite_metrics:
                summary["max_neon_green_ratio"] = max(
                    summary["max_neon_green_ratio"], metric.neon_green_ratio
                )
                summary["max_purple_ratio"] = max(
                    summary["max_purple_ratio"], metric.purple_ratio
                )
                summary["max_transparent_ratio"] = max(
                    summary["max_transparent_ratio"], metric.transparent_ratio
                )
                summary["max_near_black_ratio"] = max(
                    summary["max_near_black_ratio"], metric.near_black_ratio
                )
            if proof.status != "PASS" or len(examples) < 8:
                examples.append(
                    {
                        "spec": proof.spec,
                        "status": proof.status,
                        "color_source": asdict(proof.color_source) if proof.color_source else None,
                        "layers": [asdict(layer) for layer in proof.layers],
                        "metrics": [asdict(metric) for metric in proof.composite_metrics],
                        "notes": proof.notes[:8],
                    }
                )
        except Exception as exc:
            summary["failed"] += 1
            if len(examples) < 20:
                examples.append({"spec": spec, "status": "ERROR", "error": str(exc)})

    summary["color_sources"] = dict(sorted(color_sources.items()))
    summary["layer_formats"] = {
        name: dict(counter) for name, counter in sorted(layer_formats.items())
    }
    summary["layer_mip_counts"] = {
        name: {str(mip): count for mip, count in sorted(counter.items())}
        for name, counter in sorted(layer_mip_counts.items())
    }
    summary["examples"] = examples
    return summary


def find_source_root(asset_root: Path) -> Path:
    candidates = [Path.cwd()]
    current = asset_root
    for _ in range(5):
        candidates.append(current)
        current = current.parent
    for candidate in candidates:
        if (
            (candidate / "app/src/nhl12_app.h").exists()
            and (candidate / "RexGlue/src/graphics/d3d12/texture_cache.cpp").exists()
        ):
            return candidate
    return Path.cwd()


def source_contract_check(checks: list[dict], source_root: Path, relative_path: str,
                          name: str, passed: bool, detail: str) -> None:
    checks.append(
        {
            "name": name,
            "path": str(source_root / relative_path),
            "status": "PASS" if passed else "FAIL",
            "detail": detail,
        }
    )


def audit_nhl12_renderer_source_contract(asset_root: Path) -> dict:
    source_root = find_source_root(asset_root)
    checks: list[dict] = []

    def read(relative_path: str) -> str:
        path = source_root / relative_path
        if not path.exists():
            source_contract_check(
                checks, source_root, relative_path, f"{relative_path} exists", False,
                "required source file is missing",
            )
            return ""
        return path.read_text(encoding="utf-8", errors="replace")

    app_source = read("app/src/nhl12_app.h")
    flags_source = read("RexGlue/src/graphics/flags.cpp")
    d3d12_texture_source = read("RexGlue/src/graphics/d3d12/texture_cache.cpp")
    generic_texture_source = read("RexGlue/src/graphics/pipeline/texture/cache.cpp")
    dxbc_fetch_source = read("RexGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp")

    app_flags = {
        "nhl_force_stacked_texture3d": "true",
        "nhl_force_native_draw_resolution": "true",
        "nhl_fix_cube_reflection_fetches": "true",
        "nhl_zero_cube_reflection_fetches": "false",
        "nhl_rebase_mip_min_textures": "true",
        "nhl_fix_base_only_packed_bc_textures": "true",
        "nhl_preserve_equipment_mips_without_forced_aniso": "true",
        "resolution_scale": "1",
        "draw_resolution_scale_x": "1",
        "draw_resolution_scale_y": "1",
        "draw_resolution_scaled_texture_offsets": "false",
        "native_2x_msaa": "true",
        "vsync": "false",
        "gpu_swap_fps_limit": "60",
    }
    for flag, value in app_flags.items():
        token = f'rex::cvar::SetFlagByName("{flag}", "{value}")'
        source_contract_check(
            checks, source_root, "app/src/nhl12_app.h", f"app sets {flag}={value}",
            token in app_source, token,
        )

    cvar_defaults = {
        "nhl_force_stacked_texture3d": "true",
        "nhl_force_native_draw_resolution": "true",
        "nhl_zero_cube_reflection_fetches": "false",
        "nhl_fix_cube_reflection_fetches": "true",
        "nhl_rebase_mip_min_textures": "true",
        "nhl_fix_base_only_packed_bc_textures": "true",
        "nhl_preserve_equipment_mips_without_forced_aniso": "true",
        "native_2x_msaa": "true",
    }
    for flag, value in cvar_defaults.items():
        token = f"REXCVAR_DEFINE_BOOL({flag}, {value}"
        source_contract_check(
            checks, source_root, "RexGlue/src/graphics/flags.cpp",
            f"RexGlue default {flag}={value}", token in flags_source, token,
        )

    required_material_formats = [
        "k_8_8_8_8",
        "k_DXT1",
        "k_DXT2_3",
        "k_DXT4_5",
        "k_DXN",
        "k_8_8_8_8_AS_16_16_16_16",
        "k_DXT1_AS_16_16_16_16",
        "k_DXT2_3_AS_16_16_16_16",
        "k_DXT4_5_AS_16_16_16_16",
        "k_DXT3A",
        "k_DXT5A",
    ]
    for fmt in required_material_formats:
        token = f"xenos::TextureFormat::{fmt}"
        source_contract_check(
            checks, source_root, "RexGlue/src/graphics/d3d12/texture_cache.cpp",
            f"D3D12 protects mipped material format {fmt}", token in d3d12_texture_source, token,
        )

    source_contract_check(
        checks, source_root, "RexGlue/src/graphics/d3d12/texture_cache.cpp",
        "D3D12 material sampler guard is 2D-only",
        "sampler_key.dimension == xenos::DataDimension::k2DOrStacked" in d3d12_texture_source
        and "IsNHL12MippedMaterialFormat(sampler_key.format)" in d3d12_texture_source,
        "sampler guard must exclude cubemaps and use IsNHL12MippedMaterialFormat",
    )
    source_contract_check(
        checks, source_root, "RexGlue/src/graphics/d3d12/texture_cache.cpp",
        "DXN uses typeless BC5 with UNORM/SNORM views",
        "DXGI_FORMAT_BC5_TYPELESS" in d3d12_texture_source
        and "DXGI_FORMAT_BC5_UNORM" in d3d12_texture_source
        and "DXGI_FORMAT_BC5_SNORM" in d3d12_texture_source,
        "DXN normal maps need BC5 typeless resource plus UNORM/SNORM SRV options",
    )
    source_contract_check(
        checks, source_root, "RexGlue/src/graphics/pipeline/texture/cache.cpp",
        "NHL12 forces native 1x draw resolution",
        "REXCVAR_GET(nhl_force_native_draw_resolution)" in generic_texture_source
        and "x_out = 1" in generic_texture_source
        and "y_out = 1" in generic_texture_source,
        "draw-resolution scaling must not shift NHL12 equipment atlases",
    )
    source_contract_check(
        checks, source_root, "RexGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp",
        "explicit LOD fetches read NHL12 rebase offsets",
        "texture_rebased_mip_min_levels" in dxbc_fetch_source
        and "rebased_mip_min_level_src" in dxbc_fetch_source,
        "explicit-LOD shaders must see texture rebases",
    )
    source_contract_check(
        checks, source_root, "RexGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp",
        "cube explicit LODs are excluded from NHL12 rebase normalization",
        "instr.dimension != xenos::FetchOpDimension::kCube" in dxbc_fetch_source
        and "normalize_rebased_texture_lod" in dxbc_fetch_source,
        "cube reflection fetches must not be LOD-rebased",
    )

    failed = sum(1 for check in checks if check["status"] != "PASS")
    return {
        "source_root": str(source_root),
        "status": "PASS" if failed == 0 else "FAIL",
        "failed": failed,
        "count": len(checks),
        "checks": checks,
    }


def run_nhl12_regression_suite(root: Path, out_dir: Path, fast: bool = False) -> dict:
    suite_out = out_dir
    suite_out.mkdir(parents=True, exist_ok=True)

    source_contract = audit_nhl12_renderer_source_contract(root)
    self_test = run_renderer_key_self_tests()
    material_contract = prove_material_contract(
        root,
        ["goaliepad", "glove", "blocker", "trapper", "jersey"],
        suite_out / "material_contract",
        None,
    )
    db_colors = default_equipment_db_colors(root)
    material_stack_composites = summarize_material_stack_composites(
        root,
        suite_out / "material_stack_composites",
        ["goaliepad", "glove", "blocker", "trapper"],
        8 if fast else 40,
        64,
        db_colors,
    )

    corpuses: dict[str, dict] = {}
    if not fast:
        corpuses["goalie_equipment_upload"] = summarize_texture_corpus(
            root,
            suite_out / "goalie_equipment_upload",
            [
                "goaliepad/*.rx2",
                "glove/*.rx2",
                "blocker/*.rx2",
                "trapper/*.rx2",
            ],
            5000,
        )
    else:
        corpuses["goalie_equipment_upload_sample"] = summarize_texture_corpus(
            root,
            suite_out / "goalie_equipment_upload_sample",
            [
                "goaliepad/*_nm.rx2",
                "goaliepad/*_sm.rx2",
                "glove/*_nm.rx2",
                "glove/*_sm.rx2",
                "blocker/*_nm.rx2",
                "blocker/*_sm.rx2",
                "trapper/*_nm.rx2",
                "trapper/*_sm.rx2",
            ],
            80,
        )

    corpuses["jersey_texlib_upload_sample"] = summarize_texture_corpus(
        root,
        suite_out / "jersey_texlib_upload_sample",
        [
            "jersey/texlib*.rx2",
            "jersey/jersey*.rx2",
            "czjersey/*.rx2",
        ],
        300,
    )
    corpuses["jersey_colormap_upload_sample"] = summarize_texture_corpus(
        root,
        suite_out / "jersey_colormap_upload_sample",
        ["jersey/name_*_cm.rx2"],
        300,
    )

    failed = 0
    if source_contract["failed"]:
        failed += 1
    if self_test["failed"]:
        failed += 1
    if material_contract.failed:
        failed += 1
    if material_stack_composites["failed"]:
        failed += 1
    for corpus in corpuses.values():
        if corpus["failed"] or corpus["renderer_key_failed"] or corpus["renderer_view_failed"]:
            failed += 1

    return {
        "status": "PASS" if failed == 0 else "FAIL",
        "fast": fast,
        "failed_sections": failed,
        "source_contract": source_contract,
        "self_test": self_test,
        "material_contract": asdict(material_contract),
        "material_stack_composites": material_stack_composites,
        "texture_corpuses": corpuses,
        "notes": [
            "This suite is an offline renderer contract gate. It does not replace live normal-play visual QA.",
            "Cubemap reflection behavior must still be validated separately when cubemap code changes.",
        ],
    }


def iter_inputs(root: Path, patterns: list[str], limit: int | None) -> list[Path]:
    files: list[Path] = []
    for pattern in patterns:
        matches = [Path(p) for p in glob.glob(str(root / pattern), recursive=True)]
        files.extend(sorted(matches))
    unique = []
    seen = set()
    for path in files:
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        unique.append(path)
    return unique[:limit] if limit is not None else unique


LOG_LINE_RE = re.compile(r"\[(NHL-(?:TEX|SAMPLER))\]\s+(.*)$")
BC_FORMAT_NAMES = {
    "k_DXT1",
    "k_DXT2_3",
    "k_DXT4_5",
    "k_DXN",
    "k_DXT1_AS_16_16_16_16",
    "k_DXT2_3_AS_16_16_16_16",
    "k_DXT4_5_AS_16_16_16_16",
    "k_DXT3A",
    "k_DXT5A",
}
NHL_MIPPED_MATERIAL_FORMAT_NAMES = BC_FORMAT_NAMES | {
    "k_8_8_8_8",
    "k_8_8_8_8_AS_16_16_16_16",
}


def parse_bool(value: object) -> bool:
    if isinstance(value, bool):
        return value
    text = str(value).strip().lower()
    return text in ("1", "true", "yes")


def parse_int(value: object, default: int = 0) -> int:
    text = str(value).strip()
    try:
        return int(text, 0)
    except ValueError:
        if re.fullmatch(r"[0-9A-Fa-f]+", text) and re.search(r"[A-Fa-f]", text):
            return int(text, 16)
        return default


def parse_size(value: object) -> tuple[int, int, int]:
    parts = str(value).split("x")
    values = [parse_int(part, 0) for part in parts]
    while len(values) < 3:
        values.append(1)
    return values[0], values[1], values[2]


def parse_log_payload(payload: str) -> dict:
    entry: dict[str, object] = {}
    for token in payload.split():
        if "=" not in token:
            entry[token] = True
            continue
        key, value = token.split("=", 1)
        if value.endswith(","):
            value = value[:-1]
        entry[key] = value
    if "fetch" in entry:
        entry["fetch"] = parse_int(entry["fetch"])
    if "size" in entry:
        width, height, depth = parse_size(entry["size"])
        entry["width"] = width
        entry["height"] = height
        entry["depth"] = depth
    for key in (
        "desc",
        "pitch",
        "base",
        "mip",
        "mip_min",
        "mip_max",
        "rebased",
        "signs",
        "host_swizzle",
        "dxgi_resource",
        "dxgi_unorm",
        "dxgi_snorm",
        "raw_mag",
        "raw_min",
        "raw_mip",
        "raw_aniso",
        "final_aniso",
        "anisotropic_override",
    ):
        if key in entry:
            entry[key] = parse_int(entry[key])
    for key in (
        "valid",
        "compatible",
        "special_view",
        "null",
        "shader_signed",
        "has_mips",
        "tiled",
        "packed",
        "mip_base_map",
        "mag_linear",
        "min_linear",
        "mip_linear",
        "preserved_nhl_mips",
        "decompressed",
        "signed_separate",
    ):
        if key in entry:
            entry[key] = parse_bool(entry[key])
    if "fetch_dw" in entry:
        entry["fetch_dw"] = [
            int(part.strip(), 16) for part in str(entry["fetch_dw"]).split(",") if part.strip()
        ]
    return entry


def fetch_info_from_log_entry(entry: dict) -> FetchInfo | None:
    dwords = entry.get("fetch_dw")
    if not isinstance(dwords, list) or len(dwords) != 6:
        return None
    return decode_fetch([parse_int(value) for value in dwords], parse_int(entry.get("base", 0)) != 0)


def null_descriptor_binding_is_sampled(entry: dict) -> bool:
    # The translated shader declares both unsigned and signed SRV slots, but it
    # samples only the slot required by the runtime-swizzled TextureSign values.
    # A null signed slot is harmless when no fetched component is signed.
    if "shader_signed" not in entry or "signs" not in entry:
        return True
    packed_signs = parse_int(entry.get("signs", 0))
    if parse_bool(entry.get("shader_signed", False)):
        return packed_signs_has_signed(packed_signs)
    return packed_signs_has_not_signed(packed_signs)


def normalize_dimension_name(value: object) -> str:
    return re.sub(r"[^A-Za-z0-9]", "", str(value)).upper()


def catalog_signature_from_fetch(proof: dict, strict: bool) -> tuple:
    fetch = proof["fetch"]
    renderer_key = proof.get("renderer_key") or {}
    mip_min = parse_int(fetch.get("mip_min_level_effective", 0))
    mip_max = parse_int(fetch.get("mip_max_level_effective", 0))
    mip_count = max(1, mip_max - mip_min + 1)
    packed = renderer_key.get("normalized_packed_mips", fetch.get("packed_mips", False))
    common = (
        str(fetch.get("format_base_name", "")),
        parse_int(fetch.get("width", 0)),
        parse_int(fetch.get("height", 0)),
        parse_int(fetch.get("depth_or_array_size", 1)),
        mip_count,
    )
    if not strict:
        return common
    return (
        normalize_dimension_name(fetch.get("dimension_name", "")),
        *common,
        parse_bool(packed),
        parse_bool(fetch.get("tiled", False)),
        parse_int(fetch.get("pitch_texels_div_32", 0)) * 32,
    )


def catalog_signature_from_log_entry(entry: dict, strict: bool) -> tuple:
    mip_min = parse_int(entry.get("mip_min", 0))
    mip_max = parse_int(entry.get("mip_max", 0))
    mip_count = max(1, mip_max - mip_min + 1)
    common = (
        str(entry.get("fmt", "")),
        parse_int(entry.get("width", 0)),
        parse_int(entry.get("height", 0)),
        parse_int(entry.get("depth", 1)),
        mip_count,
    )
    if not strict:
        return common
    return (
        normalize_dimension_name(entry.get("resource_dim", "")),
        *common,
        parse_bool(entry.get("packed", False)),
        parse_bool(entry.get("tiled", False)),
        parse_int(entry.get("pitch", 0)),
    )


def catalog_fetch_shape_signature(fetch: dict) -> tuple:
    return (
        parse_int(fetch.get("type", 0)),
        parse_int(fetch.get("format_raw", 0)),
        parse_int(fetch.get("format_base", 0)),
        parse_int(fetch.get("endianness", 0)),
        parse_int(fetch.get("pitch_texels_div_32", 0)),
        parse_bool(fetch.get("tiled", False)),
        parse_bool(fetch.get("stacked", False)),
        parse_int(fetch.get("width", 0)),
        parse_int(fetch.get("height", 0)),
        parse_int(fetch.get("depth_or_array_size", 1)),
        parse_int(fetch.get("dimension", 0)),
        parse_bool(fetch.get("packed_mips", False)),
        parse_int(fetch.get("mip_min_level_fetch", 0)),
        parse_int(fetch.get("mip_max_level_fetch", 0)),
        parse_int(fetch.get("swizzle", 0)),
        tuple(parse_int(value) for value in fetch.get("signs", [])),
    )


def runtime_fetch_shape_signature(entry: dict) -> tuple | None:
    fetch_dwords = entry.get("fetch_dw")
    if not isinstance(fetch_dwords, list) or len(fetch_dwords) != 6:
        return None
    try:
        fetch = decode_fetch([parse_int(value) for value in fetch_dwords], base_page_is_present=True)
    except Exception:
        return None
    return (
        fetch.type,
        fetch.format_raw,
        fetch.format_base,
        fetch.endianness,
        fetch.pitch_texels_div_32,
        fetch.tiled,
        fetch.stacked,
        fetch.width,
        fetch.height,
        fetch.depth_or_array_size,
        fetch.dimension,
        fetch.packed_mips,
        fetch.mip_min_level_fetch,
        fetch.mip_max_level_fetch,
        fetch.swizzle,
        tuple(fetch.signs),
    )


def load_catalog_reports(report_paths: list[Path]) -> dict:
    catalog = {
        "reports": [str(path) for path in report_paths],
        "entries": 0,
        "strict": {},
        "loose": {},
        "shape": {},
    }
    for report_path in report_paths:
        report = json.loads(report_path.read_text(encoding="utf-8"))
        for proof in report.get("proofs", []):
            if proof.get("status") != "PASS" or "fetch" not in proof:
                continue
            fetch = proof["fetch"]
            renderer_key = proof.get("renderer_key") or {}
            entry = {
                "path": proof.get("path"),
                "format": fetch.get("format_base_name"),
                "width": fetch.get("width"),
                "height": fetch.get("height"),
                "depth": fetch.get("depth_or_array_size"),
                "mip_count": parse_int(fetch.get("mip_max_level_effective", 0))
                - parse_int(fetch.get("mip_min_level_effective", 0))
                + 1,
                "packed_mips": renderer_key.get(
                    "normalized_packed_mips", fetch.get("packed_mips", False)
                ),
                "raw_packed_mips": fetch.get("packed_mips", False),
                "tiled": fetch.get("tiled", False),
                "pitch": parse_int(fetch.get("pitch_texels_div_32", 0)) * 32,
                "renderer_key_status": renderer_key.get("status"),
                "mipped_material_mips_preserved": renderer_key.get(
                    "mipped_material_mips_preserved", False
                ),
            }
            strict_sig = catalog_signature_from_fetch(proof, strict=True)
            loose_sig = catalog_signature_from_fetch(proof, strict=False)
            shape_sig = catalog_fetch_shape_signature(fetch)
            catalog["strict"].setdefault(strict_sig, []).append(entry)
            catalog["loose"].setdefault(loose_sig, []).append(entry)
            catalog["shape"].setdefault(shape_sig, []).append(entry)
            catalog["entries"] += 1
    return catalog


def summarize_catalog_match(entry: dict, strict_matches: list[dict],
                            loose_matches: list[dict],
                            shape_matches: list[dict]) -> dict:
    if shape_matches and strict_matches:
        match_type = "strict+shape"
    elif shape_matches:
        match_type = "shape"
    elif strict_matches:
        match_type = "strict"
    elif loose_matches:
        match_type = "loose"
    else:
        match_type = "none"
    matches = shape_matches or strict_matches or loose_matches
    return {
        "fetch": entry.get("fetch"),
        "fmt": entry.get("fmt"),
        "size": f"{entry.get('width', 0)}x{entry.get('height', 0)}x{entry.get('depth', 1)}",
        "mip_min": entry.get("mip_min"),
        "mip_max": entry.get("mip_max"),
        "packed": entry.get("packed"),
        "tiled": entry.get("tiled"),
        "pitch": entry.get("pitch"),
        "match_type": match_type,
        "strict_matches": len(strict_matches),
        "loose_matches": len(loose_matches),
        "fetch_shape_matches": len(shape_matches),
        "sample_paths": [match.get("path") for match in matches[:6]],
    }


def analyze_nhl_log(path: Path, catalog_report_paths: list[Path] | None = None) -> LogAnalysis:
    texture_entries: list[dict] = []
    sampler_entries: list[dict] = []
    issues: list[LogAnalysisIssue] = []
    notes: list[str] = []
    catalog_report_paths = catalog_report_paths or []
    catalog = load_catalog_reports(catalog_report_paths) if catalog_report_paths else None
    catalog_strict_matches = 0
    catalog_loose_matches = 0
    catalog_fetch_shape_matches = 0
    catalog_unmatched_texture_entries = 0
    catalog_match_summaries: list[dict] = []
    unused_null_descriptor_entries = 0
    empty_null_descriptor_entries = 0

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LOG_LINE_RE.search(line)
        if not match:
            continue
        kind, payload = match.groups()
        entry = parse_log_payload(payload)
        entry["kind"] = kind
        if kind == "NHL-TEX":
            texture_entries.append(entry)
        else:
            sampler_entries.append(entry)

    def add_issue(severity: str, entry: dict, message: str) -> None:
        fetch = entry.get("fetch")
        issues.append(
            LogAnalysisIssue(
                severity=severity,
                fetch=fetch if isinstance(fetch, int) else None,
                message=message,
                entry=entry,
            )
        )

    for entry in texture_entries:
        if parse_bool(entry.get("null", False)):
            if parse_bool(entry.get("no_valid_binding", False)) and "fmt" not in entry:
                empty_null_descriptor_entries += 1
            elif null_descriptor_binding_is_sampled(entry):
                add_issue("error", entry, "sampled texture binding resolved to a null descriptor")
            else:
                unused_null_descriptor_entries += 1
        if "compatible" in entry and not parse_bool(entry["compatible"]):
            add_issue("error", entry, "shader/resource dimensions are incompatible")
        fmt = str(entry.get("fmt", ""))
        if fmt in NHL_MIPPED_MATERIAL_FORMAT_NAMES:
            expected_key = None
            fetch_info = fetch_info_from_log_entry(entry)
            if fetch_info is not None:
                expected_key = prove_renderer_key_normalization(fetch_info)
            mip_min = parse_int(entry.get("mip_min", 0))
            mip_max = parse_int(entry.get("mip_max", 0))
            packed = parse_bool(entry.get("packed", False))
            width = parse_int(entry.get("width", 0))
            height = parse_int(entry.get("height", 0))
            if expected_key is not None and packed != expected_key.normalized_packed_mips:
                add_issue(
                    "error",
                    entry,
                    "runtime binding packed state differs from current NHL12 renderer-key normalization",
                )
            if mip_max > mip_min and not packed:
                add_issue("warning", entry, "mipped NHL12 material fetch is not using packed mips")
            if (
                expected_key is None
                and fmt in BC_FORMAT_NAMES
                and mip_max == 0
                and packed
                and min(width, height) > 16
            ):
                add_issue(
                    "error",
                    entry,
                    "non-tiny base-only BC fetch still has packed mips after normalization",
                )
            if mip_min > 0 and parse_int(entry.get("rebased", 0)) == 0 and entry.get("resource_dim") != "cube":
                add_issue("warning", entry, "non-cubemap mip_min fetch was not rebased")

        if catalog and not parse_bool(entry.get("null", False)) and fmt:
            strict_sig = catalog_signature_from_log_entry(entry, strict=True)
            loose_sig = catalog_signature_from_log_entry(entry, strict=False)
            shape_sig = runtime_fetch_shape_signature(entry)
            strict_matches = catalog["strict"].get(strict_sig, [])
            loose_matches = catalog["loose"].get(loose_sig, [])
            shape_matches = catalog["shape"].get(shape_sig, []) if shape_sig else []
            if strict_matches:
                catalog_strict_matches += 1
            elif loose_matches:
                catalog_loose_matches += 1
            if shape_matches:
                catalog_fetch_shape_matches += 1
            if not strict_matches and not loose_matches and not shape_matches:
                catalog_unmatched_texture_entries += 1
            if len(catalog_match_summaries) < 120:
                catalog_match_summaries.append(
                    summarize_catalog_match(entry, strict_matches, loose_matches, shape_matches)
                )

    for entry in sampler_entries:
        fmt = str(entry.get("fmt", ""))
        if fmt not in NHL_MIPPED_MATERIAL_FORMAT_NAMES:
            continue
        has_mips = parse_bool(entry.get("has_mips", False))
        if not has_mips:
            continue
        final_aniso = parse_int(entry.get("final_aniso", 0))
        preserved = parse_bool(entry.get("preserved_nhl_mips", False))
        if final_aniso != 0 and not preserved:
            add_issue("warning", entry, "mipped NHL12 material sampler still has forced anisotropy")
        if parse_bool(entry.get("mip_base_map", False)):
            add_issue("note", entry, "mipped NHL12 material sampler is clamped to base-map LOD")

    texture_fetches = {entry.get("fetch") for entry in texture_entries if isinstance(entry.get("fetch"), int)}
    sampler_fetches = {entry.get("fetch") for entry in sampler_entries if isinstance(entry.get("fetch"), int)}
    if not texture_entries:
        notes.append("No [NHL-TEX] lines found. Run with --nhl_log_texture_bindings=true.")
    if not sampler_entries:
        notes.append("No [NHL-SAMPLER] lines found. Rebuild with the sampler diagnostic patch.")
    if unused_null_descriptor_entries:
        notes.append(
            f"{unused_null_descriptor_entries} null descriptor binding(s) were unused signed/unsigned slots."
        )
    if empty_null_descriptor_entries:
        notes.append(
            f"{empty_null_descriptor_entries} null descriptor binding(s) had no guest texture data."
        )
    if catalog:
        notes.append(
            "Catalog match: "
            f"{catalog['entries']} entries from {len(catalog_report_paths)} report(s), "
            f"{catalog_strict_matches} strict, {catalog_loose_matches} loose, "
            f"{catalog_fetch_shape_matches} raw fetch-shape, "
            f"{catalog_unmatched_texture_entries} unmatched runtime texture entries."
        )
    if not issues:
        notes.append("No risky NHL texture/sampler states were detected in this log.")

    return LogAnalysis(
        path=str(path),
        texture_entries=len(texture_entries),
        sampler_entries=len(sampler_entries),
        unique_texture_fetches=len(texture_fetches),
        unique_sampler_fetches=len(sampler_fetches),
        catalog_reports=[str(path) for path in catalog_report_paths],
        catalog_entries=catalog["entries"] if catalog else 0,
        catalog_strict_matches=catalog_strict_matches,
        catalog_loose_matches=catalog_loose_matches,
        catalog_fetch_shape_matches=catalog_fetch_shape_matches,
        catalog_unmatched_texture_entries=catalog_unmatched_texture_entries,
        catalog_match_summaries=catalog_match_summaries,
        issues=issues,
        notes=notes,
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default="extracted/cache_hdd/rendering", help="RX2 root directory")
    parser.add_argument(
        "--glob",
        action="append",
        dest="patterns",
        help="Glob relative to --root. Can be repeated.",
    )
    parser.add_argument("--limit", type=int, default=24, help="Limit files checked")
    parser.add_argument("--out", default="build/texture_proof", help="Output directory")
    parser.add_argument("--no-images", action="store_true", help="Skip DDS/PNG output")
    parser.add_argument(
        "--all-mip-pngs",
        action="store_true",
        help="Write one decoded PNG preview per mip level instead of only mip 0",
    )
    parser.add_argument(
        "--fail-non-texture",
        action="store_true",
        help="Count RX2 files without raster metadata as failures instead of skips",
    )
    parser.add_argument(
        "--material-set",
        action="append",
        dest="material_sets",
        help="Prove an NHL12 equipment material stack, e.g. goaliepad:0_11 or glove:0_15.",
    )
    parser.add_argument(
        "--discover-material-dir",
        action="append",
        dest="discover_material_dirs",
        help="Discover complete material stacks in a rendering subdirectory, e.g. goaliepad or glove.",
    )
    parser.add_argument(
        "--discover-material-limit",
        type=int,
        help="Limit discovered material stacks after de-duplication.",
    )
    parser.add_argument(
        "--material-size",
        type=int,
        default=512,
        help="PNG size for material-stack composite previews",
    )
    parser.add_argument(
        "--material-json",
        default="material_report.json",
        help="Material-stack report filename inside --out",
    )
    parser.add_argument(
        "--scan-material-contract-dir",
        action="append",
        dest="material_contract_dirs",
        help=(
            "Scan a rendering subdirectory for NHL12 material-map contract checks. "
            "Can be repeated, e.g. goaliepad, glove, jersey."
        ),
    )
    parser.add_argument(
        "--scan-material-contract-limit",
        type=int,
        help="Limit files checked by --scan-material-contract-dir after de-duplication.",
    )
    parser.add_argument(
        "--material-contract-json",
        default="material_contract_report.json",
        help="Material-map contract report filename inside --out",
    )
    parser.add_argument(
        "--verbose-material-contract",
        action="store_true",
        help="Print benign material-map contract notes, not only failures and warnings.",
    )
    parser.add_argument(
        "--db-file",
        help="NHL12 DB file for optional equipment color palette extraction, e.g. extracted/cache_hdd/db/nhlng.db.",
    )
    parser.add_argument(
        "--db-meta",
        help="NHL12 DB meta XML for optional equipment color palette extraction, e.g. extracted/cache_hdd/db/nhlng-meta.xml.",
    )
    parser.add_argument(
        "--db-bit-order",
        choices=("msb", "lsb"),
        default="msb",
        help="Bit order used for optional DB fixed-record decoding.",
    )
    parser.add_argument(
        "--analyze-log",
        help="Analyze an NHL12 renderer log containing [NHL-TEX] / [NHL-SAMPLER] diagnostics.",
    )
    parser.add_argument(
        "--nhl12-regression-suite",
        action="store_true",
        help="Run the offline NHL12 renderer regression suite and write one JSON verdict.",
    )
    parser.add_argument(
        "--nhl12-regression-suite-fast",
        action="store_true",
        help="Use a faster sampled upload corpus inside --nhl12-regression-suite.",
    )
    parser.add_argument(
        "--suite-json",
        default="nhl12_regression_suite.json",
        help="Regression-suite report filename inside --out.",
    )
    parser.add_argument(
        "--catalog-report",
        action="append",
        dest="catalog_reports",
        help=(
            "Texture proof report.json to match runtime [NHL-TEX] entries against. "
            "Can be repeated."
        ),
    )
    parser.add_argument(
        "--self-test-renderer-key",
        action="store_true",
        help="Run synthetic checks for NHL12 texture-key normalization edge cases.",
    )
    parser.add_argument(
        "--log-json",
        default="log_analysis.json",
        help="Renderer-log analysis filename inside --out",
    )
    parser.add_argument(
        "--self-test-json",
        default="renderer_key_self_test.json",
        help="Renderer-key self-test report filename inside --out",
    )
    parser.add_argument("--json", default="report.json", help="Report filename inside --out")
    args = parser.parse_args(argv)

    root = Path(args.root)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.self_test_renderer_key:
        report = run_renderer_key_self_tests()
        report_path = out_dir / args.self_test_json
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"Renderer key self-test: cases={report['count']} failed={report['failed']}")
        for result in report["results"]:
            print(f"{result['status']:4} {result['name']}")
            for failure in result["failures"]:
                print(f"     {failure}")
        print(f"Wrote {report_path}")
        return 1 if report["failed"] else 0

    if args.analyze_log:
        analysis = analyze_nhl_log(
            Path(args.analyze_log),
            [Path(path) for path in (args.catalog_reports or [])],
        )
        report_path = out_dir / args.log_json
        report_path.write_text(json.dumps(asdict(analysis), indent=2), encoding="utf-8")
        print(
            f"Log {analysis.path}: tex={analysis.texture_entries} "
            f"samplers={analysis.sampler_entries} issues={len(analysis.issues)}"
        )
        if analysis.catalog_entries:
            print(
                "Catalog: "
                f"entries={analysis.catalog_entries} "
                f"strict={analysis.catalog_strict_matches} "
                f"loose={analysis.catalog_loose_matches} "
                f"shape={analysis.catalog_fetch_shape_matches} "
                f"unmatched={analysis.catalog_unmatched_texture_entries}"
            )
            for summary in analysis.catalog_match_summaries[:20]:
                paths = ", ".join(str(path) for path in summary["sample_paths"][:2] if path)
                suffix = f" -> {paths}" if paths else ""
                print(
                    f"CAT {summary['match_type']:6} fetch={summary['fetch']} "
                    f"{summary['fmt']} {summary['size']} "
                    f"mips={summary['mip_min']}-{summary['mip_max']} "
                    f"packed={summary['packed']} pitch={summary['pitch']} "
                    f"shape={summary['fetch_shape_matches']}{suffix}"
                )
        for issue in analysis.issues[:40]:
            fetch = "*" if issue.fetch is None else str(issue.fetch)
            print(f"{issue.severity.upper():7} fetch={fetch:>2} {issue.message}")
        if len(analysis.issues) > 40:
            print(f"... {len(analysis.issues) - 40} more issue(s) in {report_path}")
        for note in analysis.notes:
            print(f"note: {note}")
        print(f"Wrote {report_path}")
        return 1 if any(issue.severity in ("error", "warning") for issue in analysis.issues) else 0

    if args.nhl12_regression_suite:
        report = run_nhl12_regression_suite(root, out_dir, args.nhl12_regression_suite_fast)
        report_path = out_dir / args.suite_json
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(
            f"NHL12 regression suite: {report['status']} "
            f"failed_sections={report['failed_sections']} fast={report['fast']}"
        )
        source_contract = report["source_contract"]
        print(
            "  source contract: "
            f"checks={source_contract['count']} failed={source_contract['failed']}"
        )
        self_test = report["self_test"]
        print(f"  renderer-key self-test: cases={self_test['count']} failed={self_test['failed']}")
        contract = report["material_contract"]
        print(
            "  material contract: "
            f"files={contract['count']} failed={contract['failed']} "
            f"warnings={contract['warnings']}"
        )
        stacks = report["material_stack_composites"]
        print(
            "  material stack composites: "
            f"count={stacks['count']} pass={stacks['passed']} "
            f"fail={stacks['failed']} exact_db={stacks['exact_db_color_matches']} "
            f"max_green={stacks['max_neon_green_ratio']:.4f} "
            f"max_purple={stacks['max_purple_ratio']:.4f} "
            f"max_black={stacks['max_near_black_ratio']:.4f}"
        )
        for name, corpus in report["texture_corpuses"].items():
            print(
                f"  {name}: count={corpus['count']} pass={corpus['passed']} "
                f"skip={corpus['skipped']} fail={corpus['failed']} "
                f"key_fail={corpus['renderer_key_failed']} "
                f"view_fail={corpus['renderer_view_failed']} "
                f"mipped={corpus['renderer_mipped_materials']}"
            )
        for note in report["notes"]:
            print(f"note: {note}")
        print(f"Wrote {report_path}")
        return 0 if report["status"] == "PASS" else 1

    if args.material_contract_dirs:
        report = prove_material_contract(
            root, args.material_contract_dirs, out_dir, args.scan_material_contract_limit
        )
        report_path = out_dir / args.material_contract_json
        report_path.write_text(json.dumps(asdict(report), indent=2), encoding="utf-8")
        print(
            f"Material contract: files={report.count} failed={report.failed} "
            f"warnings={report.warnings}"
        )
        for layer_kind, statuses in report.by_layer_kind.items():
            print(f"  {layer_kind}: {statuses}")
        print(f"  formats: {report.by_format}")
        for entry in report.proofs:
            has_warning = any("outside expected" in note for note in entry.notes)
            should_print = (
                entry.status == "FAIL" or has_warning or args.verbose_material_contract
            )
            if should_print:
                print(
                    f"{entry.status:4} {entry.layer_kind:8} {entry.format:10} "
                    f"{entry.width}x{entry.height} mips={entry.mip_count} "
                    f"packed={entry.packed_mips} {entry.path}"
                )
                for note in entry.notes:
                    print(f"     {note}")
        print(f"Wrote {report_path}")
        return 1 if report.failed else 0

    material_sets = list(args.material_sets or [])
    if args.discover_material_dirs:
        discovered = discover_material_sets(root, args.discover_material_dirs, args.discover_material_limit)
        print(
            f"Discovered {len(discovered)} material set(s) in "
            f"{', '.join(args.discover_material_dirs)}"
        )
        material_sets = list(dict.fromkeys(material_sets + discovered))

    db_colors = None
    if args.db_file or args.db_meta:
        if not args.db_file or not args.db_meta:
            print("--db-file and --db-meta must be supplied together.", file=sys.stderr)
            return 2
        db_colors = load_equipment_db_colors(Path(args.db_meta), Path(args.db_file), args.db_bit_order)
        print(
            f"Loaded DB colors from exhibitiongoalieequipment: "
            f"records={len(db_colors.records)} bit_order={args.db_bit_order}"
        )

    material_failed = 0
    if material_sets:
        material_proofs = []
        for spec in material_sets:
            try:
                proof = prove_material_set(spec, root, out_dir, args.material_size, db_colors)
                material_proofs.append(asdict(proof))
                print(
                    f"{proof.status:4} material {spec} "
                    f"layers={len(proof.layers)} composites={len(proof.composite_png_paths)}"
                )
                if proof.color_source:
                    print(
                        f"     colors {proof.color_source.source} "
                        f"kind={proof.color_source.equipment_kind} "
                        f"id={proof.color_source.equipment_id} "
                        f"matched={proof.color_source.matched_record_count} "
                        f"tints={proof.color_source.tint_colors[:4]}"
                    )
                for layer in proof.layers:
                    print(
                        f"     {layer.name:9} {layer.format:10} "
                        f"{layer.width}x{layer.height} mips={layer.mip_count} "
                        f"packed={layer.packed_mips}"
                    )
                for note in proof.notes:
                    print(f"     note: {note}")
                for metric in proof.composite_metrics:
                    print(
                        f"     lod{metric.lod:<2} artifacts "
                        f"green={metric.neon_green_ratio:.4f} "
                        f"purple={metric.purple_ratio:.4f} "
                        f"transparent={metric.transparent_ratio:.4f} "
                        f"black={metric.near_black_ratio:.4f} "
                        f"{metric.status}"
                    )
                    for note in metric.notes:
                        print(f"          note: {note}")
                if proof.status != "PASS":
                    material_failed += 1
            except Exception as exc:
                material_failed += 1
                material_proofs.append({"spec": spec, "status": "ERROR", "error": str(exc)})
                print(f"ERROR material {spec}: {exc}", file=sys.stderr)

        material_report = {
            "root": str(root),
            "count": len(material_proofs),
            "failed": material_failed,
            "discovered_dirs": args.discover_material_dirs or [],
            "proofs": material_proofs,
        }
        material_report_path = out_dir / args.material_json
        material_report_path.write_text(json.dumps(material_report, indent=2), encoding="utf-8")
        print(f"Wrote {material_report_path}")
        if not args.patterns:
            return 1 if material_failed else 0

    patterns = args.patterns or [
        "blocker/*_dm.rx2",
        "blocker/*_tm.rx2",
        "blocker/*_nm.rx2",
        "blocker/*_sm.rx2",
        "blocker/*_am.rx2",
        "goaliepad/*_dm.rx2",
        "goaliepad/*_tm.rx2",
        "goaliepad/*_nm.rx2",
        "goaliepad/*_sm.rx2",
        "goaliepad/*_am.rx2",
        "glove/*_dm.rx2",
        "glove/*_tm.rx2",
        "glove/*_nm.rx2",
        "glove/*_sm.rx2",
        "glove/*_am.rx2",
        "trapper/*_dm.rx2",
        "trapper/*_tm.rx2",
        "trapper/*_nm.rx2",
        "trapper/*_sm.rx2",
        "trapper/*_am.rx2",
    ]

    inputs = iter_inputs(root, patterns, args.limit)
    if not inputs:
        print("No RX2 files matched.", file=sys.stderr)
        return 2

    proofs = []
    failed = 0
    skipped = 0
    renderer_key_failed = 0
    renderer_view_failed = 0
    renderer_mipped_materials = 0
    renderer_base_only_fixes = 0
    renderer_tiny_base_preserved = 0
    for path in inputs:
        try:
            proof = prove_texture(path, out_dir, not args.no_images, args.all_mip_pngs)
            proofs.append(asdict(proof))
            print(
                f"{proof.status:4} {path} {proof.fetch.format_base_name} "
                f"{proof.fetch.width}x{proof.fetch.height} mips={proof.fetch.mip_max_level_effective + 1} "
                f"packed={proof.fetch.packed_mips}"
            )
            if proof.status != "PASS":
                failed += 1
            if proof.renderer_key.status != "PASS":
                renderer_key_failed += 1
            if proof.renderer_view.status == "FAIL":
                renderer_view_failed += 1
            if proof.renderer_key.mipped_material_mips_preserved and proof.renderer_key.mip_count > 1:
                renderer_mipped_materials += 1
            if proof.renderer_key.base_only_packed_fix_applied:
                renderer_base_only_fixes += 1
            if proof.renderer_key.tiny_base_packed_preserved:
                renderer_tiny_base_preserved += 1
        except NonTextureRx2 as exc:
            skipped += 1
            if args.fail_non_texture:
                failed += 1
                proofs.append({"path": str(path), "status": "ERROR", "error": str(exc)})
                print(f"ERROR {path}: {exc}", file=sys.stderr)
            else:
                proofs.append({"path": str(path), "status": "SKIP", "reason": str(exc)})
                print(f"SKIP {path}: {exc}")
        except Exception as exc:  # Keep unattended runs going.
            failed += 1
            proofs.append({"path": str(path), "status": "ERROR", "error": str(exc)})
            print(f"ERROR {path}: {exc}", file=sys.stderr)

    report = {
        "root": str(root),
        "count": len(proofs),
        "failed": failed,
        "skipped": skipped,
        "patterns": patterns,
        "renderer_key_failed": renderer_key_failed,
        "renderer_view_failed": renderer_view_failed,
        "renderer_mipped_materials": renderer_mipped_materials,
        "renderer_base_only_packed_fixes": renderer_base_only_fixes,
        "renderer_tiny_base_packed_preserved": renderer_tiny_base_preserved,
        "proofs": proofs,
    }
    report_path = out_dir / args.json
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(
        "Renderer key proof: "
        f"mipped_materials={renderer_mipped_materials} "
        f"base_only_fixes={renderer_base_only_fixes} "
        f"tiny_base_preserved={renderer_tiny_base_preserved} "
        f"key_failed={renderer_key_failed} "
        f"view_failed={renderer_view_failed}"
    )
    print(f"Wrote {report_path}")
    return 1 if failed or material_failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
