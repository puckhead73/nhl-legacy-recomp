"""Dump per-channel min/max/unique-count of a slot's RGBA8 texture to see if a
'uniform' coverage map actually hides structure in one channel.
Usage: python tools/_channels.py <packet.bin> <slot>"""
import os,sys
sys.path.insert(0,os.path.dirname(__file__))
import highcut_packet_decode as D
buf=open(sys.argv[1],"rb").read(); slot=int(sys.argv[2])
h,hs,_=D.parse_header(buf)
for d,blob in D.iter_textures(buf,h,hs):
    if d["fetch_slot"]!=slot: continue
    w,hh,fmt=d["width"],d["height"],d["tex_format"]
    if fmt!=0:
        print("not RGBA8"); break
    px=blob[:w*hh*4]
    for ci,cn in enumerate("RGBA"):
        vals=px[ci::4]
        mn,mx=min(vals),max(vals); uniq=len(set(vals))
        # histogram of a non-uniform channel
        extra=""
        if uniq>1:
            from collections import Counter
            top=Counter(vals).most_common(4)
            extra="  top="+",".join(f"{v}:{c}" for v,c in top)
        print(f"  {cn}: min={mn} max={mx} uniq={uniq}{extra}")
    # also save the alpha channel as a grayscale png to eyeball
    out=os.path.splitext(sys.argv[1])[0]+f".slot{slot}.ALPHA.png"
    rgba=bytearray(w*hh*4)
    for i in range(w*hh):
        a=px[i*4+3]; rgba[i*4:i*4+4]=bytes((a,a,a,255))
    D.write_png(out,w,hh,rgba); print("  alpha-as-gray ->",out)
    break
