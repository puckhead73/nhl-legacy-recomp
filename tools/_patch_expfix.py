"""Apply the C-5g exp_adjust fix to a captured packet's fetch blob in-place, so the
existing capture can be re-rendered to verify the fix WITHOUT a re-dump. Patches the
given texture fetch constant(s): dword_4 = (dword_4 & ~lod_bias) | real_exp(dword_3<<13).
Usage: python tools/_patch_expfix.py <packet.bin> <fc> [<fc> ...]"""
import struct, sys
sys.path.insert(0, 'tools'); import highcut_packet_decode as D
path = sys.argv[1]; fcs = [int(x) for x in sys.argv[2:]] or [8]
buf = bytearray(open(path, 'rb').read())
h, hs, ver = D.parse_header(buf)
fetch_off = hs  # fetch blob is right after the header
for fc in fcs:
    d3o = fetch_off + (fc*6 + 3)*4
    d4o = fetch_off + (fc*6 + 4)*4
    d3 = struct.unpack_from('<I', buf, d3o)[0]
    d4 = struct.unpack_from('<I', buf, d4o)[0]
    real_exp = (d3 >> 13) & 0x3F
    new = (d4 & ~(0x3FF << 12)) | (real_exp << 13)
    struct.pack_into('<I', buf, d4o, new)
    print(f"fc{fc}: dword_4 0x{d4:08X} -> 0x{new:08X}  (real_exp={real_exp if real_exp<32 else real_exp-64})")
open(path, 'wb').write(buf)
print("patched", path)
