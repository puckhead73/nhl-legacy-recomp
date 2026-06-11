import renderdoc as rd
import os, traceback

OUTDIR = r"e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
RDC = os.path.join(OUTDIR, "beta_takeover_capture.rdc")
RES = os.path.join(OUTDIR, "rdc_pick.txt")

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

    # For each interesting draw, set the event and pick the center pixel of the
    # bound output target (RenderDoc's own readback of its replay - independent of
    # our offscreen-RT copy path which RenderDoc breaks).
    for i in [0, 3, 4, 5, 6, 10, 12, 62]:
        if i >= len(draws):
            continue
        d = draws[i]
        controller.SetFrameEvent(d.eventId, False)
        state = controller.GetPipelineState()
        outs = state.GetOutputTargets()
        if not outs:
            log(f"draw {i}: no output targets"); continue
        rtid = getattr(outs[0], 'resourceId', None) or getattr(outs[0], 'resource', None)
        if rtid is None or rtid == rd.ResourceId.Null():
            log(f"draw {i}: RT null"); continue
        # texture dims
        td = None
        for t in controller.GetTextures():
            if t.resourceId == rtid: td = t; break
        w = td.width if td else 1280
        h = td.height if td else 720
        samples = []
        for (px, py) in [(w//2, h//2), (w//4, h//4), (10, 10)]:
            val = controller.PickPixel(rtid, px, py, rd.Subresource(0,0,0), rd.CompType.Typeless)
            fv = val.floatValue
            samples.append(f"({px},{py})=({fv[0]:.3f},{fv[1]:.3f},{fv[2]:.3f},{fv[3]:.3f})")
        log(f"draw {i}: RT {int(rtid)} {w}x{h} pixels: " + " ".join(samples))

    controller.Shutdown(); cap.Shutdown()
    log("DONE")
except Exception as e:
    log("EXCEPTION:", repr(e)); log(traceback.format_exc())
finally:
    f.close()
