#!/usr/bin/env python3
"""XEX2 plaintext-header inspector for nhl12-recomp.

Parses everything readable without decrypting the basefile: module flags,
security info, optional headers (entry point, base address, file format,
execution info), and the import-library directory (names, versions, counts).

Usage: python xexinfo.py <default.xex>
"""

import struct
import sys

OPT_KEYS = {
    0x000002FF: "RESOURCE_INFO",
    0x000003FF: "FILE_FORMAT_INFO",
    0x000005FF: "DELTA_PATCH_DESCRIPTOR",
    0x00000405: "BASE_REFERENCE",
    0x000080FF: "BOUNDING_PATH",
    0x00008105: "DEVICE_ID",
    0x00010001: "ORIGINAL_BASE_ADDRESS",
    0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP",
    0x00018102: "ENABLED_FOR_CALLCAP",
    0x00018200: "ENABLED_FOR_FASTCAP",
    0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE",
    0x00020301: "DEFAULT_FS_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE",
    0x00028002: "PAGE_HEAP_SIZE_AND_FLAGS",
    0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO",
    0x00040201: "TITLE_WORKSPACE_SIZE",
    0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO",
    0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALTERNATE_TITLE_IDS",
    0x00040801: "ADDITIONAL_TITLE_MEMORY",
    0x00E10402: "EXPORTS_BY_NAME",
}

MODULE_FLAGS = {
    0x01: "TITLE", 0x02: "EXPORTS_TO_TITLE", 0x04: "SYSTEM_DEBUGGER",
    0x08: "DLL_MODULE", 0x10: "MODULE_PATCH", 0x20: "PATCH_FULL",
    0x40: "PATCH_DELTA", 0x80: "USER_MODE",
}

IMAGE_FLAGS = {
    0x00000002: "MANUFACTURING_UTILITY", 0x00000004: "MANUFACTURING_SUPPORT_TOOL",
    0x00000008: "XGD2_MEDIA_ONLY", 0x00000100: "CARDEA_KEY",
    0x00000200: "XEIKA_KEY", 0x00000400: "USERMODE_TITLE",
    0x00000800: "USERMODE_SYSTEM", 0x00001000: "ORANGE0",
    0x00002000: "ORANGE1", 0x00004000: "ORANGE2",
    0x00010000: "IPTV_SIGNUP_APPLICATION", 0x00020000: "IPTV_TITLE_APPLICATION",
    0x02000000: "KEYVAULT_PRIVILEGES_REQUIRED", 0x04000000: "ONLINE_ACTIVATION_REQUIRED",
    0x08000000: "PAGE_SIZE_4KB", 0x10000000: "REGION_FREE",
    0x20000000: "REVOCATION_CHECK_OPTIONAL", 0x40000000: "REVOCATION_CHECK_REQUIRED",
}


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def flags_str(val, table):
    return " | ".join(n for bit, n in table.items() if val & bit) or "none"


