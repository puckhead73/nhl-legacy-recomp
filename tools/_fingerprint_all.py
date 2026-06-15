"""Decode every texture in a packet, report a color fingerprint per slot to spot
the gold-digit font (high R,G / low B, sparse) vs normals vs base. No PNG spam.
Usage: python tools/_fingerprint_all.py <packet.bin>"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import highcut_packet_decode as D
buf=open(sys.argv[1],"rb").read()
h,hs,ver=D.parse_header(buf)
seen=set()
for d,blob in D.iter_textures(buf,h,hs):
    s=d["fetch_slot"]
    if s in seen: continue
    seen.add(s)
    w,hh,fmt=d["width"],d["height"],d["tex_format"]
    tag=D.TEXFMT.get(fmt,"?")
    if fmt==0:
        rgba=bytearray(blob[:w*hh*4]); rgba+=bytes(w*hh*4-len(rgba))
    elif fmt in (1,2,3):
        rgba=D.decode_bc(blob,w,hh,fmt)
    else:
        print(f"slot{s:<2} {tag:6} {w}x{hh} addr=0x{d.get('fetch_base_addr',0):08X}  (no decode)"); continue
    n=w*hh; sr=sg=sb=sa=0; gold=0; opaque_a=0
    for p in range(n):
        r,g,b,a=rgba[p*4],rgba[p*4+1],rgba[p*4+2],rgba[p*4+3]
        sr+=r;sg+=g;sb+=b;sa+=a
        if r>140 and g>110 and b<110: gold+=1     # gold/yellow-ish pixel
        if a>200: opaque_a+=1
    print(f"slot{s:<2} {tag:6} {w}x{hh:<4} addr=0x{d.get('fetch_base_addr',0):08X}  "
          f"meanRGBA=({sr//n:3},{sg//n:3},{sb//n:3},{sa//n:3})  gold%={100*gold//n:3}  alpha>200%={100*opaque_a//n:3}")
