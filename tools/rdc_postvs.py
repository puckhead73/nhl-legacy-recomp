import renderdoc as rd
import os, struct, traceback

OUTDIR = r"e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
RDC = os.path.join(OUTDIR, "beta_takeover_capture.rdc")
RES = os.path.join(OUTDIR, "rdc_postvs.txt")

f = open(RES, "w")
def log(*a):
    f.write(" ".join(str(x) for x in a) + "\n"); f.flush()

def fmt_floats(data, stride, comps, n):
    out = []
    for v in range(n):
        base = v * stride
        if base + comps * 4 > len(data):
            break
        vals = struct.unpack_from("<" + "f" * comps, data, base)
        out.append("(" + ",".join(f"{x:.3f}" for x in vals) + ")")
    return out

try:
    cap = rd.OpenCaptureFile()
    log("OpenFile:", cap.OpenFile(RDC, '', None))
    st, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    log("OpenCapture:", st)

    def collect(actions, out):
        for a in actions:
            if a.flags & rd.ActionFlags.Drawcall:
                out.append(a)
            collect(a.children, out)
    draws = []
    collect(controller.GetRootActions(), draws)
    log("total draws:", len(draws))

    for i, d in enumerate(draws):
        # Identify textured draws by a bound pixel SRV; dump post-VS for those + neighbors.
        controller.SetFrameEvent(d.eventId, False)
        state = controller.GetPipelineState()
        ro = state.GetReadOnlyResources(rd.ShaderStage.Pixel)
        has_tex = any(True for _ in ro) and len(ro) > 0
        # numIndices on the action tells the draw vertex count
        nverts = d.numIndices
        try:
            pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        except Exception as e:
            log(f"draw {i}: GetPostVSData EXC {e!r}")
            continue
        vid = pv.vertexResourceId
        if vid == rd.ResourceId.Null():
            log(f"draw {i}: nverts={nverts} pixSRV={len(ro)} -> NO post-VS data (vertexResourceId null)")
            continue
        stride = pv.vertexByteStride
        # post-VS position is SV_Position float4 at the start; interpolators follow.
        data = controller.GetBufferData(vid, pv.vertexByteOffset, 0)
        ncomp = stride // 4
        positions = fmt_floats(data, stride, 4, 1)  # vtx0 SV_Position
        # full vtx0 (all components incl. interpolators)
        full = []
        if len(data) >= stride:
            full = list(struct.unpack_from("<" + "f" * ncomp, data, 0))
        log(f"draw {i}: nverts={nverts} pixSRV={len(ro)} stride={stride} topo={pv.topology} "
            f"POS={positions} vtx0_all=[" + ",".join(f"{x:.3f}" for x in full) + "]")

    controller.Shutdown(); cap.Shutdown()
    log("DONE")
except Exception as e:
    log("EXCEPTION:", repr(e)); log(traceback.format_exc())
finally:
    f.close()
