import renderdoc as rd
import os, traceback

OUTDIR = r"e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
RDC = os.path.join(OUTDIR, "beta_takeover_capture.rdc")
RES = os.path.join(OUTDIR, "rdc_graph.txt")

f = open(RES, "w")
def log(*a):
    f.write(" ".join(str(x) for x in a) + "\n"); f.flush()

try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(RDC, '', None)
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
    textures = {t.resourceId: t for t in controller.GetTextures()}

    def rid_of(x):
        r = getattr(x, 'resource', None)
        if r is None: r = getattr(x, 'resourceId', None)
        return r

    for i in range(len(draws)):
        a = draws[i]
        controller.SetFrameEvent(a.eventId, False)
        ps = controller.GetPipelineState()
        # output RT
        rts = ps.GetOutputTargets()
        rtdesc = "none"
        if rts:
            rid = rid_of(rts[0])
            if rid and rid != rd.ResourceId.Null():
                td = textures.get(rid)
                rtdesc = f"{int(rid)}({td.width}x{td.height})" if td else f"{int(rid)}"
        # pixel SRVs
        ro = ps.GetReadOnlyResources(rd.ShaderStage.Pixel)
        srvs = []
        for r in ro:
            rid = rid_of(r.descriptor)
            if rid and rid != rd.ResourceId.Null():
                td = textures.get(rid)
                srvs.append(f"{int(rid)}({td.width}x{td.height})" if td else f"{int(rid)}")
        # blend
        try:
            b = ps.GetColorBlends()[0]
            blend = f"bl{int(b.enabled)}:{b.colorBlend.source}/{b.colorBlend.destination}:wm{b.writeMask}"
        except Exception:
            blend = "bl?"
        log(f"d{i:02d} ev{a.eventId} v{a.numIndices} RT={rtdesc} {blend} "
            f"PSsrv=[{','.join(srvs) if srvs else '-'}]")

    controller.Shutdown(); cap.Shutdown()
    log("DONE")
except Exception as e:
    log("EXCEPTION:", repr(e)); log(traceback.format_exc())
finally:
    f.close()
