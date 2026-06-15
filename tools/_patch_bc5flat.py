"""Overwrite a slot's BC5 (tex_format=5) blob with FLAT blocks (R=G=128 => neutral
'up' normal), to test if a noisy normal map is the artifact source.
Usage: _patch_bc5flat.py <packet.bin> <slot>"""
import sys,struct
sys.path.insert(0,'tools'); import highcut_packet_decode as D
p,slot=sys.argv[1],int(sys.argv[2])
buf=bytearray(open(p,'rb').read()); h,hs,_=D.parse_header(buf)
off=(hs+h['fetch_bytes']+h['sys_bytes']+h['shared_bytes']+h['bool_bytes']+h['vs_float_bytes']
     +h['ps_float_bytes']+h['vs_spirv_bytes']+h['ps_spirv_bytes'])
desc_sz=struct.calcsize(D.TEX_DESC_V10)
# flat BC4 block: endpoints 128,128 then 6 zero index bytes; BC5 = two BC4 blocks
FLAT=bytes([128,128,0,0,0,0,0,0, 128,128,0,0,0,0,0,0])
done=0
for _ in range(h['texture_count']):
    d=dict(zip(D.TEX_FIELDS_V10,struct.unpack_from(D.TEX_DESC_V10,buf,off))); off+=desc_sz
    bo=off; off+=d['data_bytes']
    if d['fetch_slot']==slot and d['tex_format']==5:
        for i in range(0,d['data_bytes'],16): buf[bo+i:bo+i+16]=FLAT
        done+=1
if done: open(p,'wb').write(buf)
print(f"slot{slot}: flattened {done} BC5 normal blob(s)")
