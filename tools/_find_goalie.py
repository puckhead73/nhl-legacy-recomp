"""For each player uber-draw, decode slot1 (base jersey) mean color + flag any 2x2
stubs / magenta textures, to find the goalie (purple) and spot stubbed normal maps."""
import os,sys
sys.path.insert(0,'tools'); import highcut_packet_decode as D
build=sys.argv[1]
cnt=int(open(os.path.join(build,'highcut_frame.count')).read().split()[0])
for i in range(cnt):
    p=os.path.join(build,f'highcut_frame_{i}.bin')
    if not os.path.exists(p): continue
    buf=open(p,'rb').read(); h,hs,_=D.parse_header(buf)
    if not h: continue
    if not (h.get('vs_texture_count',0)>0 and h.get('texture_count',0)>=18 and h.get('surface_depth_base',0)==736): continue
    base=None; stubs=[]; bc5=[]
    seen=set()
    for d,blob in D.iter_textures(buf,h,hs):
        s=d['fetch_slot']
        if s in seen: continue
        seen.add(s)
        w,hh,fmt=d['width'],d['height'],d['tex_format']
        if w==2 and hh==2:
            # decode the 2x2 to see if magenta
            px=D.decode_bc(blob,2,2,fmt) if fmt in(1,2,3) else bytearray(blob[:16])
            col=(px[0],px[1],px[2]) if len(px)>=3 else (0,0,0)
            stubs.append(f"slot{s}={col}")
        if fmt==5: bc5.append(f"slot{s}({w}x{hh})")
        if s==1 and fmt in(0,1,2,3):
            rgba=bytearray(blob[:w*hh*4]) if fmt==0 else D.decode_bc(blob,w,hh,fmt)
            n=w*hh; sr=sum(rgba[0::4][:n])//n; sg=sum(rgba[1::4][:n])//n; sb=sum(rgba[2::4][:n])//n
            base=(sr,sg,sb)
    print(f"draw {i:4} base(slot1)={base}  BC5_normals={bc5 or 'NONE'}  2x2stubs={stubs or '-'}")
