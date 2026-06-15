"""Fill a slot's ENTIRE texture blob with a constant RGBA (handles multi-face cubes),
to test its contribution. Usage: _patch_texfill.py <packet.bin> <slot> <r> <g> <b> <a>"""
import sys,struct
sys.path.insert(0,'tools'); import highcut_packet_decode as D
p,slot,r,g,b,a=sys.argv[1],int(sys.argv[2]),*[int(x) for x in sys.argv[3:7]]
buf=bytearray(open(p,'rb').read()); h,hs,_=D.parse_header(buf)
off=(hs+h['fetch_bytes']+h['sys_bytes']+h['shared_bytes']+h['bool_bytes']+h['vs_float_bytes']
     +h['ps_float_bytes']+h['vs_spirv_bytes']+h['ps_spirv_bytes'])
desc_sz=struct.calcsize(D.TEX_DESC_V10)
done=0
for _ in range(h['texture_count']):
    d=dict(zip(D.TEX_FIELDS_V10,struct.unpack_from(D.TEX_DESC_V10,buf,off))); off+=desc_sz
    bo=off; off+=d['data_bytes']
    if d['fetch_slot']==slot and d['tex_format']==0:  # RGBA8 (stub)
        for i in range(0,d['data_bytes'],4):
            buf[bo+i]=r; buf[bo+i+1]=g; buf[bo+i+2]=b; buf[bo+i+3]=a
        done+=1
if done: open(p,'wb').write(buf)
print(f"slot{slot}: filled {done} blob(s) -> ({r},{g},{b},{a})")
