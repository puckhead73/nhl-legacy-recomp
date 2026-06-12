# High-cut C, C-4 — render a TEXTURED guest draw through the ported translator on plume (kickoff prompt)

> **Self-contained kickoff for a fresh session.** Mission: take a real textured menu draw, translate
> its **pixel shader** (the vertex side already works), untile its **guest texture** into a plume
> texture, bind the texture+sampler, and render it textured on the in-process plume Vulkan renderer.
> **This completes C-4 of high-cut path C.** Everything through C-3 is done, committed, and proven;
> C-4 is the next milestone and is roughly the size of all of C-3 combined (the untile is the hard part).

## Where this sits (context — don't re-derive)

High-cut **path C** renders the game on a renderer *we own* (plume RHI, Vulkan, flat RTs, no EDRAM):
`recompiled game → PM4 → SDK CommandProcessor DECODE → OUR plume renderer`. Full plan + history:
`docs/highcut-c-plume-renderer-plan.md`. Memory: `[[highcut-c-plume-renderer]]`.

**Proven and committed already (DO NOT redo):**
- **P-2b / P-3** — Xenia's Xenos→SPIR-V translator is ported to `gpu/spirv/` (rex::), links, and
  emits **`spirv-val`-clean** SPIR-V for real guest shaders. (`gpu/spirv/spirv_shader_translator*.cc`,
  `spirv_builder.cc`, `spirv_shader.cc`, `spirv_translator_tables.cc`.)
- **C-2 / C-3a / C-3b.1** — translated SPIR-V → plume shader module → graphics pipeline → a
  validation-clean draw with descriptor sets bound.
- **C-3 (DONE)** — a real decoded guest **vertex** draw renders its **correct full geometry**: a
  Xenos RectangleList draws as a full-screen quad, objectively confirmed = **921,600 fragments =
  exactly 1280×720** (verified via a PS atomic counter — see "Verification" below). Commits:
  `816e8e9`, `7f07c4e`, `fa6cb5a`.
- **C-4 STARTED** (`8b3d8d0`) — the draw survey finds the textured target (below).

## The C-4 target (already identified)

The textured draw is found by the **survey** in `RenderBetaOwnedDraw`
(`renderer/core/nhl_command_processor.cpp`, gated `NHL_HIGHCUT_XLAT_TEST`): it translates each
*interesting* owned draw (a vfetch VS or a textured PS), dumps `highcut_p3_vs_NNN.spv`, and logs
per-draw `vfetch_bindings` / `vs_tex` / **`ps_tex`**. In **live mode** the textured draws are e.g.
**draw#22 / #26 / #30** (`vfetch_bindings=1 ps_tex=1`, VS 8528 B) — real textured menu elements.
Select which draw feeds the plume bridge with `NHL_HIGHCUT_XLAT_DRAW=N`.

**Dump run** (writes `highcut_p3_vs.spv` + `highcut_p3_draw.bin` for the selected draw):
```
NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_BETA_LIVE=1 NHL_HIGHCUT_XLAT_TEST=1 NHL_HIGHCUT_XLAT_DRAW=22
```
(live mode renders every draw so the menu is reached; pick an index the survey logs as `ps_tex>=1`).

## What C-4 must build (the work)

The current bridge only handles the VS + a **solid-color** placeholder PS (`gpu/hooks/shaders/solid.hlsl`).
C-4 replaces the solid PS with the **translated guest PS** and feeds it the texture. Steps:

1. **Translate the PIXEL shader.** In the survey block, also translate `beta_current_ps_` (it samples
   the texture). Use `GetDefaultPixelShaderModification(ps_reg_count)`.
   **CRITICAL — interpolator mask:** the VS and PS modifications MUST share ONE interpolator mask or
   their I/O signatures mismatch and pipeline linking fails. The beta path already computes this
   (`interpolator_mask` ≈ `nhl_command_processor.cpp:1520-1540`, the AND of VS-writes and PS-reads).
   Translate the VS with that interpolator mask too (the C-3 VS used the *default* modification — for
   C-4 re-translate both VS and PS with the shared interpolator mask). `spirv-val` BOTH.

