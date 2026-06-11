#!/usr/bin/env python3
"""Scan a .xtr GPU trace for 3D content draws (RB_MODECONTROL edram_mode 4,
depth-tested), and for each distinct draw state report whether it's a player
mesh and whether its vertex / viewport data looks degenerate.

Discriminates:
  - no 3D draws at all        -> CPU never issues player geometry
  - 3D draws, sane viewport   -> skinning math or shader translation broken
  - 3D draws, zero/NaN vport  -> transform setup broken (CPU bone math)
"""
import collections
import struct
import sys

RB_MODECONTROL = 0x2208
RB_DEPTHCONTROL = 0x2200
PA_CL_VTE_CNTL = 0x2206
VPORT_XSCALE = 0x210F
VPORT_XOFFSET = 0x2110
VPORT_YSCALE = 0x2111
VPORT_YOFFSET = 0x2112
VPORT_ZSCALE = 0x2113
VPORT_ZOFFSET = 0x2114
SQ_PROGRAM_CNTL = 0x2180  # VS/PS sizes
RB_COLOR_MASK = 0x2104


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


def main():
    data = open(sys.argv[1], 'rb').read()
    pos = 48
    regs = {}
    states = collections.Counter()
    examples = {}
    draw_modes = collections.Counter()

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
                i += 1 + cnt
            elif pt == 3:
                op = (head >> 8) & 0x7F
                cnt = ((head >> 16) & 0x3FFF) + 1
                if op == 0x68 and cnt >= 2:  # SET_CONSTANT
                    off_type = words[i + 1]
                    typ = (off_type >> 16) & 0xFF
                    base = off_type & 0x7FF
                    bases = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908,
                             4: 0x2000, 5: 0x4904}
                    if typ in bases:
                        for k in range(cnt - 1):
                            regs[bases[typ] + base + k] = words[i + 2 + k]
                elif op in (0x22, 0x36):
                    mode = regs.get(RB_MODECONTROL, 0) & 7
                    draw_modes[mode] += 1
                    if mode == 4:
                        depth = regs.get(RB_DEPTHCONTROL, 0)
                        z_enable = (depth >> 1) & 1
                        # 3D draw if depth-test enabled (vs 2D FE quads)
                        if z_enable:
                            key = (
                                regs.get(VPORT_XSCALE, 0),
                                regs.get(VPORT_YSCALE, 0),
                                regs.get(VPORT_ZSCALE, 0),
                                regs.get(SQ_PROGRAM_CNTL, 0),
                                regs.get(RB_COLOR_MASK, 0),
                            )
                            states[key] += 1
                            if key not in examples:
                                examples[key] = dict(regs)
                i += 1 + cnt
            else:
                i += 1

    while pos + 4 <= len(data):
        (t,) = struct.unpack_from('<I', data, pos)
        if t > 11:
            break
        if t in (0, 2):
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
            pos += 20 + v[3]
        elif t == 8:
            v = struct.unpack_from('<III', data, pos)
            pos += 12 + v[2]
        elif t == 9:
            pos += 8
        elif t == 10:
            v = struct.unpack_from('<IIIIII', data, pos)
            pos += 24 + v[5]
        elif t == 11:
            v = struct.unpack_from('<IIII', data, pos)
            pos += 16 + v[3]

    print('all draw modes:', dict(draw_modes))
    print(f'{len(states)} distinct depth-tested 3D draw states, '
          f'{sum(states.values())} draws total')
    for key, c in states.most_common(12):
        xs, ys, zs, prog, mask = key
        ex = examples[key]
        print(f'-- x{c}  XSCALE={f32(xs):.2f} YSCALE={f32(ys):.2f} '
              f'ZSCALE={f32(zs):.4f} PROG={prog:08X} MASK={mask:04X} '
              f'XOFF={f32(ex.get(VPORT_XOFFSET,0)):.1f} '
              f'YOFF={f32(ex.get(VPORT_YOFFSET,0)):.1f}')


if __name__ == '__main__':
    main()
