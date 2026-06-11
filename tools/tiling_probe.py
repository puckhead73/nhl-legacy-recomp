#!/usr/bin/env python3
"""Investigate whether NHL Legacy's wide-RT tiling is OFFICIAL D3D9 predicated
tiling (runtime records geometry once, replays per tile via repeated
INDIRECT_BUFFER, using BIN draws / predication) or MANUAL (game code sets
PA_SC_WINDOW_OFFSET itself and re-issues draws inline per tile).

Walks a .xtr stream, tracks frame index (swap events) and the buffer context
stack (Primary vs Indirect, with base_ptr). Decodes PM4 type-0 + SET_CONSTANT
register writes to find PA_SC_WINDOW_OFFSET (ctx reg 0x2080) writes, classifies
the window x/y offset, and reports the local structure around any nonzero
window-offset pass: enclosing IB base_ptr, surrounding draws (op + predicate
bit), and whether the same IB is re-dispatched.
"""
import collections
import struct
import sys

CMD = ['PrimaryBufferStart', 'PrimaryBufferEnd', 'IndirectBufferStart',
       'IndirectBufferEnd', 'PacketStart', 'PacketEnd', 'MemoryRead',
       'MemoryWrite', 'EdramSnapshot', 'Event', 'Registers', 'GammaRamp']

DRAW_OPS = {0x22: 'DRAW_INDX', 0x36: 'DRAW_INDX_2',
            0x34: 'DRAW_INDX_BIN', 0x35: 'DRAW_INDX_2_BIN'}
IB_OPS = {0x3F, 0x37}
WINDOW_OFFSET_REG = 0x2080   # PA_SC_WINDOW_OFFSET (context reg)
COPY_CONTROL_REG = 0x2318    # RB_COPY_CONTROL (resolve trigger region)
MODECONTROL_REG = 0x2208     # RB_MODECONTROL (edram_mode in low bits)


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode_winoff(val):
    x = sx16(val & 0xFFFF)
    y = sx16((val >> 16) & 0xFFFF)
    return x, y


def reg_writes_from_packet(head, body):
    """Yield (reg_index, value) for a type-0 or SET_CONSTANT packet."""
    ptype = head >> 30
    if ptype == 0:
        # type-0: base in low 15 bits, count = ((head>>16)&0x7FFF)+1,
        # write_one bit 15 of head selects single-reg mode.
        base = head & 0x7FFF
        cnt = ((head >> 16) & 0x7FFF) + 1
        one = (head >> 15) & 1
        for i in range(min(cnt, len(body))):
            yield (base if one else base + i), body[i]
    elif ptype == 3:
        op = (head >> 8) & 0x7F
        if op == 0x2D and body:  # PM4_SET_CONSTANT
            info = body[0]
            offset = info & 0x7FF
            ctype = (info >> 16) & 0xFF
            if ctype == 4:  # context registers block
                for i, v in enumerate(body[1:]):
                    yield 0x2000 + offset + i, v


