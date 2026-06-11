# RenderDoc runbook — why beta's host-RT 3D player geometry vanishes

> Goal of this investigation, stated once, precisely:
> **In the host-RT EDRAM path, beta's 3D player draws reach `D3DDrawIndexedInstanced` with no errors and
> `Update()` reports color+depth bound (`bound_bits=0x3`), yet the resolved color contains only the
> background — the player geometry never lands in the host render target. The base command processor
> (the oracle), using the SAME SDK `D3D12RenderTargetCache`, renders it correctly. Find where beta's
> geometry diverges from the oracle's.**
>
> This is the one remaining question that black-box probing cannot answer (see
> `[[beta-scene04-projection]]` UPDATE 5). RenderDoc is white-box: it shows the bound RT, the viewport,
> the post-VS vertex positions, and per-pixel history. The divergence point IS the bug.

This codebase already has RenderDoc integration, so you do **not** write any capture code — you set env
vars and launch under RenderDoc.

---

## 0. What "host-RT path" means here (don't confuse the three paths)

| Run | Env | Player result | Use |
|---|---|---|---|
| **Host-RT takeover** (this investigation) | `NHL_BETA_EDRAM=1`, NO `NHL_BETA_RT_PATH` | geometry **absent** | the buggy capture |
| **Oracle** (reference) | beta enabled, **no** `NHL_BETA_TAKEOVER` | correct | the gold capture to diff against |
| ROV takeover | `NHL_BETA_RT_PATH=rov` | present but green | not this investigation |

The SDK cache defaults to `Path::kHostRenderTargets` (path=0). Host-RT = `NHL_BETA_EDRAM=1` without the
ROV override. Confirm in the log line `2/5 D3D12RenderTargetCache initialized (path=0 ...)`.

---

## 1. Prerequisites

- **RenderDoc** installed (the Windows UI app, `qrenderdoc.exe`). v1.30+ is fine.
- Build green: `cmd.exe /c "e:\Repositories\nhl-legacy-recomp\_build_beta.bat"` (BUILD_EXIT=0).
- The capture hook attaches via `RenderDocAPI::CreateIfConnected()` — it returns non-null **only when the
  process was launched under RenderDoc** (RenderDoc injects its in-app API). Launching the exe normally
  with `NHL_BETA_RENDERDOC=1` does nothing; you must launch it **from RenderDoc**.
