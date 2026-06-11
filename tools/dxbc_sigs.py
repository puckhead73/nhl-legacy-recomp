import struct, sys

def parse(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'DXBC', d[:4]
    nchunks = struct.unpack_from('<I', d, 28)[0]
    offs = struct.unpack_from('<%dI' % nchunks, d, 32)
    print('  chunks:', [d[o:o+4].decode('ascii','replace') for o in offs])
    for o in offs:
        fourcc = d[o:o+4]
        size = struct.unpack_from('<I', d, o+4)[0]
        if fourcc not in (b'ISGN', b'OSGN', b'ISG1', b'OSG1'):
            continue
        body = d[o+8:o+8+size]
        cnt, ptbl = struct.unpack_from('<II', body, 0)
        print('  %s count=%d' % (fourcc.decode(), cnt))
        # ISGN/OSGN elements are fixed 24 bytes; ISG1/OSG1 add a 4-byte stream prefix.
        stride = 28 if fourcc in (b'ISG1', b'OSG1') else 24
        for i in range(cnt):
            base = 8 + i*stride
            vals = struct.unpack_from('<7I', body, base) if base+28 <= len(body) else None
            # ISGN element: Name(4) Index(4) SysVal(4) Type(4) Reg(4) Mask(1)+RW(1)+pad(2)
            # ISG1 prepends Stream(4).
            extra = 4 if fourcc in (b'ISG1', b'OSG1') else 0
            nameoff, semidx, sysval, comptype, reg = struct.unpack_from('<5I', body, base+extra)
            masks = struct.unpack_from('<I', body, base+extra+20)[0]
            nstart = o + 8 + nameoff
            nend = d.index(b'\x00', nstart) if nstart < len(d) and b'\x00' in d[nstart:] else nstart
            name = d[nstart:nend].decode('ascii', 'replace')
            print('     name=%-14s idx=%d reg=%d mask=0x%X rw=0x%X (nameoff=%d stride=%d)' %
                  (name, semidx, reg, masks & 0xFF, (masks >> 8) & 0xFF, nameoff, stride))

for label, path in [('VS', sys.argv[1]), ('PS', sys.argv[2])]:
    print('====', label, path)
    parse(path)
