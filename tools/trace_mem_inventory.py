#!/usr/bin/env python3
"""Inventory the memory ranges (reads/writes) + EDRAM snapshot in a .xtr trace.
Shows base, decoded length, encoding, and totals so we can see whether a
single-frame trace is self-contained (carries resident textures/buffers)."""
import struct
import sys

CMD = ['PrimaryBufferStart', 'PrimaryBufferEnd', 'IndirectBufferStart',
       'IndirectBufferEnd', 'PacketStart', 'PacketEnd', 'MemoryRead',
       'MemoryWrite', 'EdramSnapshot', 'Event', 'Registers', 'GammaRamp']


def main():
    data = open(sys.argv[1], 'rb').read()
    pos = 4 + 40 + 4
    reads = []
    writes = []
    edram = []
    while pos + 4 <= len(data):
        (t,) = struct.unpack_from('<I', data, pos)
        if t > 11:
            print(f'desync at 0x{pos:X}: type={t}')
            break
        name = CMD[t]
        if name in ('PrimaryBufferStart', 'IndirectBufferStart'):
            pos += 12
        elif name in ('PrimaryBufferEnd', 'IndirectBufferEnd', 'PacketEnd'):
            pos += 4
        elif name == 'PacketStart':
            _, base_ptr, count = struct.unpack_from('<III', data, pos)
            pos += 12 + count * 4
        elif name in ('MemoryRead', 'MemoryWrite'):
            _, base_ptr, enc, enc_len, dec_len = struct.unpack_from('<IIIII', data, pos)
            (reads if name == 'MemoryRead' else writes).append((base_ptr, dec_len, enc_len, enc))
            pos += 20 + enc_len
        elif name == 'EdramSnapshot':
            _, enc, enc_len = struct.unpack_from('<III', data, pos)
            edram.append((enc_len, enc))
            pos += 12 + enc_len
        elif name == 'Event':
            pos += 8
        elif name == 'Registers':
            _, first, cnt, exec_and_pad, enc, enc_len = struct.unpack_from('<IIIIII', data, pos)
            pos += 24 + enc_len
        elif name == 'GammaRamp':
            _, comp_pad, enc, enc_len = struct.unpack_from('<IIII', data, pos)
            pos += 16 + enc_len

    def dump(label, lst):
        total = sum(d for _, d, _, _ in lst)
        print(f'\n{label}: {len(lst)} ranges, {total} bytes decoded ({total/1024/1024:.2f} MB)')
        for base, dec, enc_len, enc in sorted(lst):
            print(f'  base=0x{base:08X}  decoded={dec:>10}  encoded={enc_len:>10}  enc={enc}')

    dump('MemoryRead', reads)
    dump('MemoryWrite', writes)
    print(f'\nEdramSnapshot: {len(edram)} -> {[(e, enc) for e, enc in edram]}')

    # Is the disclaimer frontbuffer present?
    fb = 0x1CF32000
    for base, dec, _, _ in reads + writes:
        if base <= fb < base + dec:
            print(f'\nFrontbuffer 0x{fb:08X} IS covered by range base=0x{base:08X} dec={dec}')
            break
    else:
        print(f'\nFrontbuffer 0x{fb:08X} NOT covered by any memory range')


if __name__ == '__main__':
    main()
