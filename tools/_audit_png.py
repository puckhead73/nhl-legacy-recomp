#!/usr/bin/env python3
"""C-5 broader-scene audit: scan a headless replay PNG for gross renderer anomalies.
Usage: python tools/_audit_png.py <png>
Flags (heuristic, for triage — a human still eyeballs the image):
  magenta%  : unsupported-format stub texels leaked to screen (untileBindings couldn't decode)
  black%    : near-black pixels (dark equipment / missing-lighting / dropped surface)
  green%    : strong-green-dominant pixels (the classic stale-resolve / BC corruption signature)
A clean broadcast frame: magenta ~0, black modest (crowd shadows/edges), green low.
"""
import sys
from PIL import Image

def main():
    if len(sys.argv) < 2:
        print("usage: _audit_png.py <png>"); return 2
    im = Image.open(sys.argv[1]).convert('RGB')
    px = im.load(); W, H = im.size
    n = W * H
    magenta = black = green = bright = 0
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            if r > 200 and b > 200 and g < 80: magenta += 1          # magenta stub
            if r < 18 and g < 18 and b < 18: black += 1              # near-black
            if g > 90 and g > r + 55 and g > b + 55: green += 1      # green-dominant corruption
            if r + g + b > 360: bright += 1                          # lit content present
    pct = lambda v: 100.0 * v / n
    print(f"  size {W}x{H}")
    print(f"  magenta(stub)   {pct(magenta):6.3f}%   {'<<< FORMAT STUB LEAK' if pct(magenta)>0.05 else 'ok'}")
    print(f"  green(corrupt)  {pct(green):6.3f}%   {'<<< CHECK (corruption?)' if pct(green)>1.5 else 'ok'}")
    print(f"  near-black      {pct(black):6.3f}%   {'<<< CHECK (dark/dropped?)' if pct(black)>35 else 'ok'}")
    print(f"  lit content     {pct(bright):6.3f}%   {'ok' if pct(bright)>8 else '<<< scene may be missing/dark'}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
