"""Decode one texture from a packet with alpha forced opaque (defeats the
alpha=0-looks-white viewer artifact) so base colormaps are visible. Also
prints a crude content fingerprint (mean RGB, % non-near-white, % black).
Usage: python tools/_decode_opaque.py <packet.bin> <tex_index>
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(__file__))
import highcut_packet_decode as D
path, ti = sys.argv[1], int(sys.argv[2])
buf = open(path, "rb").read()
h, hs, ver = D.parse_header(buf)
for i, (d, blob) in enumerate(D.iter_textures(buf, h, hs)):
    if i != ti:
        continue
    w, hh, fmt = d["width"], d["height"], d["tex_format"]
    if fmt in (0,):
        rgba = bytearray(blob[:w*hh*4]); rgba += bytes(w*hh*4-len(rgba))
    elif fmt in (1,2,3):
        rgba = D.decode_bc(blob, w, hh, fmt)
    else:
        print("unsupported fmt", fmt); sys.exit(1)
    # force opaque + fingerprint
    n = w*hh; sr=sg=sb=0; nonwhite=0; black=0
    for p in range(n):
        r,g,b = rgba[p*4], rgba[p*4+1], rgba[p*4+2]
        rgba[p*4+3] = 255
        sr+=r; sg+=g; sb+=b
        if r<240 or g<240 or b<240: nonwhite+=1
        if r<16 and g<16 and b<16: black+=1
    out = os.path.splitext(path)[0] + f".OPAQUE.tex{ti}.{w}x{hh}.png"
    D.write_png(out, w, hh, rgba)
    print(f"tex{ti} {D.TEXFMT.get(fmt)} {w}x{hh} slot={d['fetch_slot']} addr=0x{d.get('fetch_base_addr',0):08X}")
    print(f"  meanRGB=({sr//n},{sg//n},{sb//n})  non-near-white={100*nonwhite//n}%  near-black={100*black//n}%")
    print(f"  -> {out}")
    break
