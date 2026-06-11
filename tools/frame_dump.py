#!/usr/bin/env python3
"""Dump the in-order PM4 structure of ONE frame of a .xtr stream: IB enter/exit
(base_ptr), draws (op + predicate bit), surface-info changes, viewport changes,
window-offset writes, and resolves (kCopy). Used to determine whether wide-RT
tiling is official (record-once / replay-per-tile) or manual."""
import struct, sys

CMD = ['PrimaryBufferStart','PrimaryBufferEnd','IndirectBufferStart','IndirectBufferEnd',
       'PacketStart','PacketEnd','MemoryRead','MemoryWrite','EdramSnapshot','Event','Registers','GammaRamp']
DRAW = {0x22:'DRAW_INDX',0x36:'DRAW_INDX_2',0x34:'DRAW_INDX_BIN',0x35:'DRAW_INDX_2_BIN'}
IB = {0x3F,0x37}
LOADCTX = 0x2E  # PM4_LOAD_CONSTANT_CONTEXT (reg load from memory)
LOADALU = 0x2F


def regwrites(head, body):
    pt = head >> 30
    if pt == 0:
        base = head & 0x7FFF; cnt = ((head >> 16) & 0x7FFF) + 1; one = (head >> 15) & 1
        for i in range(min(cnt, len(body))):
            yield (base if one else base + i), body[i]
    elif pt == 3 and ((head >> 8) & 0x7F) == 0x2D and body:
        info = body[0]; off = info & 0x7FF; ct = (info >> 16) & 0xFF
        if ct == 4:
            for i, v in enumerate(body[1:]):
                yield 0x2000 + off + i, v


def sp(v):  # surface_pitch + msaa from RB_SURFACE_INFO
    return v & 0x3FFF, (v >> 16) & 0x3


def main():
    path = sys.argv[1]; target = int(sys.argv[2])
    data = open(path, 'rb').read(); pos = 48
    frame = 0
    depth = 0
    last_sp = None; last_draw = None; draw_run = 0
    n_loadctx = 0; n_loadalu = 0
    out = []

    def flush_draws():
        nonlocal draw_run, last_draw
        if draw_run:
            out.append(f'    {draw_run}x {last_draw}')
            draw_run = 0; last_draw = None

    while pos + 4 <= len(data):
        (t,) = struct.unpack_from('<I', data, pos)
        if t > 11: break
        n = CMD[t]
        emit = (frame == target)
        if n in ('PrimaryBufferStart', 'IndirectBufferStart'):
            _, bp, c = struct.unpack_from('<III', data, pos); pos += 12
            if emit and n == 'IndirectBufferStart':
                flush_draws(); out.append(f'  IB_ENTER base=0x{bp:08X}')
        elif n in ('PrimaryBufferEnd', 'IndirectBufferEnd'):
            pos += 4
            if emit and n == 'IndirectBufferEnd':
                flush_draws(); out.append(f'  IB_EXIT')
        elif n == 'PacketEnd':
            pos += 4
        elif n == 'PacketStart':
            _, bp, c = struct.unpack_from('<III', data, pos); pos += 12
            w = struct.unpack_from('>%dI' % c, data, pos) if c else (); pos += c * 4
            if not w: continue
            head = w[0]; body = w[1:]; pt = head >> 30
            if pt == 3:
                op = (head >> 8) & 0x7F; pred = head & 1
                if op in DRAW:
                    if emit:
                        d = f'{DRAW[op]} pred={pred}'
                        if d == last_draw: draw_run += 1
                        else: flush_draws(); last_draw = d; draw_run = 1
                elif op in IB and body and emit:
                    flush_draws(); out.append(f'    IB_DISPATCH addr=0x{body[0]:08X}')
                elif op == LOADCTX and emit:
                    n_loadctx += 1
                    flush_draws(); out.append(f'    LOAD_CONSTANT_CONTEXT (reg load from mem) words={body[:4]}')
                elif op == LOADALU:
                    n_loadalu += 1
            for reg, val in regwrites(head, body):
                if not emit: continue
                if reg == 0x2000:
                    pitch, msaa = sp(val)
                    if (pitch, msaa) != last_sp:
                        flush_draws(); out.append(f'  RB_SURFACE_INFO pitch={pitch} msaa={msaa} (0x{val:08X})')
                        last_sp = (pitch, msaa)
                elif reg == 0x2080 and val != 0:
                    flush_draws(); out.append(f'  *** WINDOW_OFFSET = 0x{val:08X} ***')
                elif reg == 0x2208 and (val & 7) == 6:
                    flush_draws(); out.append(f'  RESOLVE (RB_MODECONTROL edram_mode=kCopy)')
        elif n in ('MemoryRead', 'MemoryWrite'):
            _, bp, e, el, dl = struct.unpack_from('<IIIII', data, pos); pos += 20 + el
        elif n == 'EdramSnapshot':
            _, e, el = struct.unpack_from('<III', data, pos); pos += 12 + el
        elif n == 'Event':
            (_, et) = struct.unpack_from('<II', data, pos); pos += 8
            if et == 0:
                if frame == target:
                    flush_draws()
                    out.append(f'  --- SWAP (end of frame {frame}) ---')
                frame += 1
                if frame > target: break
        elif n == 'Registers':
            _, f, c, xp, e, el = struct.unpack_from('<IIIIII', data, pos); pos += 24 + el
        elif n == 'GammaRamp':
            _, cp, e, el = struct.unpack_from('<IIII', data, pos); pos += 16 + el

    print(f'=== frame {target} structure ===')
    for line in out:
        print(line)
    print(f'\nLOAD_CONSTANT_CONTEXT in frame: {n_loadctx}, LOAD_ALU_CONSTANT: {n_loadalu}')


if __name__ == '__main__':
    main()
