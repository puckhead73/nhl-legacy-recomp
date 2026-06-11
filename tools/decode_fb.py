"""Decode raw guest frontbuffer dumps into viewable PNGs to find the composited menu.

Loops over the candidate guest addresses dumped by C++ (oracle_fb.bin / _A / _B),
decoding each as linear k_8_8_8_8 in both channel orders. Saves a PNG per candidate
and prints mean RGB + a coarse "is it flat?" signal so we can spot the real menu.

Usage: py -3.10 decode_fb.py [W H]   (defaults 1280 720)
"""
import sys, os
from PIL import Image

OUT = r"e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
W = int(sys.argv[1]) if len(sys.argv) > 1 else 1280
H = int(sys.argv[2]) if len(sys.argv) > 2 else 720
AW = (W + 31) & ~31

def linear(data, swap_rb):
    out = bytearray(W * H * 4)
    for y in range(H):
        row = (y * AW) * 4
        for x in range(W):
            s = row + x * 4
            b0, b1, b2 = data[s], data[s+1], data[s+2]
            o = (y * W + x) * 4
            if swap_rb:
                out[o], out[o+1], out[o+2], out[o+3] = b2, b1, b0, 255
            else:
                out[o], out[o+1], out[o+2], out[o+3] = b0, b1, b2, 255
    return out

def stats(px):
    n = W * H
    step = 311
    rs = [px[i*4+0] for i in range(0, n, step)]
    gs = [px[i*4+1] for i in range(0, n, step)]
    bs = [px[i*4+2] for i in range(0, n, step)]
    mr, mg, mb = sum(rs)/len(rs), sum(gs)/len(gs), sum(bs)/len(bs)
    # variance of luma -> flat vs real content
    lum = [0.3*rs[i]+0.59*gs[i]+0.11*bs[i] for i in range(len(rs))]
    ml = sum(lum)/len(lum)
    var = sum((l-ml)**2 for l in lum)/len(lum)
    return mr, mg, mb, var

for name in ["oracle_fb.bin", "oracle_fb_A.bin", "oracle_fb_B.bin"]:
    p = os.path.join(OUT, name)
    if not os.path.exists(p):
        print(f"{name}: MISSING"); continue
    data = open(p, "rb").read()
    base = name.replace(".bin", "")
    for swap, tag in [(False, "rgba"), (True, "bgra")]:
        px = linear(data, swap)
        mr, mg, mb, var = stats(px)
        outpng = f"{base}_{tag}.png"
        Image.frombytes("RGBA", (W, H), bytes(px)).convert("RGB").save(os.path.join(OUT, outpng))
        print(f"{outpng}: meanRGB=({mr:.0f},{mg:.0f},{mb:.0f}) lumaVar={var:.0f}")
print("done")
