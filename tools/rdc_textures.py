import renderdoc as rd
import os, traceback

OUTDIR = r"e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
RDC = os.path.join(OUTDIR, "beta_takeover_capture.rdc")
RES = os.path.join(OUTDIR, "rdc_textures.txt")

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

    textures = {t.resourceId: t for t in controller.GetTextures()}

    for i in [4, 5, 6, 7, 8, 10]:
        if i >= len(draws):
            continue
        controller.SetFrameEvent(draws[i].eventId, False)
        ro = controller.GetPipelineState().GetReadOnlyResources(rd.ShaderStage.Pixel)
        log(f"--- draw {i}: {len(ro)} pixel SRVs")
        for k, r in enumerate(ro):
            rid = getattr(r.descriptor, 'resource', None)
            if rid is None or rid == rd.ResourceId.Null():
                log(f"    SRV[{k}] null"); continue
            td = textures.get(rid)
            if not td:
                log(f"    SRV[{k}] resid={int(rid)} (no texture desc)"); continue
            # Read a few texels to gauge content; save a PNG.
            ts = rd.TextureSave()
            ts.resourceId = rid; ts.destType = rd.FileType.PNG; ts.mip = 0; ts.slice.sliceIndex = 0
            png = os.path.join(OUTDIR, f"rdc_tex_d{i}_{k}.png")
            ok = controller.SaveTexture(ts, png)
            log(f"    SRV[{k}] resid={int(rid)} {td.width}x{td.height} fmt={td.format.Name()} "
                f"mips={td.mips} -> {'saved' if ok else 'SAVE FAILED'} {os.path.basename(png)}")

    controller.Shutdown(); cap.Shutdown()
    log("DONE")
except Exception as e:
    log("EXCEPTION:", repr(e)); log(traceback.format_exc())
finally:
    f.close()