def main():
    path = sys.argv[1]
    target_frame = int(sys.argv[2]) if len(sys.argv) > 2 else None
    data = open(path, 'rb').read()
    pos = 4 + 40 + 4

    frame = 0
    # stack of (kind, base_ptr) ; kind 'P' primary, 'I' indirect
    bufstack = []
    winoff_hist = collections.Counter()
    # per-frame event log of interesting tokens
    events = []  # (frame, kind, detail)
    ib_dispatch_seq = []  # (frame, base_ptr) of INDIRECT_BUFFER dispatches
    cur_winoff = (0, 0)
    draw_pred_counts = collections.Counter()  # predicate bit usage on draws
    bin_draws = 0
    frames_with_neg_winoff = set()

    while pos + 4 <= len(data):
        (t,) = struct.unpack_from('<I', data, pos)
        if t > 11:
            break
        name = CMD[t]
        if name == 'PrimaryBufferStart':
            _, base_ptr, count = struct.unpack_from('<III', data, pos)
            bufstack.append(('P', base_ptr))
            pos += 12
        elif name == 'IndirectBufferStart':
            _, base_ptr, count = struct.unpack_from('<III', data, pos)
            bufstack.append(('I', base_ptr))
            ib_dispatch_seq.append((frame, base_ptr))
            if target_frame is None or frame == target_frame:
                events.append((frame, 'IB_ENTER', f'base=0x{base_ptr:08X}'))
            pos += 12
        elif name in ('PrimaryBufferEnd', 'IndirectBufferEnd'):
            if bufstack:
                k, bp = bufstack.pop()
                if name == 'IndirectBufferEnd' and (target_frame is None or frame == target_frame):
                    events.append((frame, 'IB_EXIT', f'base=0x{bp:08X}'))
            pos += 4
        elif name == 'PacketEnd':
            pos += 4
        elif name == 'PacketStart':
            _, base_ptr, count = struct.unpack_from('<III', data, pos)
            pos += 12
            words = struct.unpack_from('>%dI' % count, data, pos) if count else ()
            pos += count * 4
            if not words:
                continue
            head = words[0]
            body = words[1:]
            ptype = head >> 30
            if ptype == 3:
                op = (head >> 8) & 0x7F
                pred = head & 1
                if op in DRAW_OPS:
                    draw_pred_counts[(DRAW_OPS[op], pred)] += 1
                    if op in (0x34, 0x35):
                        bin_draws += 1
                    if (target_frame is None or frame == target_frame):
                        enc = bufstack[-1] if bufstack else ('?', 0)
                        events.append((frame, 'DRAW',
                                       f'{DRAW_OPS[op]} pred={pred} in {enc[0]}:0x{enc[1]:08X} winoff={cur_winoff}'))
                if op in IB_OPS and body and (target_frame is None or frame == target_frame):
                    events.append((frame, 'IB_DISPATCH', f'addr=0x{body[0]:08X}'))
            # register writes (type-0 or SET_CONSTANT)
            for reg, val in reg_writes_from_packet(head, body):
                if reg == WINDOW_OFFSET_REG:
                    xy = decode_winoff(val)
                    cur_winoff = xy
                    winoff_hist[xy] += 1
                    if xy[0] != 0 or xy[1] != 0:
                        frames_with_neg_winoff.add(frame)
                    if (target_frame is None or frame == target_frame):
                        enc = bufstack[-1] if bufstack else ('?', 0)
                        events.append((frame, 'WINOFF',
                                       f'x={xy[0]} y={xy[1]} (val=0x{val:08X}) in {enc[0]}:0x{enc[1]:08X}'))
                elif reg == MODECONTROL_REG:
                    mode = val & 0x7
                    if mode == 6 and (target_frame is None or frame == target_frame):
                        enc = bufstack[-1] if bufstack else ('?', 0)
                        events.append((frame, 'RESOLVE(edram_mode=kCopy)',
                                       f'val=0x{val:08X} in {enc[0]}:0x{enc[1]:08X}'))
        elif name in ('MemoryRead', 'MemoryWrite'):
            _, base_ptr, enc, enc_len, dec_len = struct.unpack_from('<IIIII', data, pos)
            pos += 20 + enc_len
        elif name == 'EdramSnapshot':
            _, enc, enc_len = struct.unpack_from('<III', data, pos)
            pos += 12 + enc_len
        elif name == 'Event':
            (_, et) = struct.unpack_from('<II', data, pos)
            pos += 8
            if et == 0:
                frame += 1
        elif name == 'Registers':
            _, first, cnt, exec_and_pad, enc, enc_len = struct.unpack_from('<IIIIII', data, pos)
            pos += 24 + enc_len
        elif name == 'GammaRamp':
            _, comp_pad, enc, enc_len = struct.unpack_from('<IIII', data, pos)
            pos += 16 + enc_len

    print('=== window-offset (PA_SC_WINDOW_OFFSET) value histogram (all frames) ===')
    for xy, n in winoff_hist.most_common():
        print(f'  x={xy[0]:>6} y={xy[1]:>6} : {n}')
    print(f'\nframes with a NONZERO window offset: {len(frames_with_neg_winoff)} '
          f'(of {frame}) -> {sorted(frames_with_neg_winoff)[:40]}')
    print(f'\nBIN draws (DRAW_INDX_BIN/2_BIN) total: {bin_draws}')
    print('draw predicate-bit usage (op, predicate_bit) -> count:')
    for k, n in draw_pred_counts.most_common():
        print(f'  {k}: {n}')

    if target_frame is not None:
        print(f'\n=== structural event log for frame {target_frame} '
              f'(WINOFF / IB / DRAW / RESOLVE only) ===')
        # only print a focused slice: from first nonzero winoff-context onward
        printed = 0
        for fr, kind, detail in events:
            if kind in ('WINOFF', 'IB_DISPATCH', 'RESOLVE(edram_mode=kCopy)') or kind.startswith('RESOLVE'):
                print(f'  [{kind}] {detail}')
                printed += 1
            if printed > 400:
                print('  ... (truncated)')
                break


if __name__ == '__main__':
    main()
