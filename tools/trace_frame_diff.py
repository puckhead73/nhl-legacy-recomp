#!/usr/bin/env python3
"""Per-frame (per-swap-interval) structural summary of a .xtr stream trace:
PM4 opcode counts, draw-mode counts, indirect-buffer count, memory read/write
counts. Used to find where Xenia's skeleton->content transition (swap ~12)
diverges from the ReXGlue stream."""
import collections
import struct
import sys

RB_MODECONTROL = 0x2208

def frames(path, max_swap):
    data = open(path, 'rb').read()
    pos = 48
    regs = {}
    swap = 0
    cur = collections.Counter()
    out = []

    def handle(words):
        i, n = 0, len(words)
        while i < n:
            head = words[i]
            pt = head >> 30
            if pt == 0:
                base = head & 0x7FFF
                cnt = ((head >> 16) & 0x3FFF) + 1
                one = (head >> 15) & 1
                for k in range(cnt):
                    if i + 1 + k >= n:
                        break
                    regs[base + (0 if one else k)] = words[i + 1 + k]
                cur['t0_regs'] += cnt
                i += 1 + cnt
            elif pt == 3:
                op = (head >> 8) & 0x7F
                cnt = ((head >> 16) & 0x3FFF) + 1
                cur[f'op{op:02X}'] += 1
                if op in (0x22, 0x36):
                    mode = regs.get(RB_MODECONTROL, 0) & 7
                    mask = regs.get(0x2104, 0)
                    cur[f'draw_m{mode}_msk{mask:X}'] += 1
                i += 1 + cnt
            else:
                i += 1

    while pos + 4 <= len(data) and swap <= max_swap:
        (t,) = struct.unpack_from('<I', data, pos)
        if t > 11:
            break
        if t in (0, 2):
            if t == 2:
                cur['ib'] += 1
            pos += 12
        elif t in (1, 3, 5):
            pos += 4
        elif t == 4:
            v = struct.unpack_from('<III', data, pos)
            pos += 12
            handle(struct.unpack_from(f'>{v[2]}I', data, pos))
            pos += v[2] * 4
        elif t in (6, 7):
            v = struct.unpack_from('<IIIII', data, pos)
            cur['memrd' if t == 6 else 'memwr'] += 1
            cur['memrd_b' if t == 6 else 'memwr_b'] += v[4]
            pos += 20 + v[3]
        elif t == 8:
            v = struct.unpack_from('<III', data, pos)
            pos += 12 + v[2]
        elif t == 9:
            out.append((swap, cur))
            cur = collections.Counter()
            swap += 1
            pos += 8
        elif t == 10:
            v = struct.unpack_from('<IIIIII', data, pos)
            pos += 24 + v[5]
        elif t == 11:
            v = struct.unpack_from('<IIII', data, pos)
            pos += 16 + v[3]
    return out

def main():
    max_swap = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    a = frames(sys.argv[1], max_swap)
    b = frames(sys.argv[2], max_swap)
    for (sa, ca), (sb, cb) in zip(a, b):
        keys = sorted(set(ca) | set(cb))
        diffs = [f'{k}: {ca.get(k,0)}/{cb.get(k,0)}' for k in keys
                 if ca.get(k, 0) != cb.get(k, 0)]
        same = [f'{k}:{ca[k]}' for k in sorted(ca) if ca[k] == cb.get(k, 0)]
        print(f'== swap {sa}  (A/B differing)')
        print('   diff:', '; '.join(diffs) if diffs else '(none)')
        print('   same:', ' '.join(same))

if __name__ == '__main__':
    main()
