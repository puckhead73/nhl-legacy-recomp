import renderdoc as rd
import os, traceback

OUTDIR = r"e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
RDC = os.path.join(OUTDIR, "beta_takeover_capture.rdc")
RES = os.path.join(OUTDIR, "rdc_results.txt")

f = open(RES, "w")
def log(*a):
    f.write(" ".join(str(x) for x in a) + "\n"); f.flush()

def resid_of(obj):
    # 1.44: UsedDescriptor.descriptor.resource ; older: BoundResource.resourceId
    for path in (("resourceId",), ("resource",), ("descriptor", "resource")):
        cur = obj; ok = True
        for p in path:
            if hasattr(cur, p): cur = getattr(cur, p)
            else: ok = False; break
        if ok and isinstance(cur, rd.ResourceId):
            return cur
    return None

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

    def save_tex(resid, path, label):
        if resid is None or resid == rd.ResourceId.Null():
            log(label, "NULL"); return False
        ts = rd.TextureSave()
        ts.resourceId = resid; ts.destType = rd.FileType.PNG; ts.mip = 0; ts.slice.sliceIndex = 0
        ok = controller.SaveTexture(ts, path)
        # also fetch dimensions/format
        td = None
        for t in controller.GetTextures():
            if t.resourceId == resid: td = t; break
        dim = f"{td.width}x{td.height} fmt={td.format.Name()}" if td else "?"
        log(label, "resid", int(resid), dim, "->", "saved" if ok else "FAILED")
        return ok

    # find first draw that has bound pixel read-only resources (textures)
    found = False
    for i, d in enumerate(draws):
        controller.SetFrameEvent(d.eventId, False)
        state = controller.GetPipelineState()
        ro = state.GetReadOnlyResources(rd.ShaderStage.Pixel)
        ids = []
        for r in ro:
            rid = resid_of(r)
            if rid is not None and rid != rd.ResourceId.Null():
                ids.append(rid)
        if ids:
            log("draw idx", i, "pixel SRVs:", len(ids))
            for k, rid in enumerate(ids[:4]):
                save_tex(rid, os.path.join(OUTDIR, f"rdc_srv_d{i}_{k}.png"), f"  SRV[{k}]")
            found = True
            if i >= 8:  # sample a couple textured draws then stop
                break
    if not found:
        log("no pixel SRVs found on any draw; sample UsedDescriptor attrs:")
        controller.SetFrameEvent(draws[8].eventId, False)
        ro = controller.GetPipelineState().GetReadOnlyResources(rd.ShaderStage.Pixel)
        log("  count:", len(ro))
        if ro:
            log("  attrs:", [a for a in dir(ro[0]) if not a.startswith('_')])

    controller.Shutdown(); cap.Shutdown()
    log("DONE")
except Exception as e:
    log("EXCEPTION:", repr(e)); log(traceback.format_exc())
finally:
    f.close()
