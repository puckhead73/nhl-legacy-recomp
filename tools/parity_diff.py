import sys
from PIL import Image, ImageChops

a = Image.open(sys.argv[1]).convert('RGB')
b = Image.open(sys.argv[2]).convert('RGB')
if a.size != b.size:
    b = b.resize(a.size)
w, h = a.size
pa, pb = a.load(), b.load()

diff = ImageChops.difference(a, b)
diff.save(sys.argv[3] if len(sys.argv) > 3 else 'parity_diff.png')

tot = w * h
sse = 0
nd = [0, 0, 0, 0]  # pixels differing by > 8/32/64/128
for y in range(h):
    for x in range(w):
        ra, ga, ba = pa[x, y]
        rb, gb, bb = pb[x, y]
        d = abs(ra-rb) + abs(ga-gb) + abs(ba-bb)
        sse += (ra-rb)**2 + (ga-gb)**2 + (ba-bb)**2
        m = max(abs(ra-rb), abs(ga-gb), abs(ba-bb))
        if m > 8:   nd[0] += 1
        if m > 32:  nd[1] += 1
        if m > 64:  nd[2] += 1
        if m > 128: nd[3] += 1
mse = sse / (tot * 3)
import math
psnr = 10 * math.log10(255*255 / mse) if mse else 99
print(f'{sys.argv[1]} vs {sys.argv[2]}  ({w}x{h})')
print(f'  MSE={mse:.1f}  PSNR={psnr:.2f} dB')
print(f'  pixels differing >8/255:  {100.0*nd[0]/tot:.1f}%')
print(f'  pixels differing >32/255: {100.0*nd[1]/tot:.1f}%')
print(f'  pixels differing >64/255: {100.0*nd[2]/tot:.1f}%')
print(f'  pixels differing >128/255:{100.0*nd[3]/tot:.1f}%')
