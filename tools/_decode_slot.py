"""Decode the texture bound to a given fetch_slot from a packet, alpha forced opaque.
Usage: python tools/_decode_slot.py <packet.bin> <slot>"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import highcut_packet_decode as D
path, slot = sys.argv[1], int(sys.argv[2])
buf = open(path, "rb").read()
h, hs, ver = D.parse_header(buf)
seen=set()
for i,(d,blob) in enumerate(D.iter_textures(buf,h,hs)):
    if d["fetch_slot"]!=slot or d["fetch_slot"] in seen: 
        if d["fetch_slot"]==slot: pass
        continue
    seen.add(slot)
    w,hh,fmt=d["width"],d["height"],d["tex_format"]
    if fmt==0:
        rgba=bytearray(blob[:w*hh*4]); rgba+=bytes(w*hh*4-len(rgba))
    elif fmt in (1,2,3):
        rgba=D.decode_bc(blob,w,hh,fmt)
    else:
        print("fmt",fmt,"unsupported"); sys.exit(0)
    for p in range(w*hh): rgba[p*4+3]=255
    out=os.path.splitext(path)[0]+f".SLOT{slot}.{D.TEXFMT.get(fmt)}.{w}x{hh}.png"
    D.write_png(out,w,hh,rgba)
    print(f"slot{slot} {D.TEXFMT.get(fmt)} {w}x{hh} addr=0x{d.get('fetch_base_addr',0):08X} -> {out}")
    break
