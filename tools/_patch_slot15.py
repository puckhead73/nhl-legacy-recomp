"""Overwrite every player draw's slot15 (the magenta-stubbed k_24_8 depth/shadow map)
2x2 blob with a neutral value, to test if it's the source of the equipment speckle.
Usage: python tools/_patch_slot15.py <build_dir> <slot> <r> <g> <b>"""
import os,sys,struct
sys.path.insert(0,'tools'); import highcut_packet_decode as D
build,slot,r,g,b=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4]),int(sys.argv[5])
cnt=int(open(os.path.join(build,'highcut_frame.count')).read().split()[0])
patched=0
for i in range(cnt):
    p=os.path.join(build,f'highcut_frame_{i}.bin')
    if not os.path.exists(p): continue
    buf=bytearray(open(p,'rb').read()); h,hs,_=D.parse_header(buf)
    if not h: continue
    # walk textures tracking byte offset (mirror iter_textures)
    if h['version']>=10: desc_sz=struct.calcsize(D.TEX_DESC_V10); fields=D.TEX_FIELDS_V10; descfmt=D.TEX_DESC_V10
    else: continue
    off=(hs+h['fetch_bytes']+h['sys_bytes']+h['shared_bytes']+h['bool_bytes']+h['vs_float_bytes']
         +h['ps_float_bytes']+h['vs_spirv_bytes']+h['ps_spirv_bytes'])
    changed=False
    for _ in range(h['texture_count']):
        d=dict(zip(fields,struct.unpack_from(descfmt,buf,off))); off+=desc_sz
        blob_off=off; off+=d['data_bytes']
        if d['fetch_slot']==slot and d['width']==2 and d['height']==2 and d['data_bytes']>=16:
            for px in range(0,16,4):
                buf[blob_off+px]=r; buf[blob_off+px+1]=g; buf[blob_off+px+2]=b; buf[blob_off+px+3]=255
            changed=True
    if changed:
        open(p,'wb').write(buf); patched+=1
print(f"patched slot{slot} -> ({r},{g},{b}) in {patched} draws")
