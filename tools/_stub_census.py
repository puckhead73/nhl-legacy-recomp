"""Census of every STUBBED texture binding across a captured frame: read each binding's
real Xenos format/dimension from its fetch constant, detect whether untileBindings
stubbed it (packet desc = 2x2 RGBA8), and tally by (format,dim,stub-color)."""
import os,sys,struct
from collections import Counter
sys.path.insert(0,'tools'); import highcut_packet_decode as D
build=sys.argv[1]
cnt=int(open(os.path.join(build,'highcut_frame.count')).read().split()[0])
FMT={0:'k_8_8_8_8',2:'k_8',6:'k_8_8_8_8(6)',18:'k_DXT1',19:'k_DXT2_3',20:'k_DXT4_5',
     21:'k_16_16_16_16_EDRAM',22:'k_24_8(depth)',23:'k_24_8_FLOAT',24:'k_16',25:'k_16_16',
     26:'k_16_16_16_16',30:'k_16_FLOAT',31:'k_16_16_FLOAT',32:'k_16_16_16_16_FLOAT',
     33:'k_32',34:'k_32_32',35:'k_32_32_32_32',36:'k_32_FLOAT',37:'k_32_32_FLOAT',
     38:'k_32_32_32_32_FLOAT',41:'k_16_MPEG',42:'k_16_16_MPEG',49:'k_DXN(BC5)',
     50:'k_8_8_8_8_AS_16_16_16_16',54:'k_DXT1_AS_16_16_16_16'}
DIM={0:'1D',1:'2D',2:'3D',3:'cube'}
stub=Counter(); real=Counter(); stubcolor={}
for i in range(cnt):
    p=os.path.join(build,f'highcut_frame_{i}.bin')
    if not os.path.exists(p): continue
    buf=open(p,'rb').read(); h,hs,_=D.parse_header(buf)
    if not h: continue
    fetch=struct.unpack_from(f"<{h['fetch_bytes']//4}I",buf,hs)
    seen=set()
    for d,blob in D.iter_textures(buf,h,hs):
        s=d['fetch_slot']; key=(i,s)
        if key in seen: continue
        seen.add(key)
        if s*6+5>=len(fetch): continue
        d0,d1=fetch[s*6+0],fetch[s*6+1]
        fmt=d1&0x3F; dim=(d0>>9)&0x3
        name=f"{FMT.get(fmt,'fmt'+str(fmt))} dim={DIM.get(dim,dim)}"
        is_stub=(d['width']==2 and d['height']==2 and d['tex_format']==0)
        if is_stub:
            col=(blob[0],blob[1],blob[2]) if len(blob)>=3 else (0,0,0)
            tag='MAGENTA' if col==(255,0,255) else ('cube-neutral' if col==(32,32,32) else f'rgb{col}')
            stub[(name,tag)]+=1
        else:
            real[name]+=1
print("=== STILL STUBBED (real format -> what untileBindings can't handle) ===")
for (name,tag),n in stub.most_common():
    print(f"  {n:6}  {name:32} -> {tag}")
print("\n=== handled (real untiled, for contrast) ===")
for name,n in real.most_common(10):
    print(f"  {n:6}  {name}")