2. **Reflect the PS's descriptor interface.** Disassemble the PS SPIR-V (`spirv-dis`) and read its
   `OpVariable` storage classes + set/binding decorations (the same way C-3 reflected the VS — see
   the reflection approach in the plan/memory). Xenia's SPIR-V uses **separate** images + samplers
   (header note in `spirv_translator.h`: "Not using combined images and samplers… for every fetch
   constant there are, for regular fetches, two bindings (unsigned and signed)"). Expect: a texture
   descriptor set (sampled images, 2 per fetch constant — unsigned+signed) + a sampler descriptor set,
   plus the PS's own system/float/bool/fetch constant UBOs (likely the same set 1 layout as the VS).

3. **Untile the guest texture (the hard part).** The PS's `beta_current_ps_->texture_bindings()[k]`
   gives the **texture fetch-constant slot** + dimension. The texture fetch constant is **6 dwords**
   at `regs[0x4800 + tex_slot * 6]` (vs the *vertex* fetch which is 2 dwords at `slot*2` — same 192-
   dword space). Parse `xenos::xe_gpu_texture_fetch_t` for base address, width/height, format, **tiled**
   flag, endian, pitch. Read the guest bytes via `memory_->TranslatePhysical(base)`.
   - If the `tiled` bit is **clear**, the texels are already linear — just copy (try this first; some
     menu textures are linear).
   - If **tiled**, convert Xenos tiled addressing → linear. Reference Xenia's texture tiling math
     (`texture_util` / `GetTiledOffset` / `TextureGuestLayout`); `rex/graphics/pipeline/texture/util.h`
     is already included by the command processor — check what untile helpers it exposes. Do CPU untile
     first (correctness over speed; GPU-compute untile is a later optimization).
   - Convert the Xenos texel format → a plume `RenderFormat` the PS expects (start with the common
     menu formats: 8888 / DXT). Endian-swap per the fetch constant.

4. **Create + bind the texture.** Create a plume `RenderTexture` (`RenderTextureDesc::Texture2D`,
   sampled), upload the untiled linear texels (staging buffer → `copyTextureRegion` buffer→texture, as
   in `third_party/plume/examples/cube/main.cpp` ~line 290), create a `RenderSampler`
   (`device->createSampler`), and `setTexture`/`setSampler` into the PS's descriptor sets. Bind both
   the unsigned and signed texture descriptors (point the signed one at the same texture for bring-up).

5. **Build the VS+real-PS pipeline + render.** Extend the plume C-3 path (`gpu/hooks/plume_present.cpp`)
   to: createShader the translated PS (replacing `solidFrag`), build the pipeline layout from the
   union of the VS sets + PS texture/sampler/constant sets, create the pipeline, bind everything, draw.
   Bridge the PS SPIR-V + the texture bytes + the texture metadata via the disk packet (extend
   `gpu/hooks/highcut_draw_packet.h` — add a PS-SPIR-V blob and a texture blob + a small texture
   descriptor: width/height/format).

## Gotchas that WILL bite (learned this session — heed them)

1. **`color_exp_bias` or the output is BLACK.** The PS's final instruction is
   `oC0 = color * xe_color_exp_bias[0].x`. If the PS system-constants UBO leaves `color_exp_bias` = 0,
   **every pixel becomes (0,0,0,0)** — the long-hunted all-zero textured output. Set
   `color_exp_bias[i] = exp2(RB_COLOR_INFO.color_exp_bias)` (0 guest bias → 1.0). The beta path does
   this at `nhl_command_processor.cpp` ~line 2010. The C-3 system-constants dump did NOT set it (VS
   doesn't need it); the **PS does**. This is `[[beta-ps-constant-bank-fix]]` territory.
2. **`spirv-val` EVERY translated shader.** The PS is a different/more-complex shader; it may surface
   another kVersion-6-body-vs-kVersion-12-header mismatch (like the system-constants one C-3a fixed).
   `C:\VulkanSDK\1.4.350.0\Bin\spirv-val.exe highcut_p3_ps.spv` must exit 0 before wiring it in.
3. **Float-controls stay disabled.** Translate with
   `features.{signed_zero_inf_nan_preserve,denorm_flush_to_zero,rounding_mode_rte}_float32 = false`
   (plume's device lacks `VK_KHR_shader_float_controls`; those execution modes crash pipeline creation).
   The C-3 P-3 block already does this — keep it for the PS too.
4. **Interpolator-mask agreement** (see step 1) — mismatch = pipeline link failure (the beta path's
   error 666/660 comment).
5. **PS float constants.** The PS reads ALU constant registers (`xe_uniform_float_constants`, a set-1
   binding the VS didn't use). Dump the float constants (the register file's ALU constant bank) into
   that UBO, or the PS reads zeros. The system-constants struct layout fix from C-3a applies; the PS's
   *float* constants are a separate UBO — reflect it and fill it from the guest constant registers.

## Build / run / verify

- Build: `_build_beta.bat` (targets `out/build/win-amd64-relwithdebinfo`). It auto-reconfigures; the
  glslang `createAccessChain` stateless patch is an idempotent `string(REPLACE)` in `CMakeLists.txt`
  (don't remove it). `solid.hlsl` (and any new PS HLSL) compile via `plume_compile_pixel_shader`.
- Dump: the live-mode command above (writes the `.spv` + `.bin` for the selected textured draw).
- Render: `NHL_HIGHCUT_PRESENT=1 NHL_HIGHCUT_C3=full` (plume Vulkan window; the thread loads the
  dumped files and draws). Add `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` +
  `VK_LAYER_SETTINGS_PATH=<build>/vk_layer_settings.txt` (debug_action=LOG_MSG, log_filename) to get
  a validation log — **target 0 errors/VUID**.
- **Verification (pixel readback is BLOCKED — use the atomic counter).** plume's swapchain images lack
  `TRANSFER_SRC` and an offscreen `copyTextureRegion` GPU-faults, so don't try a texture readback. The
  C-3 path proves rasterization via a **PS atomic counter** (`solid.hlsl`: `RWByteAddressBuffer` at
  `space2/u0`, incremented per fragment; read the host-visible buffer after the fence — see
  `plume_present.cpp` "C-3b.2 verify"). Keep that counter in the textured PS path to confirm fragments.
  For texture *correctness* (did it actually SAMPLE the texture, not just shade), the cheap signal is
  the **plume window** (a recognizable textured menu element) — or have the PS also write a sampled
  texel to a small UAV and read it back. (Building a real offscreen→readback PNG path is a worthwhile
  side-quest but blocked by the plume copy fault noted above; if you fix that, you get free visual diffs.)

## Done criteria

- A real textured menu draw (`ps_tex>=1`, e.g. draw#22) renders on plume with its **guest texture
  sampled** (not the solid placeholder): translated VS+PS both `spirv-val`-clean, pipeline created,
  draw validation-clean, fragments rasterized (atomic counter > 0), and the textured element visible
  in the plume window (or sampled-texel readback confirms non-constant output).
- Commit C-4 (the PS-translation + untile + texture-bind + textured-render). Update
  `docs/highcut-c-plume-renderer-plan.md` + `[[highcut-c-plume-renderer]]`. Then C-5 (full frame,
  per-surface flat plume RTs) resumes.

## Scope / discipline

- Keep everything **gated** (`NHL_HIGHCUT_*`); the beta/DXBC path and all validated scenes must stay
  byte-identical. The plume render only runs under `NHL_HIGHCUT_PRESENT`.
- Translate from the ALREADY-PORTED `gpu/spirv/` translator — do **not** re-port from Xenia. If the PS
  surfaces a new kVersion-6-vs-12 mismatch, fix it the way C-3a fixed the system constants (match the
  SDK `spirv_translator.h` / `dxbc_translator.h` member set), and `spirv-val` to confirm.
- Untile correctness is iterative; lean on `spirv-val` + the validation layer + the atomic counter as
  the fast feedback loop, and start with the simplest texture (linear/8888) before tiled/compressed.
