import renderdoc as rd
import os, traceback

OUTDIR = r"e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
RDC = os.path.join(OUTDIR, "beta_takeover_capture.rdc")
RES = os.path.join(OUTDIR, "rdc_rt.txt")

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

    def dump_at(i, tag):
        if i >= len(draws): return
        controller.SetFrameEvent(draws[i].eventId, False)
        ps = controller.GetPipelineState()
        # current bound color render targets
        rts = ps.GetOutputTargets()
        log(f"--- draw {i} ({tag}): {len(rts)} RTs")
        for k, rt in enumerate(rts):
            rid = getattr(rt, 'resource', None) or getattr(rt, 'resourceId', None)
            if rid is None or rid == rd.ResourceId.Null():
                log(f"    RT[{k}] null"); continue
            td = textures.get(rid)
            ts = rd.TextureSave()
            ts.resourceId = rid; ts.destType = rd.FileType.PNG; ts.mip = 0; ts.slice.sliceIndex = 0
            png = os.path.join(OUTDIR, f"rt_d{i:02d}_{tag}_{k}.png")
            ok = controller.SaveTexture(ts, png)
            wh = f"{td.width}x{td.height}:{td.format.Name()}" if td else "?"
            log(f"    RT[{k}] resid={int(rid)} {wh} -> {'saved' if ok else 'FAIL'} {os.path.basename(png)}")
        # Also list ALL bound pixel resources via the full descriptor access (not just ReadOnly)
        try:
            used = ps.GetReadOnlyResources(rd.ShaderStage.Pixel)
            nonnull = sum(1 for r in used if (getattr(r.descriptor,'resource',None) not in (None, rd.ResourceId.Null())))
            log(f"    pixel SRVs bound (nonnull): {nonnull}/{len(used)}")
        except Exception as e:
            log("    srv-enum err", repr(e))

    last = len(draws) - 1
    for i, tag in [(0,"first"), (3,"afterbg"), (12,"afterUI"), (last,"final")]:
        dump_at(i, tag)

    controller.Shutdown(); cap.Shutdown()
    log("DONE")
except Exception as e:
    log("EXCEPTION:", repr(e)); log(traceback.format_exc())
finally:
    f.close()
