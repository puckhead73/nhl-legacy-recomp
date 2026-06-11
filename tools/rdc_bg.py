import renderdoc as rd
import os, traceback

OUTDIR = r"e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
RDC = os.path.join(OUTDIR, "beta_takeover_capture.rdc")
RES = os.path.join(OUTDIR, "rdc_bg.txt")

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

    for i in range(min(16, len(draws))):
        controller.SetFrameEvent(draws[i].eventId, False)
        ps = controller.GetPipelineState()
        # --- bound pixel SRVs ---
        ro = ps.GetReadOnlyResources(rd.ShaderStage.Pixel)
        srv_desc = []
        for k, r in enumerate(ro):
            rid = getattr(r.descriptor, 'resource', None)
            if rid is None or rid == rd.ResourceId.Null():
                continue
            td = textures.get(rid)
            if td:
                ts = rd.TextureSave()
                ts.resourceId = rid; ts.destType = rd.FileType.PNG; ts.mip = 0; ts.slice.sliceIndex = 0
                png = os.path.join(OUTDIR, f"bg_d{i:02d}_srv{k}.png")
                controller.SaveTexture(ts, png)
                srv_desc.append(f"srv{k}=resid{int(rid)}:{td.width}x{td.height}:{td.format.Name()}")
            else:
                srv_desc.append(f"srv{k}=resid{int(rid)}:?")
        # --- output-merger blend state ---
        try:
            blend = ps.GetColorBlends()
            b0 = blend[0] if blend else None
        except Exception as e:
            b0 = None
        if b0 is not None:
            bdesc = (f"blendEnable={b0.enabled} "
                     f"src={b0.colorBlend.source} dst={b0.colorBlend.destination} op={b0.colorBlend.operation} "
                     f"writeMask={b0.writeMask}")
        else:
            bdesc = "blend=?"
        # --- depth state ---
        try:
            ds = ps.GetDepthState()
            ddesc = f"depthTest={ds.depthEnable} depthWrite={ds.depthWrites} func={ds.depthFunction}"
        except Exception:
            ddesc = "depth=?"
        # --- vertex/index counts ---
        a = draws[i]
        log(f"draw {i:02d} ev{a.eventId} verts={a.numIndices} | {bdesc} | {ddesc} | "
            f"{'; '.join(srv_desc) if srv_desc else 'NO-SRV'}")

    controller.Shutdown(); cap.Shutdown()
    log("DONE")
except Exception as e:
    log("EXCEPTION:", repr(e)); log(traceback.format_exc())
finally:
    f.close()
