"""C-5g: find player uber-draws in a captured frame, group them by PS-shader hash,
and catalog each draw's textures. Answers the §5.1 blocker: do helmet and jersey
draws share the same translated PS, or different ones?

Usage: python tools/_scan_player_draws.py <build_dir>
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import highcut_packet_decode as D

build = sys.argv[1]
cnt = int(open(os.path.join(build, "highcut_frame.count")).read().split()[0])

# field offsets within a packet, mirroring _extract_spirv.py
def spirv_blobs(buf, h, hs):
    off = (hs + h["fetch_bytes"] + h["sys_bytes"] + h["shared_bytes"] + h["bool_bytes"]
           + h["vs_float_bytes"] + h["ps_float_bytes"])
    vs = buf[off:off + h["vs_spirv_bytes"]]; off += h["vs_spirv_bytes"]
    ps = buf[off:off + h["ps_spirv_bytes"]]
    return vs, ps

groups = {}   # ps_hash -> list of draw dicts
for i in range(cnt):
    p = os.path.join(build, f"highcut_frame_{i}.bin")
    if not os.path.exists(p):
        continue
    buf = open(p, "rb").read()
    h, hs, ver = D.parse_header(buf)
    if not h:
        continue
    vs_tex = h.get("vs_texture_count", 0)
    ps_tex = h.get("texture_count", 0)
    sdb = h.get("surface_depth_base", 0)
    # player uber-draw heuristic from the kickoff prompt
    if not (vs_tex > 0 and ps_tex >= 18 and sdb == 736):
        continue
    vs, ps = spirv_blobs(buf, h, hs)
    ph = hashlib.sha1(ps).hexdigest()[:12]
    # catalog textures
    texs = []
    for d, blob in D.iter_textures(buf, h, hs):
        texs.append((d["fetch_slot"], D.TEXFMT.get(d["tex_format"], "?"),
                     f"{d['width']}x{d['height']}", d.get("fetch_base_addr", 0)))
    groups.setdefault(ph, []).append({
        "draw": i, "vs_tex": vs_tex, "ps_tex": ps_tex,
        "verts": h["vertex_count"], "ps_bytes": len(ps), "vs_bytes": len(vs),
        "texs": texs,
    })

print(f"frame: {cnt} draws; {sum(len(v) for v in groups.values())} player uber-draws "
      f"in {len(groups)} distinct PS shader(s)\n")
for ph, draws in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    didx = [d["draw"] for d in draws]
    print(f"=== PS {ph}  ({len(draws)} draws, {draws[0]['ps_bytes']}B)  draws={didx}")
    # show the texture slot/format signature of the first draw in the group
    d0 = draws[0]
    print(f"    sample draw {d0['draw']}: verts={d0['verts']} vs_tex={d0['vs_tex']} ps_tex={d0['ps_tex']}")
    for slot, fmt, dim, addr in sorted(d0["texs"]):
        print(f"      slot{slot:<2} {fmt:8} {dim:10} addr=0x{addr:08X}")
