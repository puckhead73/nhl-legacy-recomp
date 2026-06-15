"""Crop + nearest-neighbor upscale a region of a PNG for inspection.
Usage: python tools/_crop.py <in.png> <out.png> <x> <y> <w> <h> <scale>"""
import sys,struct,zlib
def read_png(p):
    d=open(p,'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
    pos=8; W=H=0; idat=b''
    while pos<len(d):
        ln=struct.unpack('>I',d[pos:pos+4])[0]; typ=d[pos+4:pos+8]; data=d[pos+8:pos+8+ln]; pos+=12+ln
        if typ==b'IHDR': W,H=struct.unpack('>II',data[:8])
        elif typ==b'IDAT': idat+=data
        elif typ==b'IEND': break
    raw=zlib.decompress(idat); out=bytearray(); stride=W*4
    prev=bytearray(stride)
    for y in range(H):
        f=raw[y*(stride+1)]; line=bytearray(raw[y*(stride+1)+1:y*(stride+1)+1+stride])
        for i in range(stride):
            a=line[i-4] if i>=4 else 0; b=prev[i]; c=prev[i-4] if i>=4 else 0; x=line[i]
            if f==1: line[i]=(x+a)&255
            elif f==2: line[i]=(x+b)&255
            elif f==3: line[i]=(x+((a+b)>>1))&255
            elif f==4:
                p=a+b-c; pa=abs(p-a); pb=abs(p-b); pc=abs(p-c)
                pr=a if(pa<=pb and pa<=pc) else (b if pb<=pc else c); line[i]=(x+pr)&255
        out+=line; prev=line
    return W,H,out
def write_png(p,W,H,rgba):
    raw=bytearray()
    for y in range(H): raw.append(0); raw+=rgba[y*W*4:(y+1)*W*4]
    def ch(t,d): c=t+d; return struct.pack('>I',len(d))+c+struct.pack('>I',zlib.crc32(c)&0xffffffff)
    png=b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',W,H,8,6,0,0,0))+ch(b'IDAT',zlib.compress(bytes(raw),9))+ch(b'IEND',b'')
    open(p,'wb').write(png)
inp,outp,x,y,w,h,s=sys.argv[1],sys.argv[2],*map(int,sys.argv[3:8])
W,H,px=read_png(inp)
ow,oh=w*s,h*s; out=bytearray(ow*oh*4)
for j in range(oh):
    for i in range(ow):
        sx=x+i//s; sy=y+j//s
        if 0<=sx<W and 0<=sy<H:
            o=(j*ow+i)*4; q=(sy*W+sx)*4; out[o:o+4]=px[q:q+4]
write_png(outp,ow,oh,out); print(f"{outp} {ow}x{oh}")
