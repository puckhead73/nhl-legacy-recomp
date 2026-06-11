"""Compare our owned-render output vs the oracle to characterize the color transform.

Samples the sky region (clouds) which is large + roughly uniform in both images, then
fits candidate transforms (linear scale, power/gamma, sRGB encode) to see which one maps
our pixels onto the oracle's. Tells us the exact correction needed for parity.

Usage: py -3.10 color_fit.py <ours.png> <oracle.png>
"""
import sys, math
from PIL import Image

def load(p):
    im = Image.open(p).convert("RGB")
    return im

def resize_to(im, w, h):
    return im.resize((w, h), Image.BILINEAR)

ours = load(sys.argv[1])
orac = load(sys.argv[2])
print(f"ours  {ours.size}  oracle {orac.size}")

# Resize oracle to ours so we sample the same normalized coords.
if orac.size != ours.size:
    orac = resize_to(orac, ours.width, ours.height)

W, H = ours.size
po, pr = ours.load(), orac.load()

# Sample a grid over the upper sky region (avoid the flag/UI at center-bottom).
xs = range(W // 10, W, W // 10)
ys = range(H // 10, H // 2, H // 10)  # upper half = sky
pairs = []
for y in ys:
    for x in xs:
        pairs.append((po[x, y], pr[x, y]))

# Per-channel summary + candidate-fit error.
def srgb_encode(c):  # linear -> sRGB, c in [0,1]
    return 12.92*c if c <= 0.0031308 else 1.055*(c**(1/2.4)) - 0.055

for ch, name in enumerate("RGB"):
    o = [a[ch]/255.0 for a, b in pairs]
    r = [b[ch]/255.0 for a, b in pairs]
    mo, mr = sum(o)/len(o), sum(r)/len(r)
    # best linear scale  r ~= k*o
    num = sum(oi*ri for oi, ri in zip(o, r)); den = sum(oi*oi for oi in o) or 1e-9
    k = num/den
    err_lin = sum((k*oi-ri)**2 for oi, ri in zip(o, r))/len(o)
    # best gamma  r ~= o^g  (fit g via log-log, ignoring near-0)
    gs = [math.log(ri+1e-4)/math.log(oi+1e-4) for oi, ri in zip(o, r) if oi > 0.02 and ri > 0.02]
    g = sum(gs)/len(gs) if gs else float('nan')
    err_gam = sum((oi**g-ri)**2 for oi, ri in zip(o, r) if oi > 0)/max(1, sum(1 for oi in o if oi>0))
    # sRGB encode  r ~= srgb(o)
    err_srgb = sum((srgb_encode(oi)-ri)**2 for oi, ri in zip(o, r))/len(o)
    print(f"{name}: mean ours={mo:.3f} oracle={mr:.3f} | k={k:.3f}(e={err_lin:.4f}) "
          f"gamma={g:.3f}(e={err_gam:.4f}) srgb(e={err_srgb:.4f})")
