"""Per goalie draw: render state (cwm/blend/depth/cull) + each texture's REAL exp_adjust
(dword_3 bits13-18) to spot the helmet over-bright (high exp) and the invisible glove
(color-mask off / depth-only / alpha blend)."""
import os,sys,struct
sys.path.insert(0,'tools'); import highcut_packet_decode as D
build=sys.argv[1]; draws=[int(x) for x in sys.argv[2:]]
def sext(v,p,b):
    x=(v>>p)&((1<<b)-1); return x-(1<<b) if x>>(b-1) else x
for i in draws:
    p=os.path.join(build,f'highcut_frame_{i}.bin')
    if not os.path.exists(p): continue
    buf=open(p,'rb').read(); h,hs,_=D.parse_header(buf)
    if not h: continue
    fetch=struct.unpack_from(f"<{h['fetch_bytes']//4}I",buf,hs)
    exps=[]
    seen=set()
    for d,blob in D.iter_textures(buf,h,hs):
        s=d['fetch_slot']
        if s in seen: continue
        seen.add(s)
        e=sext(fetch[s*6+3],13,6)
        if e!=0: exps.append(f"slot{s}={e}(x{2.0**e:.2g})")
    dw=f"DW" if h.get('depth_write') else "d-"
    de="EN" if h.get('depth_enable') else "--"
    bl=f"blend(s{h['blend_src']}/d{h['blend_dst']}/o{h['blend_op']})" if h.get('blend_enable') else "noblend"
    print(f"draw {i:4} verts={h['vertex_count']:5} cwm=0x{h['color_write_mask']:X} {bl:22} depth={de}{dw}/{D.CMP.get(h['depth_func'],'?')} cull={D.CULL.get(h['cull_mode'],'?')} ps_tex={h['texture_count']:2}  exp_adjust!=0: {exps or 'none'}")