def main(path):
    data = open(path, "rb").read()
    assert data[:4] == b"XEX2", "not a XEX2 file"
    module_flags = be32(data, 4)
    pe_offset = be32(data, 8)
    sec_offset = be32(data, 0x10)
    opt_count = be32(data, 0x14)

    print(f"file size:        {len(data):,}")
    print(f"module flags:     0x{module_flags:08X} ({flags_str(module_flags, MODULE_FLAGS)})")
    print(f"basefile offset:  0x{pe_offset:X} (header region = {pe_offset:,} bytes)")

    # --- security info ---
    image_size = be32(data, sec_offset + 0x04)
    image_flags = be32(data, sec_offset + 0x10C)
    load_address = be32(data, sec_offset + 0x110)
    export_table = be32(data, sec_offset + 0x160)
    game_regions = be32(data, sec_offset + 0x178)
    media_flags = be32(data, sec_offset + 0x17C)
    aes_key = data[sec_offset + 0x150 : sec_offset + 0x160]
    print(f"image size:       0x{image_size:X} ({image_size:,})")
    print(f"load address:     0x{load_address:08X}")
    print(f"image flags:      0x{image_flags:08X} ({flags_str(image_flags, IMAGE_FLAGS)})")
    print(f"export table:     0x{export_table:08X}")
    print(f"game regions:     0x{game_regions:08X}")
    print(f"media flags:      0x{media_flags:08X}")
    print(f"enc. file key:    {aes_key.hex()}")

    # --- optional headers ---
    opts = {}
    for i in range(opt_count):
        key = be32(data, 0x18 + i * 8)
        val = be32(data, 0x18 + i * 8 + 4)
        opts[key] = val
    print(f"\noptional headers ({opt_count}):")
    for key, val in sorted(opts.items()):
        name = OPT_KEYS.get(key, "?")
        inline = (key & 0xFF) < 2  # low byte 0/1 => value stored inline
        kind = "inline" if inline else "@offset"
        print(f"  0x{key:08X} {name:<24} {kind} 0x{val:08X}")

    print()
    if 0x00010100 in opts:
        print(f"entry point:      0x{opts[0x00010100]:08X}")
    if 0x00010201 in opts:
        print(f"image base:       0x{be32(data, opts[0x00010201]) if (0x00010201 & 0xFF) >= 2 else opts[0x00010201]:08X}")
    if 0x00010001 in opts:
        print(f"original base:    0x{opts[0x00010001]:08X}")
    if 0x000183FF in opts:
        o = opts[0x000183FF]
        size = be32(data, o)
        name = data[o + 4 : o + 4 + size].split(b"\0")[0].decode("ascii", "replace")
        print(f"original PE name: {name}")
    if 0x00020200 in opts:
        print(f"default stack:    0x{opts[0x00020200]:X}")
    if 0x00030000 in opts:
        print(f"system flags:     0x{opts[0x00030000]:08X}")

    if 0x00040006 in opts:
        o = opts[0x00040006]
        media_id, version, base_version, title_id = struct.unpack_from(">IIII", data, o)
        platform, exec_table, disc_no, disc_cnt = struct.unpack_from("BBBB", data, o + 16)
        print(f"execution info:   title_id=0x{title_id:08X} media_id=0x{media_id:08X}")
        print(f"                  version=0x{version:08X} base_version=0x{base_version:08X}")
        print(f"                  platform={platform} disc {disc_no}/{disc_cnt}")

    if 0x000003FF in opts:
        o = opts[0x000003FF]
        enc_type, comp_type = struct.unpack_from(">HH", data, o + 4)
        enc = {0: "none", 1: "encrypted (retail/devkit key)"}.get(enc_type, "?")
        comp = {0: "none", 1: "raw (basic blocks)", 2: "compressed (LZX)",
                3: "delta-compressed"}.get(comp_type, "?")
        print(f"file format:      encryption={enc}, compression={comp}")
        if comp_type == 2:
            win_size, block_size = struct.unpack_from(">II", data, o + 8)
            print(f"                  lzx window=0x{win_size:X} first_block=0x{block_size:X}")

    if 0x000103FF in opts:
        o = opts[0x000103FF]
        str_size, str_count = struct.unpack_from(">II", data, o + 4)
        strings = data[o + 12 : o + 12 + str_size].split(b"\0")
        names = [s.decode("ascii", "replace") for s in strings if s]
        lib_off = o + 12 + str_size
        lib_count = str_count
        print(f"\nimport libraries ({lib_count}):")
        total = 0
        for _ in range(lib_count):
            size = be32(data, lib_off)
            lib_id = be32(data, lib_off + 0x18)
            ver = be32(data, lib_off + 0x1C)
            ver_min = be32(data, lib_off + 0x20)
            name_idx, count = struct.unpack_from(">HH", data, lib_off + 0x24)
            name = names[name_idx] if name_idx < len(names) else f"#{name_idx}"
            v = f"{(ver >> 28) & 0xF}.{(ver >> 24) & 0xF}.{(ver >> 8) & 0xFFFF}.{ver & 0xFF}"
            print(f"  {name:<16} version={v} imports={count}")
            total += count
            lib_off += size
        print(f"  total import records: {total}")


if __name__ == "__main__":
    main(sys.argv[1])
