> ## RESOLUTION (2026-06-14) — ROOT CAUSE FOUND + FIX IMPLEMENTED (pending live re-capture to confirm)
> **The number was a POINT-vs-LINEAR sampler bug on the nameplate-LAYOUT texture — NOT the CLAMP
> hypothesis.** Offline analysis of the player jersey draw (capture idx 639, surface depth=736) proved:
> - The jersey **number/letter atlas** (gold digits 0–9 + A–Z) IS captured + bound: PS texture slot4,
>   BC3 512×512. Its guest clamp mode is already CLAMP, so hypothesis 1 (CLAMP sampler) is **ruled out**
>   for the number.
> - The player PS computes the atlas UV by first sampling a **nameplate-layout data texture** (slot6,
>   RGBA8 256×256) and decoding its RGBA via a fixed-point unpack (`×261120…`) into a precise atlas UV
>   (then the atlas sample is gated by a predicate). This is the **jersey_nameplate** layout the user
>   named — it determines where each digit/letter/patch lands.
> - **slot6 is the ONLY texture in the draw with guest `mag=POINT min=POINT`** (every other is LINEAR).
>   Plume hardcoded LINEAR+CLAMP for *all* PS textures (`plume_present.cpp:756`), so the layout map got
>   bilinearly blended → garbage atlas UV → the number sampled a blank atlas cell → **no number**. The
>   chest crest (slot9, a normal LINEAR decal) was unaffected — exactly the asymmetry in §2. Same class
>   of bug as the C-5d.3 bone-palette POINT fix.
>
> **Fix (committed in this session's working tree):** honor the guest per-sampler filter+clamp instead
> of one hardcoded sampler. Packet bumped **v8→v9** with a new `SamplerPacketDesc` (per `SamplerBinding`,
> filter+clamp resolved from the guest fetch constant); plume builds one `RenderSampler` per binding and
> binds it at `nTex+i`. Files: `gpu/hooks/highcut_draw_packet.h`, `renderer/core/nhl_command_processor.cpp`
> (capture `GetSamplerBindingsAfterTranslation()` + write descs), `gpu/hooks/plume_present.cpp` (read +
> per-binding sampler create/bind, `xenosFilter`/`xenosMipMode`/`xenosClamp` helpers),
> `tools/highcut_packet_decode.py` (accept v9). Builds clean (`_build_beta.bat` → `BUILD_EXIT=0`).
> Diagnostic: `tools/_inspect_clamp.py <build_dir> <surf_depth> <lo> <hi>` dumps per-texture clamp+filter.
>
> **TO CONFIRM (user step — needs a live F10 capture; the old v8 capture won't replay on the v9 reader):**
> `.\_c5dump.ps1` → drive to a face-off → F10 → hold ~5s; then `.\_c5render.ps1` and check the back
> numbers. Verify with `python tools/_inspect_clamp.py out/build/win-amd64-relwithdebinfo 736 639 640`
> that slot6 still reports `mag=POINT` (the capture path is unchanged; the plume sampler now obeys it).
>
> ---

# High-cut C-5f — jersey numbers (self-contained kickoff for a cold session)

> **Mission:** the player **back jersey NUMBERS** (and likely the nameplate) don't render. Everything
> else in the 3D gameplay scene now does. Find why the number layer is missing and fix it.
> Prior context: `docs/highcut-c5-restart-prompt.md` (the session that got the scene rendering),
> `docs/highcut-c-plume-renderer-plan.md` (full path-C history). Memory:
> `[[highcut-c5-skinning-solved-ps-dark]]`, `[[highcut-c-plume-renderer]]`.

---

## 1. Current state — what RENDERS (proven, committed `f415117`, don't redo)

A full **Scottrade Center face-off** renders correctly via `_c5render.ps1` (default flags):
ice + center-ice Blues logo + ad decals (EA Sports / Tim Hortons / Honda) + blue/red lines + face-off
dots + **players with correct jerseys** (Blues blue, Blackhawks white, ref stripes) + skate spray.

**The whole "players exploded / black frame" saga was NOT skinning** — it was two bugs, both fixed in
`f415117`:
- **Primitive restart:** players are indexed TRIANGLE STRIPS with `0xFFFF` reset; capture flattened
  them to LISTS and drew the reset markers as out-of-bounds vertices → the "explosion." Fixed
  (`kTriangleStrip → kTopoTriangleStrip`; plume auto-enables `primitiveRestartEnable`).
- **Inverted cull:** y-flip front-face compensation double-flipped → closed meshes shown front-from-
  behind, ice culled. Fixed (use guest `front_ccw` directly; `NHL_HIGHCUT_FLIP_FACE` = old behavior).

Also proven this path: skinning (set2 bone palette) works; PS shading/textures/lighting work (jerseys
are correctly textured + lit); the **chest crest renders** (the Blues logo is visible); the default
surface-split picks **depth=736 = the main 3D scene**. The translator is verbatim-correct (ruled out).

---

## 2. The target — back numbers missing

The base jersey renders, and the **chest crest renders**, but the **number on the back is absent**
(missing, NOT magenta — so it's not the unsupported-format placeholder). The number/nameplate is almost
certainly a **decal / second layer**, distinct from the base jersey and the chest crest.

**Key asymmetry to exploit:** chest crest renders, back number does not. Whatever differs between those
two is the lead.

---

## 3. Texture-format support (so you know what "magenta" vs "missing" means)

Capture (`renderer/core/nhl_command_processor.cpp` ~2090-2118) supports: `k_8_8_8_8`→RGBA8, `k_8`→R8
expanded to RGBA8 (fonts/coverage masks — recently added), `k_DXT1`→BC1, `k_DXT2_3`→BC2, `k_DXT4_5`→BC3.
**Anything else → 2×2 MAGENTA** placeholder + a log line `[highcut-C4] tex slot=N UNSUPPORTED fmt=...`.
⇒ If the number used an unsupported format it would be a magenta patch, not missing. "Missing" points
at UV/sampler/blend/separate-draw, OR a format that decodes to blank.

The player PS draws sample several textures (BC1/BC3/RGBA8, all supported): e.g. 256×256 BC3, 512×512
BC3, 1024×512 BC1, 256×128 BC1, plus 2×2 RGBA8 placeholders. One of the 256×128 / smaller BC1s is a
plausible number/name strip — verify with `--png` (below).

---

## 4. Hypotheses (ranked; verify, don't assume)

1. **Hardcoded `CLAMP` sampler.** `plume_present.cpp:439` and `:756` force
   `addressU/V/W = CLAMP` for ALL textures; the guest sampler address mode (clamp/wrap/mirror) is NEVER
   read. A number/name decal whose UVs tile or sit outside [0,1] would sample the clamped edge → blank.
   **Strongest single suspect.** Fix = decode the guest sampler's clamp mode (Xenos sampler fetch
   constant / SQ_TEX_SAMPLER) into `RenderTextureAddressMode` per binding.
2. **Separate decal draw being dropped.** The number may be its own small draw (a quad on the back)
   that is (a) on a non-primary surface the split drops, (b) failing depth/stencil, (c) alpha-blended to
   invisible, or (d) not captured. Check the draw stream for a number-shaped draw after each player body.
3. **Second texture layer in the player PS not contributing.** The number could be a decal texture the
   player PS samples + blends; if its texture is a 2×2 placeholder (unbound/zero), or the blend/alpha
   kills it, the number vanishes. Inspect the player PS's bound textures + the decal blend.
4. **Alpha/blend on the decal.** If the number is alpha-tested or alpha-blended and the alpha channel is
   wrong (e.g., `k_8` coverage expanded into the wrong channel, or a swizzle), it composites to nothing.

---

## 5. Plan — RenderDoc-driven, isolate the number layer

- **C5f-1 — Is the number a separate DRAW or a texture LAYER?** Capture (`_c5dump.ps1`) and decode
  (`python tools/highcut_packet_decode.py <build_dir> --png`) to dump every draw's textures to PNG.
  Look for a number/nameplate texture (digits, or a per-team name strip). If found, note which draw +
  slot carries it. Also scan for small decal draws positioned behind the players.
- **C5f-2 — RenderDoc the player draw.** `_c5renderdoc.ps1 -Draw <player>` → Pipeline State → Fragment
  Shader → Resources: are all PS textures bound + non-placeholder? Is one a number strip? Check its
  sampler (the **address mode** — if `CLAMP` and the decal expects `WRAP`, that's hypothesis 1). Use the
  Texture Viewer to see the bound number texture content + the player's UVs (Mesh Viewer VS-Out, the
  texcoord interpolators) over the number region.
- **C5f-3 — Test the sampler hypothesis cheaply.** If hypothesis 1 looks right, temporarily hardcode
  `addressU/V/W = WRAP` (or MIRROR) at `plume_present.cpp:439/756`, rebuild, render — does the number
  appear? If yes, do it properly: capture the guest sampler clamp mode into the packet and map it
  per-binding. (Compare against how the chest crest's sampler differs — it renders, so its mode works.)
- **C5f-4 — If it's a separate draw:** find it in the stream, check its surface/depth/stencil/blend/cull,
  and why the split or a state drops it.

---

## 6. Tooling + run recipes (all build-clean; `_c5render.ps1` now renders the scene by DEFAULT)

- **Build:** `_build_beta.bat` → `BUILD_EXIT=0` (vcvars64 + LLVM on PATH). NOTE: building via an agent's
  `cmd.exe`-from-bash was unreliable this session (vswhere/PATH) — build from a normal shell.
- **Capture:** `_c5dump.ps1` (beta-live, F10 at a 3D scene, hold ~5s) → `highcut_frame_*.bin` +
  `.count` + `highcut_resolves.bin`. Packets are **v8**; re-dump after any packet bump.
- **Replay:** `_c5render.ps1` — DEFAULT now renders the correct scene (cull fixed; do NOT pass
  `-FlipFace`). Flags: `-MinDraw N -MaxDraw M`, `-PrimaryPitch/-PrimaryDepth` (pick surface), `-NoSplit`
  (all surfaces to one RT — but this POLLUTES the shared depth/stencil with prepass/mask draws → black;
  the split is needed), `-NoCull`, `-Offscreen`.
- **Offline decode:** `python tools/highcut_packet_decode.py <build_dir> [--png]` — per-draw state,
  surfaces, resolves; `--png` decodes textures (use this to FIND the number texture).
- **RenderDoc:** `_c5renderdoc.ps1 [-Draw N]` — Vulkan layer via env (no admin). Focus the
  `NHL high-cut (plume Vulkan)` window → F12 → `%TEMP%\RenderDoc\*.rdc` → qrenderdoc.exe. Pipeline State
  → Fragment Shader → Resources shows the PS textures + samplers (the address-mode field is the key for
  hypothesis 1).

---

## 7. Dead-ends / gotchas (do NOT re-investigate)

- **Skinning works.** Bone palette set2 is correct. The "explosion" was primitive restart, not skinning.
- **Primitive restart + cull are FIXED** (`f415117`). Don't reopen them.
- **Translator is verbatim-correct** vs upstream Xenia. Not the bug.
- **`color_exp_bias`=1, texture `exp_adjust`=0, interpolators healthy** — all verified for player draws.
- **`-NoSplit` is black** (prepass/mask draws pollute the shared depth/stencil) — use the default split.
- **Unsupported tex formats → 2×2 magenta** (visible). A MISSING number is not an unsupported-format
  issue; look at sampler/UV/blend/separate-draw.
- **Agent `cmd.exe` builds were flaky** — verify the exe actually rebuilt (mtime) or build from a real
  shell.

## 8. Key files
- Sampler creation (CLAMP hardcode): `gpu/hooks/plume_present.cpp:439` (xlat sampler), `:756`
  (PS/VS texture sampler in `createTextures`). PS textures bind to **set3**.
- Texture untile + format → magenta fallback: `renderer/core/nhl_command_processor.cpp` ~2066-2162.
- Packet `TexturePacketDesc` (width/height/tex_format/swizzle/fetch_slot): `gpu/hooks/highcut_draw_packet.h`.
- Guest sampler decode (for the real clamp mode) lives where the CP reads `SQ_TEX_SAMPLER` /
  `xe_gpu_texture_fetch_t` — search the command processor for the sampler/fetch-constant reads.
- Decoder: `tools/highcut_packet_decode.py` (`--png`).
- Memory: `[[highcut-c5-skinning-solved-ps-dark]]`, `[[highcut-c-plume-renderer]]`.
