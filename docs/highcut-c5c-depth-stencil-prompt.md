# High-cut C, C-5c — depth + stencil buffer + a 3D scene (kickoff prompt)

> **Self-contained kickoff for a fresh session.** Mission: give the C-5 plume replay a **depth +
> stencil buffer** and honor each draw's **depth test / depth write / stencil test / face cull**, so
> a **3D gameplay scene** renders with correct occlusion (near hides far) and the menu's stencil-masked
> draws behave. Everything through C-5b is committed and proven; C-5c is the next milestone.
> Full path-C plan + history: `docs/highcut-c-plume-renderer-plan.md`. Memory: `[[highcut-c-plume-renderer]]`.

## Where this sits (context — don't re-derive)

High-cut **path C** renders the game on a renderer we own (plume RHI, **Vulkan**, flat RTs, **no
EDRAM/fold**): `recompiled game → PM4 → SDK CommandProcessor DECODE → OUR plume renderer`. The whole
chain is proven: a real menu **frame** (all draws) replays on plume.

**Proven + committed (DO NOT redo):**
- **C-4 (DONE, `e674acf`)** — one real textured guest draw renders on plume (translated VS+PS, untiled
  texture, validation-clean, visible).
- **C-5a (DONE, `562c8b7`/`615af97`)** — **multi-draw full frame**: `NHL_HIGHCUT_FRAME_CAPTURE` dumps
  EVERY owned draw of a frame to `highcut_frame_<N>.bin` (count in `highcut_frame.count`); the plume
  thread (`NHL_HIGHCUT_C5`) replays them ALL into ONE flat RT (the swapchain) in order with per-draw
  blend. A recognizable NHL Legacy main menu renders, 0 VUID.
