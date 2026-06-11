#!/usr/bin/env python3
"""Parse a ReXGlue/Xenia .xtr GPU stream trace: per-command-type counts and
PM4 opcode histogram from kPacketStart payloads (the actual ring packets)."""
import collections
import struct
import sys

CMD = ['PrimaryBufferStart', 'PrimaryBufferEnd', 'IndirectBufferStart',
       'IndirectBufferEnd', 'PacketStart', 'PacketEnd', 'MemoryRead',
       'MemoryWrite', 'EdramSnapshot', 'Event', 'Registers', 'GammaRamp']

# PM4 type-3 opcode names we care about.
PM4 = {0x22: 'DRAW_INDX', 0x36: 'DRAW_INDX_2', 0x68: 'SET_CONSTANT',
       0x2F: 'LOAD_ALU_CONSTANT', 0x6D: 'SET_SHADER_CONSTANTS',
       0x46: 'EVENT_WRITE', 0x58: 'EVENT_WRITE_EXT', 0x10: 'NOP',
       0x3C: 'WAIT_REG_MEM', 0x44: 'COND_WRITE', 0x4A: 'XE_SWAP',
       0x48: 'ME_INIT', 0x45: 'REG_RMW', 0x37: 'INDIRECT_BUFFER',
       0x32: 'INVALIDATE_STATE'}

def main():
    data = open(sys.argv[1], 'rb').read()
    pos = 4 + 40 + 4  # TraceHeader
    cmd_counts = collections.Counter()
    pm4_counts = collections.Counter()
    type0_count = 0
    swap_events = 0
    truncated = False
    while pos + 4 <= len(data):
        (t,) = struct.unpack_from('<I', data, pos)
        if t > 11:
            print(f'desync at 0x{pos:X}: type={t}')
            truncated = True
            break
        name = CMD[t]
        cmd_counts[name] += 1
        if name in ('PrimaryBufferStart', 'IndirectBufferStart'):
            pos += 12
        elif name in ('PrimaryBufferEnd', 'IndirectBufferEnd', 'PacketEnd'):
            pos += 4
        elif name == 'PacketStart':
            _, base_ptr, count = struct.unpack_from('<III', data, pos)
            pos += 12
            if count >= 1:
                head = struct.unpack_from('>I', data, pos)[0]
                ptype = head >> 30
                if ptype == 3:
                    op = (head >> 8) & 0x7F
                    pm4_counts[PM4.get(op, f'op_{op:02X}')] += 1
                elif ptype == 0:
                    type0_count += 1
                else:
                    pm4_counts[f'type{ptype}'] += 1
            pos += count * 4
        elif name in ('MemoryRead', 'MemoryWrite'):
            _, base_ptr, enc, enc_len, dec_len = struct.unpack_from(
                '<IIIII', data, pos)
            pos += 20 + enc_len
        elif name == 'EdramSnapshot':
            _, enc, enc_len = struct.unpack_from('<III', data, pos)
            pos += 12 + enc_len
        elif name == 'Event':
            (_, et) = struct.unpack_from('<II', data, pos)
            if et == 0:
                swap_events += 1
            pos += 8
        elif name == 'Registers':
            # type(4) first(4) count(4) execute(bool->4? packed?) enc(4) len(4)
            # struct packing: uint32,uint32,uint32,bool(+3 pad),uint32,uint32
            _, first, cnt, exec_and_pad, enc, enc_len = struct.unpack_from(
                '<IIIIII', data, pos)
            pos += 24 + enc_len
        elif name == 'GammaRamp':
            # type(4) uint8(+3 pad) enc(4) len(4)
            _, comp_pad, enc, enc_len = struct.unpack_from('<IIII', data, pos)
            pos += 16 + enc_len
    print('command counts:', dict(cmd_counts))
    print('swap events:', swap_events)
    print('type-0 (register write) packets:', type0_count)
    print('PM4 type-3 opcodes:', dict(pm4_counts.most_common()))
    if truncated:
        print('(parse stopped early — struct layout guess may be off)')

if __name__ == '__main__':
    main()
