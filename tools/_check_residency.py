"""C-5g RESIDENCY PRE-FLIGHT. Before investing in a replay/instrument pass, verify a
captured frame actually has the jersey number-glyph assets resident. The recurring
trap (2026-06-14/15): a live F10 capture grabs guest memory at a moment when the
number-glyph atlas hasn't streamed in, so its texels are the game's missing-texture
MAGENTA (or an empty BLACK base). Such a capture CANNOT render the number no matter
how correct the compositing is — debugging it is wasted effort.

This scans the player uber-draws (vs_tex>0, ps_tex>=18, surface_depth_base==736) and
classifies every texture's health:
  OK        — real content
  MAGENTA   — uniform (255,0,255): the game's missing-texture placeholder (NOT our 2x2
              stub; this is full-size guest memory) => asset not resident
  BLACK     — uniform near-zero RGBA: empty/uncaptured
  UNIFORM   — single flat color (e.g. an all-yellow coverage map): suspicious

Shared BC3 atlases (one address sampled by >1 player draw) are the font/decal atlases;
if one of those is MAGENTA, the jersey number cannot render. Verdict: PASS / FAIL.

Usage: python tools/_check_residency.py <build_dir>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import highcut_packet_decode as D


def classify(d, blob):
    """Return (verdict, meanRGBA) for a texture descriptor+blob."""
    w, hh, fmt = d["width"], d["height"], d["tex_format"]
    if fmt == 4:        # RGBA32F bone palette — not an image
        return "OK", None
    if fmt == 5:        # BC5 normal — stdlib decoder can't; assume OK (rarely a font)
        return "OK", None
    if fmt == 0:
        rgba = bytearray(blob[:w * hh * 4]); rgba += bytes(w * hh * 4 - len(rgba))
    else:
        rgba = D.decode_bc(blob, w, hh, fmt)
    n = w * hh
    if n == 0:
        return "EMPTY", None
    sr = sg = sb = sa = 0
    # uniformity: compare every texel to the first
    r0, g0, b0 = rgba[0], rgba[1], rgba[2]
    uniform = True
    step = max(1, n // 4096)  # sample for speed on big textures
    cnt = 0
    for p in range(0, n, step):
        r, g, b, a = rgba[p * 4], rgba[p * 4 + 1], rgba[p * 4 + 2], rgba[p * 4 + 3]
        sr += r; sg += g; sb += b; sa += a; cnt += 1
        if abs(r - r0) > 8 or abs(g - g0) > 8 or abs(b - b0) > 8:
            uniform = False
    mean = (sr // cnt, sg // cnt, sb // cnt, sa // cnt)
    if uniform:
        if mean[0] > 230 and mean[1] < 40 and mean[2] > 230:
            return "MAGENTA", mean
        if mean[0] < 16 and mean[1] < 16 and mean[2] < 16:
            return "BLACK", mean
        return "UNIFORM", mean
    return "OK", mean


def main():
    build = sys.argv[1]
    cnt = int(open(os.path.join(build, "highcut_frame.count")).read().split()[0])
    player_draws = []        # (draw_idx, [(slot,fmt,dim,addr,verdict,mean)])
    addr_to_draws = {}       # fetch_base_addr -> set(draw_idx) for BCn (shared-atlas detection)
    addr_info = {}           # fetch_base_addr -> (fmt, dim, verdict)
    for i in range(cnt):
        p = os.path.join(build, f"highcut_frame_{i}.bin")
        if not os.path.exists(p):
            continue
        buf = open(p, "rb").read()
        h, hs, ver = D.parse_header(buf)
        if not h:
            continue
        if not (h.get("vs_texture_count", 0) > 0 and h.get("texture_count", 0) >= 18
                and h.get("surface_depth_base", 0) == 736):
            continue
        rows = []
        seen = set()
        for d, blob in D.iter_textures(buf, h, hs):
            s = d["fetch_slot"]
            if s in seen:
                continue
            seen.add(s)
            v, mean = classify(d, blob)
            addr = d.get("fetch_base_addr", 0)
            fmt = D.TEXFMT.get(d["tex_format"], "?")
            rows.append((s, fmt, f"{d['width']}x{d['height']}", addr, v, mean))
            if fmt in ("BC1", "BC2", "BC3", "BC5"):
                addr_to_draws.setdefault(addr, set()).add(i)
                addr_info[addr] = (fmt, f"{d['width']}x{d['height']}", v)
        player_draws.append((i, rows))

    if not player_draws:
        print("no player uber-draws found (vs_tex>0, ps_tex>=18, depth=736) — wrong capture?")
        return 2

    print(f"frame: {cnt} draws; {len(player_draws)} player uber-draws "
          f"[{', '.join(str(d) for d, _ in player_draws)}]\n")
    for didx, rows in player_draws:
        print(f"=== draw {didx} ===")
        for s, fmt, dim, addr, v, mean in sorted(rows):
            flag = "" if v == "OK" else f"   <<< {v}"
            mtxt = f" mean={mean}" if mean else ""
            print(f"  slot{s:<2} {fmt:6} {dim:10} 0x{addr:08X}  {v:8}{mtxt}{flag}")
        print()

    # Shared BCn atlases (sampled by >1 player draw) — the font/decal/number atlases.
    shared = {a: ds for a, ds in addr_to_draws.items() if len(ds) > 1}
    print("shared BCn atlases (sampled by >1 player draw — font / decal / number layers):")
    bad = []
    for a, ds in sorted(shared.items()):
        fmt, dim, v = addr_info[a]
        note = "" if v == "OK" else f"   <<< {v} (NOT RESIDENT)"
        print(f"  0x{a:08X} {fmt} {dim}  draws={sorted(ds)}  {v}{note}")
        if v in ("MAGENTA", "BLACK", "UNIFORM"):
            bad.append((a, fmt, dim, v))

    # Verdict
    any_magenta = any(v == "MAGENTA" for _, rows in player_draws for *_, v, _ in rows)
    print()
    if bad or any_magenta:
        print("VERDICT: FAIL — number/decal assets are NOT resident in this capture.")
        for a, fmt, dim, v in bad:
            print(f"         shared atlas 0x{a:08X} ({fmt} {dim}) is {v}.")
        print("         Re-capture with jersey numbers clearly ON-SCREEN at F10 so the")
        print("         glyph atlas is paged in. This capture cannot render the number.")
        return 1
    print("VERDICT: PASS — no missing (magenta/black/uniform) shared atlases in player draws.")
    print("         Safe to replay / instrument for the jersey-number compositing path.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