- **C-5a/b fixes (all committed):** kQuadList text → index-expanded to triangles (`615af97`); `k_8`
  font atlas → replicate 1-byte coverage to RGBA8 (`6a4519a`); guest texture **swizzle** → applied to
  the plume view component mapping (fixes the BGRA logo, `95830f1`); **black clear** (`ea309ff`);
  per-draw **scissor** from `PA_SC_WINDOW_SCISSOR` (`f5b9dfd` — but it's full-screen for the menu, so
  NOT the menu's clip mechanism); real **`RB_COLOR_MASK`** write mask (`e22c827`). Render bisection
  tool `NHL_HIGHCUT_C5_MINDRAW/MAXDRAW` (`258308d`).
- **Offline texture/packet decode is the proven debugging tool** — parse `highcut_frame_*.bin` with a
  python struct reader + a stdlib BC3 decoder + minimal PNG encoder, write PNGs to the build dir, and
  Read them. Used it to prove all texture DATA is correct and to find the swizzle/font/mask bugs.

**The v3 packet** (`gpu/hooks/highcut_draw_packet.h`) per draw carries: header + fetch consts (768) +
SystemConstants + shared-mem (vertex bytes) + bool/loop + packed VS/PS float consts + **inline VS
SPIR-V** + PS SPIR-V + N `TexturePacketDesc`+texels; plus viewport, blend, scissor, color_write_mask.

## Known-OPEN issues C-5c should resolve or explain

- **Menu "green bars" (deferred, likely STENCIL).** Two bright-green bars at the bottom nav. PROVEN
  NOT data: no green texture/vertex-color/PS-constant exists; black clear didn't remove them; the real
  `RB_COLOR_MASK` (so they're not mask=0 draws) didn't either. Strong leading hypothesis: they are
  **draws the game STENCIL-tests OUT** (stencil compare fails → not drawn), but C-5 has **no stencil
  buffer**, so they draw. **C-5c's stencil test should hide them.** FIRST THING TO CHECK: do the
  green-bar draws have `RB_DEPTHCONTROL.stencil_enable` set with a compare func that would fail vs the
  cleared stencil? (Bisection found them around draws ~115-122 of the menu capture; re-bisect on a
  fresh capture since indices shift per frame.)
- **Menu description-text overflow / ticker not clipped.** Same root: the game clips these by
  **stencil** (window scissor is full-screen), so they need the stencil buffer + a stencil-masked
  region. May or may not fully resolve in C-5c depending on how the mask is built (a mask draw writes
  stencil, later draws test it) — verify.

## What C-5c must build (the work)

### 1. Add a depth+stencil buffer to the plume render (`gpu/hooks/plume_present.cpp`)
- Create a plume depth-stencil `RenderTexture` at `kWidth×kHeight` (1280×720), format
  `RenderFormat::D32_FLOAT_S8_UINT` (24 depth bits unused; depth+stencil in one), flag
  `RenderTextureFlag::DEPTH_TARGET`. Create a depth view.
- Attach it to the per-frame framebuffer: `RenderFramebufferDesc.depthAttachment` /
  `depthAttachmentView` (today `CreateFramebuffers` sets `depthAttachment = nullptr`). The swapchain
  color attachment stays; add the shared depth-stencil.
- Per frame in `RenderClear`: clear depth to the guest clear (usually **1.0**) and stencil to **0**
  via `clearDepth`/`clearDepthStencil` (check the plume cmd API; the cube example clears depth). Do it
  once, after `setFramebuffer`, alongside the existing `clearColor`.
- Barrier the depth texture to `RenderTextureLayout::DEPTH_WRITE` like the color barrier.

### 2. Capture per-draw depth/stencil/cull state (CP side, `RenderBetaOwnedDraw` packet block)
The beta path ALREADY decodes the normalized state — reuse it:
- `reg::RB_DEPTHCONTROL ndc = draw_util::GetNormalizedDepthControl(*register_file_);` (line ~1373)
  gives `ndc.z_enable`, `ndc.z_write_enable`, `ndc.zfunc` (xenos::CompareFunction), `ndc.stencil_enable`,
  `ndc.backface_enable`, and the front/back stencil func+ops (the beta ROV block at ~2237-2255 reads
  `ndc.value >> 8 & 0xFFF` for the front stencil func/ops, `>> 20` for back).
- Stencil ref/masks: `RB_STENCILREFMASK` front = reg `0x210D`, back = `0x210C` (`.stencilref`,
  `.stencilmask` = read mask, `.stencilwritemask`). See the beta ROV block (~2238-2250).
- Cull: `PA_SU_SC_MODE_CNTL` at `0x2205` — `cull_front` (bit0), `cull_back` (bit1), and the front-face
  winding (`face` bit ~ check registers.h `PA_SU_SC_MODE_CNTL`). The beta path's `NHL_BETA_NOCULL`
  clears bits 0-1 (line ~1525). 3D NEEDS culling or back faces z-fight; the menu had cull off.
- Depth format: `reg::RB_DEPTH_INFO.depth_format` (kD24S8 vs kD24FS8 float24 — for bring-up just use
  the plume D32_FLOAT_S8_UINT; float24 nuance deferred).

### 3. Extend the packet → v4 (`highcut_draw_packet.h`)
Bump `kDrawPacketVersion` to 4. Add to `DrawPacketHeader` (keep v3 layout, append): `depth_enable`,
`depth_write`, `depth_func` (xenos::CompareFunction value), `stencil_enable`, `stencil_read_mask`,
`stencil_write_mask`, `stencil_ref`, front `{fail_op,pass_op,depth_fail_op,compare_func}` + back
`{...}` (xenos::StencilOp / CompareFunction values), `cull_mode` (0=none,1=front,2=back), `front_ccw`
(0/1). These are plume-neutral ints (the Xenos enum values); the plume side maps them. The plume
`LoadC5Frames`/`BuildRenderableDraw` reads them and the C-5 render path uses them.

### 4. Build pipelines with depth/stencil/cull + map enums (plume side)
In `BuildRenderableDraw`, set on `RenderGraphicsPipelineDesc`: `depthEnabled`, `depthWriteEnabled`,
`depthFunction`, `depthTargetFormat = D32_FLOAT_S8_UINT`, `stencilEnabled`, `stencilReadMask`,
`stencilWriteMask`, `stencilReference`, `stencilFrontFace`/`stencilBackFace`
(`RenderStencilFaceDesc{passOp,failOp,depthFailOp,compareFunction}`), `cullMode`, `frontFace`. Add
small mappers: xenos::CompareFunction → `RenderComparisonFunction`, xenos::StencilOp →
`RenderStencilOp`, cull → `RenderCullMode`, winding → `RenderFrontFace`. Set `depthClipEnabled` true.
**A draw with depth disabled must set depthEnabled=false** (so the menu's 2D draws are unaffected).

### 5. Validate a 3D scene + regress the menu
- Capture a 3D gameplay frame (drive into a game, F10 takeover, `NHL_HIGHCUT_FRAME_CAPTURE`), replay.
- Confirm depth occlusion (players/rink occlude correctly, near hides far) and **no fold/shear** (the
  EDRAM fold that broke scene_04 on the low cut is structurally absent on this flat path).
- Re-render the menu and confirm no regression (text/logo/textures still correct; check whether the
  green bars + text clipping resolve once stencil is honored).

## Gotchas that WILL bite (heed them)

1. **Depth z range.** Vulkan NDC depth is [0,1] (D3D convention). The VS writes `gl_Position.z` via
   `SystemConstants.ndc_scale[2]/ndc_offset[2]` (already captured each draw). For the menu C-4/C-5 the
   2D draws had z≈0; for 3D verify z lands in [0,1] (if everything renders at one depth or gets
   clipped, the ndc z transform is wrong — log `ndc_scale[2]/offset[2]`).
2. **Cull winding + the Vulkan y-flip.** C-5 bakes a y-flip into `ndc_scale[1]/offset[1]` (plume
   passes viewports verbatim). A y-flip REVERSES triangle winding, so `front_ccw` must account for the
   flip or backface culling culls the FRONT faces (everything vanishes / inside-out). If 3D geometry
   is invisible with culling on, try inverting the front-face or disabling cull (`NHL_BETA_NOCULL`-style
   gate) to confirm this is the cause.
3. **Per-draw VIEWPORT for 3D.** C-5a uses the FULL swapchain viewport for every draw and positions
   geometry purely via per-draw `ndc_scale/offset`. 3D draws (vte transform enabled) may use a guest
   viewport ≠ full surface; the packet ALREADY carries `vp_x/y/w/h` (currently UNUSED by the replay).
   If 3D geometry is mis-placed/scaled, apply the per-draw viewport (scaled guest→1280×720) in the
   render loop instead of the full-screen one. (The fold/shear is gone on the flat path, but the
   viewport mapping still has to be right.)
3. **MSAA.** 3D scenes may be 2X/4X MSAA (frame-feature inventory). The plume RT is 1X. Render 1X for
   bring-up (slight aliasing); a multisampled RT + resolve is deferred. Don't let an MSAA register
   mismatch crash pipeline creation.
4. **Per-surface / Resolve still deferred (C-5d).** 3D frames render to intermediate surfaces and
   Resolve (render-to-texture). C-5 flattens everything to one RT, so render-to-texture composition
   (shadow maps, the rink-cam, post) will be wrong/missing — EXPECTED. Validate basic geometry+depth
   first; per-surface flat RTs + Resolve=host-copy is the NEXT increment (C-5d).
5. **Stencil clear value + ops order.** Clear stencil to 0. A "mask" draw typically writes stencil
   (pass_op=REPLACE, ref=1) with color masked off; later draws test `stencil == 1`. If the green bars
   are stencil-masked, honoring stencil hides them — but only if BOTH the mask-writing draw AND the
   masked draws are present and their ops/refs are captured correctly. spot-check the green draws'
   stencil func/ops in the dump log.
6. **Validation layer.** Adding a depth attachment + depth/stencil state is a common VUID source
   (format mismatch, missing depth barrier, write-mask vs read-only). Run with
   `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` + `vk_layer_settings.txt` and target 0 VUID.

## Build / run / verify

- Build: `_build_beta.bat` → `BUILD_EXIT=0`.
- Capture (drive to a 3D scene OR the menu): `_c5dump.ps1` (beta-live + F10 + `NHL_HIGHCUT_FRAME_CAPTURE`;
  writes `highcut_frame_<N>.bin` + `highcut_frame.count`). The dump log prints per-draw
  `cwm`/`flags`/`vp`; ADD depth/stencil/cull to that log for verification.
- Render: `_c5render.ps1` (`NHL_HIGHCUT_PRESENT=1 NHL_HIGHCUT_C5=1` + the validation layer). Bisect
  with `NHL_HIGHCUT_C5_MINDRAW/MAXDRAW`.
- **Offline check (reuse the proven tool):** the python packet parser at the top of this thread —
  parse `highcut_frame_*.bin`, decode textures to PNG (stdlib BC3 + PNG), and read the images. The v4
  header adds the depth/stencil fields; update the struct format string (`<14I 6f 8I 4I ...`).

## Done criteria

- A real 3D gameplay scene renders on plume with **correct depth occlusion** (near occludes far, no
  z-fighting with cull on) and **no fold/shear**, validation-clean.
- The 2D menu is **unregressed** (depth-disabled 2D draws still composite correctly); ideally the
  green bars + text clipping resolve once stencil is honored (or are clearly explained as needing the
  mask-write + masked-draw pairing / per-surface).
- Commit C-5c; update `docs/highcut-c-plume-renderer-plan.md` + `[[highcut-c-plume-renderer]]`.

## Scope / discipline

- Keep everything **gated** (`NHL_HIGHCUT_*`); the beta/DXBC path + validated scenes stay byte-identical.
- **Depth/stencil INFRASTRUCTURE + a 3D scene is C-5c.** Per-surface flat RTs + guest Resolve =
  host-copy (correct render-to-texture composition) is **C-5d** — don't try to do it here; expect
  render-to-texture content (shadows/cams/post) to be wrong until then.
- Reuse the CP's existing decode (`draw_util::GetNormalizedDepthControl`, the ROV stencil block) — do
  NOT re-derive the depth/stencil register layout.

## Key files + pointers

- Capture: `renderer/core/nhl_command_processor.cpp` `RenderBetaOwnedDraw` (the survey block ~1567 +
  the packet block ~1865; `draw_util::GetNormalizedDepthControl` ~1373; the ROV stencil decode
  ~2237-2255). Per-frame count reset/write in `IssueSwap` (~5290). Member `highcut_capture_idx_` in
  `nhl_command_processor.h`.
- Packet: `gpu/hooks/highcut_draw_packet.h` (bump to v4).
- Replay: `gpu/hooks/plume_present.cpp` (`RenderableDraw`, `BuildRenderableDraw`, `LoadC5Frames`,
  `RenderClear`, `CreateFramebuffers`). plume depth/stencil API in
  `third_party/plume/plume_render_interface_types.h`: `RenderGraphicsPipelineDesc`
  {depthEnabled,depthWriteEnabled,depthFunction,depthTargetFormat,stencilEnabled,stencilReadMask,
  stencilWriteMask,stencilReference,stencilFrontFace,stencilBackFace,cullMode,frontFace},
  `RenderStencilFaceDesc`{passOp,failOp,depthFailOp,compareFunction}, `RenderFramebufferDesc`
  {depthAttachment,depthAttachmentView}, formats D32_FLOAT_S8_UINT, `RenderTextureFlag::DEPTH_TARGET`,
  `RenderTextureLayout::DEPTH_WRITE`.
- Scripts: `_c5dump.ps1` / `_c5render.ps1`. Memory: `[[highcut-c-plume-renderer]]`.