- Reference data for the player draws (scene_02 EditPlayer, frame 30), so you can find them:
  - Player passes: `vte=0x43F` (3D, viewport transform on), `surf_pitch=640`, two window-offset groups —
    pass A `win_off=(0,0)`, **pass B `win_off=(-640,0)`** (pass B carries the on-screen player).
  - Host RT for this surface: **640 wide × ~4096 tall** (EDRAM tile strip), color base tile 0, depth base
    tile 736, format 8888 (k_8_8_8_8), **msaa=1**. The player occupies roughly the first 720 rows.
  - The color resolve that should contain the player is resolve **#16** (src=0 color, base 0); the depth
    resolve is **#15** (src=4). The composite samples those downstream.
  - Already ruled out (don't re-test): depth-compare cull (`NHL_BETA_DEPTH_FORCE_ALWAYS`), back-face cull
    (`NHL_BETA_NOCULL`), per-frame reseed (`NHL_BETA_NO_RESEED`), per-draw RT re-bind invalidate. Viewport
    + ndc for pass B match upstream Xenia exactly (`extent=640, ndc_scale=2, offset=-1`).

---

## 2. Launch the BUGGY capture (host-RT takeover) under RenderDoc

In the RenderDoc UI → **Launch Application** tab:

- **Executable Path:** `e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo\nhllegacy.exe`
- **Working Directory:** `e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo`
- **Command-line Arguments:** `--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"`
- **Environment Variables** (Edit → add each):

  ```
  NHL_BACKEND=beta
  NHL_BETA_TAKEOVER=1
  NHL_BETA_EDRAM=1
  NHL_BETA_DEPTH=1
  NHL_BETA_CAPTURE_FRAME=30
  NHL_REPLAY_XTR=e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo\gpu_trace\scene_02\454109EC_stream.xtr
  NHL_BETA_RENDERDOC=1
  NHL_BETA_EDRAM_DIAG=1
  ```

  Optional, to shrink the capture so the player draw is easy to find (cap just past the player passes):
  `NHL_BETA_MAX_DRAW=360`. Or render ONLY the player passes: `NHL_BETA_ONLY_DRAW=<comma list>` (get the
  indices from the `edram-draw #… win_off=(-640,0)` lines in the log).

- Click **Launch**. The hook calls `StartFrameCapture` at the first owned draw and `EndFrameCapture` at
  IssueSwap; the log prints `RenderDoc StartFrameCapture (takeover frame)` and `EndFrameCapture -> 1`.
  A `.rdc` thumbnail appears in RenderDoc's capture list. Save it as `beta_hostrt.rdc`.

> Equivalent CLI (if you prefer scripted capture): set the same env in the shell, then
> `& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture -d e:\...\win-amd64-relwithdebinfo -w `
> `e:\...\nhllegacy.exe --game_data_root "H:\..."`. The in-app API still does the bracketing.

---

## 3. Launch the ORACLE capture (the gold reference) under RenderDoc

Same as §2 but **drop takeover** and use the oracle bracket env:

- Remove `NHL_BETA_TAKEOVER`, `NHL_BETA_EDRAM`, `NHL_BETA_DEPTH`, `NHL_BETA_RENDERDOC`, `NHL_BETA_MAX_DRAW`.
- Add:
  ```
  NHL_BACKEND=beta
  NHL_BETA_RDOC_ORACLE=1
  NHL_BETA_CAPTURE_FRAME=30   (or NHL_SHOT_FRAME=30 — see run_oracle.ps1)
  NHL_REPLAY_XTR=...scene_02\454109EC_stream.xtr
  ```
- Launch. Log prints `RenderDoc ORACLE capture CONNECTED` then `EndFrameCapture (oracle/base frame)`.
  Save as `oracle_hostrt.rdc`. This is the base CP rendering the player correctly through the same SDK
  cache — your ground truth.

---

## 4. Find the player draw in each capture

In RenderDoc → **Event Browser**:

1. The player passes are **indexed** 3D draws (`DrawIndexedInstanced`) into a tall, narrow, 8888 render
   target. The 2D background/UI draws go to wide RTs; the player draws are the ones whose **Output Merger
   RT is ~640 wide and very tall**.
2. To match the SAME draw between `beta_hostrt.rdc` and `oracle_hostrt.rdc`, do **not** rely on event IDs
   (they differ — beta interleaves resolves differently). Match by: **vertex/index count**, **VS/PS shader
   disassembly hash**, and the bound RT key. The pass-B player draws are the cluster of identical-count
   indexed draws right after the `win_off=(-640,0)` group.
3. Right-click the player draw → **"Show in Timeline"** to confirm it's the one that should paint the
   player, and note its EID in each capture.

If you used `NHL_BETA_ONLY_DRAW`, there will only be a handful of draws — the player draw is obvious.

---

## 5. The diff protocol — six panels, same draw, beta vs oracle

For the matched player draw, open these RenderDoc panels in **both** captures and compare. The **first
panel that differs is the bug.** Go in this order (cheapest cause first):

### 5a. Output Merger → bound Render Target (THE top suspect)
- **Pipeline State → OM → Render Targets.** Record for beta vs oracle: the RT **resource**, its
  **Width × Height**, **Format**, and **Sample Count (MSAA)**.
- **Decision:**
  - If beta's bound RT has a **different MSAA sample count, format, or dimensions** than the oracle's →
    beta is keying a **different host-RT instance** than the resolve later reads. This is the leading
    hypothesis (the fold draws log `msaa=1` but resolve #16 is `copy_sample_select=4`). The fix is in how
    beta drives `Update()` / the `RenderTargetKey` (sample count / pitch) so the rendered RT and the
    resolved RT are the same key. **This is the most likely outcome — check it first.**
  - If the RT resource is the **same key/shape** as the oracle's, move on.

### 5b. Texture Viewer → the bound RT, "after this draw"
- Select the OM RT, scrub the event slider to **just after** the player draw. The host RT is a tall strip;
  the player should appear in the top ~720 rows (it may be at a fold-shifted X).
- **Decision:**
  - Oracle shows the player here, beta does **not** → geometry never rasterized into this RT; continue to
    5c/5d to find why (clip vs reject).
  - Beta shows the player **here** but the final resolve is empty → the geometry DID land; the bug is
    downstream in the **resolve/dump tile addressing** (jump to 5f).

### 5c. Mesh Viewer → VS Output (where does the geometry actually project?)
- Open **Mesh Viewer** on the player draw, **VS Output** tab. Look at the post-transform clip/NDC
  positions and the "Output" preview.
- **Decision:**
  - Beta's VS-output positions **differ from the oracle's** (off-screen, behind near plane, NaN, collapsed)
    → a **vertex-shader / system-constant** bug (W-divide flags, ndc_scale/offset, clip planes, the
    `PA_SC_WINDOW_OFFSET` handling). Note: black-box said ndc matches Xenia, so this would be a subtle
    constant — verify the SystemConstants CBV (5e).
  - Beta's positions **match the oracle** but nothing rasterizes → it's a **fixed-function clip**
    (viewport/scissor/RT-bounds), go to 5d.

### 5d. Rasterizer → Viewport + Scissor, and the host-RT bounds
- **Pipeline State → Rasterizer.** Compare beta vs oracle: **Viewport** (TopLeft, Width, Height,
  Min/MaxDepth), **Scissor**, **cull mode**, **front-face winding**, **depth clip**.
- Cross-check against the bound RT size from 5a. The host RT is **640 wide**; pass B's player should land in
  `host_x[~10,410]`. If beta's **viewport/scissor or the 640-wide RT bounds clip the player out** (e.g. the
  player projects to host_x > 640 and is clipped, or the scissor is wrong), that's the bug.
- **Decision:** a viewport/scissor/winding/depth-clip that differs from the oracle → fix beta's
  rasterizer-state driving for these draws.

### 5e. Pipeline constants → SystemConstants CBV (b0-ish) and fetch/float CBVs
- For the player draw, inspect the bound **constant buffers** beta passes (`SetGraphicsRootConstantBufferView`
  slots 0–4: fetch, float VS, float PS, **SystemConstants**, bool). Compare the SystemConstants
  (ndc_scale/offset, flags, color_exp_bias, the EDRAM bases) to the oracle's.
- **Decision:** a different ndc/flags/exp_bias value than the oracle → a constant-setup bug in
  `RenderBetaOwnedDraw`.

### 5f. Pixel History (the definitive per-pixel answer)
- In the Texture Viewer, pick a pixel that SHOULD be the player (use the oracle to find its host-RT
  coordinate), right-click → **History**.
- **Decision tree from the fragment list:**
  - **No fragments at all** at that pixel → geometry didn't cover it → clip/viewport/VS issue (5c/5d).
  - **Fragments generated but "Failed Depth Test"** → depth state/buffer (even though force-ALWAYS was
    tested black-box, RenderDoc shows the actual depth func + the depth value read; confirm the depth RT is
    the one bound and cleared as expected).
  - **Fragments generated, "Failed Scissor/Sample mask"** → scissor / sample-mask / MSAA-coverage.
  - **Fragment passed and wrote a color** → it DID write; the resolve reads a different RT/region → 5a + 5f
    on the resolve dispatch.

---

## 6. Inspect the RESOLVE too (if 5b shows the player in the RT but it's lost later)

The resolve is a **compute dispatch** (DumpRenderTargets → resolve copy) inside the SDK. In the Event
Browser find the resolve events for base 0 (after the player passes). For beta vs oracle compare:
- The **input SRV** bound to the dump/resolve compute (which host-RT texture — same key as the one the
  player rendered into?).
- The **dispatch dimensions** and the destination buffer region.
- **Decision:** if beta's resolve reads a **different host-RT texture** than the one the player rendered
  into (different MSAA/pitch key) → confirms the 5a key-mismatch from the resolve side. This closes the
  loop on the MSAA-sample / RenderTargetKey hypothesis.

---

## 7. Expected outcome → the fix it points to

| RenderDoc finding | Bug | Fix location |
|---|---|---|
| Player draw's OM RT differs in MSAA/format/pitch from oracle (5a/6) | beta keys a different host-RT than the resolve reads | how `RenderBetaOwnedDraw` drives `Update()` / RB_SURFACE_INFO sample count + pitch (the EDRAM-mode MSAA override path, ~nhl_command_processor.cpp:1160) |
| VS-output positions differ (5c/5e) | VS/system-constant divergence | SystemConstants setup in `RenderBetaOwnedDraw` |
| Positions match, fixed-function clips (5d/5f) | viewport/scissor/winding/depth-clip | the EDRAM viewport branch (~nhl_command_processor.cpp:1349) |
| Player in RT but resolve reads elsewhere (5b/6) | resolve/dump tile addressing or RT key | `BetaResolveEdram` / the register state passed to `Resolve()` |

Whatever the matched draw's first divergent panel is, that's the single beta-side knob to fix — and unlike
the ROV green (structural shared-EDRAM overlap, not cheaply fixable) or the offscreen position (non-affine
fold), the host-RT path is the oracle's own path, so a matching fix makes beta render the player correctly
through the real EDRAM/resolve seam (and generalizes to all 3D).

---

## 8. Gotchas specific to this setup

- **CreateIfConnected returns null if not launched under RenderDoc.** If the log says "NOT connected",
  you launched the exe directly — relaunch from the RenderDoc UI / `renderdoccmd capture`.
- **Deferred command list:** beta records into a deferred list executed at IssueSwap. RenderDoc captures
  the real GPU submission, so the draw order you see is the executed order (draws then resolves), which is
  what you want.
- **Single-frame replay:** the bracket captures exactly the takeover/oracle frame (CAPTURE_FRAME=30). One
  `.rdc` per launch.
- **The host RT is a tall strip (≈640×4096), not a screen rectangle.** In the Texture Viewer it looks
  narrow and very tall; the player is in the first ~720 rows, possibly fold-shifted in X. Don't mistake the
  empty lower rows for "nothing rendered."
- **Match draws by content, not EID** — beta and oracle have different event numbering because beta
  dispatches resolves as draws inline.
- **Reduce noise** with `NHL_BETA_ONLY_DRAW`/`NHL_BETA_MAX_DRAW` so the capture has few draws; but take at
  least one FULL-frame capture too, in case isolation changes RT-cache ownership behavior.
- Keep `NHL_BETA_EDRAM_DIAG=1` on — the log's `edram-draw #… color_edram_tile/surf_pitch/msaa/win_off` and
  `vpi #…` lines let you correlate a RenderDoc draw back to the register state.

---

## 9. One-paragraph summary for whoever runs this

Capture two frames under RenderDoc — `beta_hostrt.rdc` (`NHL_BETA_EDRAM=1 NHL_BETA_DEPTH=1
NHL_BETA_TAKEOVER=1 NHL_BETA_RENDERDOC=1`) and `oracle_hostrt.rdc` (`NHL_BETA_RDOC_ORACLE=1`, no takeover).
Find the pass-B player draw (`win_off=-640`, indexed, into the ~640-wide tall 8888 RT) in each, matched by
vertex count + shader hash. Walk the six panels (OM RT → Texture Viewer → Mesh VS-output → Rasterizer →
SystemConstants → Pixel History) and stop at the first that differs from the oracle. The top suspect is the
Output-Merger RT differing in MSAA sample count from what the resolve later reads; the pixel-history call
will say definitively whether the geometry was clipped, depth/scissor-rejected, or written-but-lost. That
divergent panel names the single beta-side fix.
