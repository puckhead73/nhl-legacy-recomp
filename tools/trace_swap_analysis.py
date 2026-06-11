#!/usr/bin/env python3
"""From a .xtr stream trace: dump XE_SWAP (op 0x64) packet payloads
(frontbuffer ptr) and the histogram of GPU MemoryWrite destinations,
to check whether anything ever writes the presented frontbuffer."""
import collections
import struct
import sys

def main():
    data = open(sys.argv[1], 'rb').read()
    pos = 48
    swap_payloads = []
    memwrites = collections.Counter()
    draw_count = 0
    while pos + 4 <= len(data):
        (t,) = struct.unpack_from('<I', data, pos)
        if t > 11:
            break
        if t in (0, 2):          # Primary/IndirectBufferStart
            pos += 12
        elif t in (1, 3, 5):     # ends
            pos += 4
        elif t == 4:             # PacketStart
            _, base_ptr, count = struct.unpack_from('<III', data, pos)
            pos += 12
            if count >= 1:
                head = struct.unpack_from('>I', data, pos)[0]
                if (head >> 30) == 3:
                    op = (head >> 8) & 0x7F
                    if op == 0x64 and len(swap_payloads) < 4:
                        words = struct.unpack_from(f'>{count}I', data, pos)
                        swap_payloads.append(words)
                    elif op == 0x36:
                        draw_count += 1
            pos += count * 4
        elif t in (6, 7):        # MemoryRead/Write
            _, base_ptr, enc, enc_len, dec_len = struct.unpack_from(
                '<IIIII', data, pos)
            if t == 7:
                memwrites[(base_ptr, dec_len)] += 1
            pos += 20 + enc_len
        elif t == 8:             # EdramSnapshot
            _, enc, enc_len = struct.unpack_from('<III', data, pos)
            pos += 12 + enc_len
        elif t == 9:             # Event
            pos += 8
        elif t == 10:            # Registers
            vals = struct.unpack_from('<IIIIII', data, pos)
            pos += 24 + vals[5]
        elif t == 11:            # GammaRamp
            vals = struct.unpack_from('<IIII', data, pos)
            pos += 16 + vals[3]
    print('draws:', draw_count)
    print('first XE_SWAP packets:')
    for w in swap_payloads:
        print('  ', ' '.join(f'{x:08X}' for x in w))
    print('GPU memory write destinations (base, len) x count:')
    for (bp, ln), c in memwrites.most_common(20):
        print(f'  {bp:08X} len={ln:7d} x{c}')

if __name__ == '__main__':
    main()
