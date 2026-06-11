#!/usr/bin/env python3
"""Inspect the register snapshot block(s) in a .xtr trace and report whether key
render-state registers (viewport, scissor, RB_COLOR_INFO, RB_MODECONTROL) are
covered and their restored values. Also reports execute_callbacks."""
import struct
import sys

try:
    import snappy
    HAVE_SNAPPY = True
except ImportError:
    HAVE_SNAPPY = False

CMD = ['PrimaryBufferStart', 'PrimaryBufferEnd', 'IndirectBufferStart',
       'IndirectBufferEnd', 'PacketStart', 'PacketEnd', 'MemoryRead',
       'MemoryWrite', 'EdramSnapshot', 'Event', 'Registers', 'GammaRamp']

# Key Xenos registers (word indices).
KEY = {
    0x2000: 'RB_SURFACE_INFO',
    0x2001: 'RB_COLOR_INFO',
    0x2010: 'RB_DEPTH_INFO',
    0x2208: 'RB_MODECONTROL',
    0x200E: 'PA_SC_WINDOW_SCISSOR_TL',
    0x200F: 'PA_SC_WINDOW_SCISSOR_BR',
    0x2080: 'PA_SC_WINDOW_OFFSET',
    0x210F: 'PA_CL_VPORT_XSCALE',
    0x2110: 'PA_CL_VPORT_XOFFSET',
    0x2111: 'PA_CL_VPORT_YSCALE',
    0x2112: 'PA_CL_VPORT_YOFFSET',
    0x2113: 'PA_CL_VPORT_ZSCALE',
    0x2114: 'PA_CL_VPORT_ZOFFSET',
    0x2300: 'RB_BLENDCONTROL0',
    0x2200: 'RB_DEPTHCONTROL',
    0x2205: 'RB_COLORCONTROL',
    0x2104: 'PA_SU_SC_MODE_CNTL',
}


def main():
    data = open(sys.argv[1], 'rb').read()
    pos = 4 + 40 + 4
    blocks = []
    while pos + 4 <= len(data):
        (t,) = struct.unpack_from('<I', data, pos)
        if t > 11:
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
            pos += 20 + enc_len
        elif name == 'EdramSnapshot':
            _, enc, enc_len = struct.unpack_from('<III', data, pos)
            pos += 12 + enc_len
        elif name == 'Event':
            pos += 8
        elif name == 'Registers':
            _, first, cnt, exec_and_pad, enc, enc_len = struct.unpack_from('<IIIIII', data, pos)
            payload = data[pos + 24: pos + 24 + enc_len]
            blocks.append((first, cnt, exec_and_pad & 0xFF, enc, enc_len, payload))
            pos += 24 + enc_len
        elif name == 'GammaRamp':
            _, comp_pad, enc, enc_len = struct.unpack_from('<IIII', data, pos)
            pos += 16 + enc_len

    print(f'{len(blocks)} register block(s)')
    for i, (first, cnt, exe, enc, enc_len, payload) in enumerate(blocks):
        print(f'\nBlock {i}: first=0x{first:04X} count={cnt} (0x{first:04X}..0x{first+cnt-1:04X}) '
              f'execute_callbacks={exe} enc={enc} enc_len={enc_len}')
        vals = None
        if enc == 0:
            vals = list(struct.unpack_from('<%dI' % cnt, payload, 0))
        elif HAVE_SNAPPY:
            raw = snappy.uncompress(payload)
            vals = list(struct.unpack_from('<%dI' % cnt, raw, 0))
        else:
            print('  (snappy not available; cannot decode values)')
        if vals is not None:
            for reg, nm in sorted(KEY.items()):
                if first <= reg < first + cnt:
                    print(f'    0x{reg:04X} {nm:28} = 0x{vals[reg-first]:08X}')


if __name__ == '__main__':
    main()
