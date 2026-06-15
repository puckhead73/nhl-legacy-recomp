import os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import highcut_packet_decode as D
path = sys.argv[1]
buf = open(path, "rb").read()
h, hs, ver = D.parse_header(buf)
off = hs + h["fetch_bytes"] + h["sys_bytes"] + h["shared_bytes"] + h["bool_bytes"] + h["vs_float_bytes"] + h["ps_float_bytes"]
vs = buf[off:off+h["vs_spirv_bytes"]]; off += h["vs_spirv_bytes"]
ps = buf[off:off+h["ps_spirv_bytes"]]
base = os.path.splitext(path)[0]
open(base+".vs.spv","wb").write(vs)
open(base+".ps.spv","wb").write(ps)
print(f"vs={len(vs)}B ps={len(ps)}B -> {base}.{{vs,ps}}.spv")
