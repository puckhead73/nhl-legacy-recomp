#!/usr/bin/env python3
"""Annotate guest_stacks.txt LR values with the containing sub_ function
(nearest preceding SetFunction address from nhllegacy_register.cpp)."""
import bisect, re, sys

def main():
    register_cpp, stacks_txt = sys.argv[1], sys.argv[2]
    addrs = sorted({int(m, 16) for m in
                    re.findall(r'SetFunction\(0x([0-9A-Fa-f]+)',
                               open(register_cpp).read())})
    def sym(va):
        if not (0x82000000 <= va < 0x84000000):
            return f'{va:08X}'
        i = bisect.bisect_right(addrs, va) - 1
        if i < 0:
            return f'{va:08X}'
        return f'{va:08X}(sub_{addrs[i]:08X}+{va - addrs[i]:X})'

    for line in open(stacks_txt):
        if line.startswith('  stack:'):
            toks = line.split()
            out = ['  stack:']
            for t in toks[1:]:
                m = re.fullmatch(r'(<-)?([0-9A-Fa-f]{8})', t)
                if m:
                    out.append((m.group(1) or '') + sym(int(m.group(2), 16)))
                else:
                    out.append(t)
            print(' '.join(out))
        else:
            print(line, end='')

if __name__ == '__main__':
    main()
