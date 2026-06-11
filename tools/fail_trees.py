#!/usr/bin/env python3
"""Group cache:\ load failures in a runtime log by tree, to target cache population."""
import re, collections, sys

lines = open(sys.argv[1], encoding='utf-8', errors='ignore').read().splitlines()
fails = []
for l in lines:
    if '0xc000000f' not in l:
        continue
    m = re.search(r"path='([^']*)'", l)
    if m:
        fails.append(m.group(1))

top = collections.Counter()
sub = collections.Counter()
for p in fails:
    q = p.replace('cache:' + '\\', '')
    parts = q.split('\\')
    if parts:
        top[parts[0]] += 1
    if len(parts) >= 2:
        sub[parts[0] + '/' + parts[1]] += 1

print('total fails:', len(fails))
print('--- by top tree ---')
for k, c in top.most_common():
    print(f'  {c:4d}  {k}')
print('--- second-level (top 20) ---')
for k, c in sub.most_common(20):
    print(f'  {c:4d}  {k}')
