#!/usr/bin/env python3
"""Track GPU register state through a .xtr stream trace and classify draws:
RB_MODECONTROL edram_mode (4=ColorDepth render, 6=Copy/resolve) and, for
copy draws, RB_COPY_DEST_BASE — does anything resolve to the frontbuffer?"""
import collections
import struct
import sys

RB_MODECONTROL = 0x2208
RB_COPY_CONTROL = 0x2318
RB_COPY_DEST_BASE = 0x2319
RB_COPY_DEST_PITCH = 0x231A
RB_COPY_DEST_INFO = 0x231B
RB_SURFACE_INFO = 0x2000
RB_COLOR_INFO = 0x2001
RB_DEPTH_INFO = 0x2002
PA_SC_WINDOW_SCISSOR_BR = 0x2092

def main():
    data = open(sys.argv[1], 'rb').read()
    pos = 48
    regs = {}
    draw_modes = collections.Counter()
    copy_dests = collections.Counter()
    render_targets = collections.Counter()
    scissors = collections.Counter()
    first_copies = []

    def handle_packet(words):
        nonlocal regs
        i = 0
        n = len(words)
        while i < n:
            head = words[i]
            ptype = head >> 30
            if ptype == 0:
                base = head & 0x7FFF
                cnt = ((head >> 16) & 0x3FFF) + 1
                one = (head >> 15) & 1
                for k in range(cnt):
                    if i + 1 + k >= n:
                        break
                    regs[base + (0 if one else k)] = words[i + 1 + k]
                i += 1 + cnt
            elif ptype == 3:
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
                    mode = regs.get(RB_MODECONTROL, 0) & 0x7
                    draw_modes[mode] += 1
                    if mode == 6:
                        dest = regs.get(RB_COPY_DEST_BASE, 0)
                        copy_dests[dest] += 1
                        if len(first_copies) < 5:
                            first_copies.append({
                                'dest': dest,
                                'copy_control': regs.get(RB_COPY_CONTROL, 0),
                                'dest_pitch': regs.get(RB_COPY_DEST_PITCH, 0),
                                'dest_info': regs.get(RB_COPY_DEST_INFO, 0),
                            })
                    elif mode == 4:
                        render_targets[regs.get(RB_COLOR_INFO, 0)] += 1
                        scissors[regs.get(PA_SC_WINDOW_SCISSOR_BR, 0)] += 1
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
            _, base_ptr, count = struct.unpack_from('<III', data, pos)
            pos += 12
            words = struct.unpack_from(f'>{count}I', data, pos)
            handle_packet(words)
            pos += count * 4
        elif t in (6, 7):
            vals = struct.unpack_from('<IIIII', data, pos)
            pos += 20 + vals[3]
        elif t == 8:
            vals = struct.unpack_from('<III', data, pos)
            pos += 12 + vals[2]
        elif t == 9:
            pos += 8
        elif t == 10:
            vals = struct.unpack_from('<IIIIII', data, pos)
            pos += 24 + vals[5]
        elif t == 11:
            vals = struct.unpack_from('<IIII', data, pos)
            pos += 16 + vals[3]

    print('draw modes (RB_MODECONTROL&7):', dict(draw_modes))
    print('copy destinations:', {f'{k:08X}': v for k, v in
                                  copy_dests.most_common(10)})
    print('render RB_COLOR_INFO values:', {f'{k:08X}': v for k, v in
                                            render_targets.most_common(8)})
    print('render scissor BR values:', {f'{k:08X}': v for k, v in
                                         scissors.most_common(8)})
    print('first copy draws:')
    for c in first_copies:
        print('  ', {k: f'{v:08X}' for k, v in c.items()})

if __name__ == '__main__':
    main()
