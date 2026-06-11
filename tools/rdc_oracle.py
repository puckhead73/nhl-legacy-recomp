import renderdoc as rd
import os, traceback

OUTDIR = r"e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
RDC = os.path.join(OUTDIR, "oracle_capture_capture.rdc")
RES = os.path.join(OUTDIR, "rdc_oracle.txt")

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

    # Dump bound pixel SRVs for the cloud/flag draws + the final RT
    for i in [4,5,6,7,8,9,10,11,12]:
        if i >= len(draws): break
        controller.SetFrameEvent(draws[i].eventId, False)
        ps = controller.GetPipelineState()
        ro = ps.GetReadOnlyResources(rd.ShaderStage.Pixel)
        descs = []
        for k, r in enumerate(ro):
            rid = rid_of(r.descriptor)
            if rid is None or rid == rd.ResourceId.Null():
                continue
            td = textures.get(rid)
            if td:
                ts = rd.TextureSave()
                ts.resourceId = rid; ts.destType = rd.FileType.PNG; ts.mip = 0; ts.slice.sliceIndex = 0
                png = os.path.join(OUTDIR, f"oracle_d{i:02d}_srv{k}_resid{int(rid)}.png")
                ok = controller.SaveTexture(ts, png)
                descs.append(f"srv{k}=resid{int(rid)}:{td.width}x{td.height}:{td.format.Name()}:{'saved' if ok else 'FAIL'}")
            else:
                descs.append(f"srv{k}=resid{int(rid)}:?")
        log(f"d{i:02d} ev{draws[i].eventId} v{draws[i].numIndices} PSsrv=[{'; '.join(descs) if descs else '-'}]")

    # Final present RT
    last = len(draws) - 1
    controller.SetFrameEvent(draws[last].eventId, False)
    ps = controller.GetPipelineState()
    rts = ps.GetOutputTargets()
    if rts:
        rid = rid_of(rts[0])
        if rid and rid != rd.ResourceId.Null():
            td = textures.get(rid)
            ts = rd.TextureSave()
            ts.resourceId = rid; ts.destType = rd.FileType.PNG; ts.mip = 0; ts.slice.sliceIndex = 0
            png = os.path.join(OUTDIR, f"oracle_finalRT_resid{int(rid)}.png")
            ok = controller.SaveTexture(ts, png)
            log(f"finalRT resid={int(rid)} {td.width}x{td.height if td else '?'} -> {'saved' if ok else 'FAIL'}")

    controller.Shutdown(); cap.Shutdown()
    log("DONE")
except Exception as e:
    log("EXCEPTION:", repr(e)); log(traceback.format_exc())
finally:
    f.close()
