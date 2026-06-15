> ## ⚠️ UPDATE 2026-06-15 — READ THIS FIRST (changes the diagnosis below)
>
> The §3 "composited but discarded before the framebuffer / layering bug" conclusion is **most
> likely WRONG**. New evidence: in the current capture the number's source textures are simply
> **not resident** — the font glyph atlas is uniform **magenta** (the game's missing-texture
> placeholder; genuine guest memory, not our 2×2 stub) on the St. Louis jersey, and the
> name/coverage/positioning map (slot6) is **exactly uniform `(255,255,0,255)`** on BOTH player
> jerseys (no structure in any channel). With no glyph source and no positioning data, the number
> can't render no matter how correct the compositing is. Every §3 "proof" is equally explained by
> these missing inputs (forcing an already-magenta font / already-yellow map → "no change"). A
> 12-register `_c5probe -Step` on a jersey showed the number in **none** of the 12 registers —
> consistent with starved inputs. The user confirmed numbers DO render live, so our F10 captures
> just grab a frame before the assets page in.
>
> **New procedure (use the toolchain built this session):**
> 1. Re-capture (`_c5dump.ps1`) with jersey numbers clearly **ON-SCREEN** at F10.
> 2. `python tools/_check_residency.py <build_dir>` — must print **PASS** (no magenta/uniform shared
>    atlases). If FAIL, re-capture; do NOT debug a FAIL capture.
> 3. `.\_c5probe.ps1 -Draw <jerseyDraw> -Step` — dumps registers[0..11] of that draw, isolated, to
>    `_probe_d<N>_reg*.png`. The number should appear in ONE register. If a LATER register drops it
>    → THAT is a genuine compositing bug (then §3's approach applies). If it shows correctly →
>    fixed by residency.
>
> **Toolchain (all built + verified 2026-06-15):**
> - `tools/_check_residency.py <dir>` — residency PASS/FAIL preflight.
> - `tools/_instrument_ps.py <in.ps.spv> <out> --reg K | --ssa %ID | --step [--alpha]` — force the PS
>   fragment output = `xe_var_registers[K]` (or an SSA id); emits valid SPIR-V.
> - plume hooks: `NHL_HIGHCUT_DEBUG_PS=<spv>` + `NHL_HIGHCUT_DEBUG_PS_DRAW=<idx>` swap one draw's PS;
>   `NHL_HIGHCUT_C5_SHOT=<png>` one-shot framebuffer→PNG readback (verify a headless replay without
>   eyeballing the window). Plume RHI gained an image→buffer copy branch + swapchain TRANSFER_SRC.
> - `_c5probe.ps1 -Draw N [-Reg K|-Ssa %ID|-Step] [-Alpha] [-Full]` — extract→instrument→replay→PNG
>   in one command (isolates the draw by default).
> - `tools/_scan_player_draws.py <dir>` — group player über-draws by PS hash + list their textures.
>
> Full detail: memory `[[highcut-c5g-jersey-number-residency]]`. The rest of this doc is the prior
> (pre-2026-06-15) understanding — keep for the §5/§6 mechanics, but treat §3/§4 as superseded.

---

# High-cut C-5g — jersey back NUMBERS (self-contained kickoff for a cold session)

> **Mission:** player **jersey back numbers** don't render in the high-cut plume replay. Everything else
> in the gameplay scene now does — rink, players, jerseys, crests, helmet decals, **referee numbers**, and
> correct surfaces/reflections. This session fixed three real bugs and *precisely localized* the number
> bug to the über-shader's composition. Read §3 (what we PROVED) before touching anything.
>
> Prior context: `docs/highcut-c5-jersey-numbers-prompt.md` (the session that started this),
> `docs/highcut-c-plume-renderer-plan.md`. Memory: `[[highcut-c5f-jersey-number-sampler-fix]]`,
> `[[highcut-c5-skinning-solved-ps-dark]]`, `[[highcut-c-plume-renderer]]`.

---

## 1. The asset model (authoritative — confirmed by the user, who knows the NHL asset structure)

A player **jersey** composites the number at runtime as the **TOP layer** over the jersey:

| Asset | Format | Role | Our slot (varies per capture) |
|---|---|---|---|
| jersey **colormap** | **DXT1** (`k_DXT1`→BC1) | base jersey color + crest + stripes baked in | a 1024² / 512² BC1 |
| **`font_#_#_#_cm`** glyph atlas | **DXT5/BC3** (`k_DXT4_5`→BC3) | the gold 0-9 / A-Z digit+letter glyphs (one shared atlas) | a 512² BC3 (the obvious gold-digits texture) |
| **`name_#_#_#_cm`** | **A8R8G8B8** (`k_8_8_8_8`→RGBA8) | a **non-visible COVERAGE / POSITIONING map**: where the number/name goes. **YELLOW (B=0) = "draw the glyph here", WHITE (B=255) = background.** | a 256² RGBA8 (there are TWO 256² RGBA8 in the draw) |

So the number = font glyphs, stamped where the name map says, on top of the jersey. **NOT** baked.

**Referee** numbers are **baked into the ref jersey texture** (a different system) — that's why the ref's
"15" renders trivially and tells us nothing about the player path. **Numbers also render on HELMETS**
(font+name-ish) but **not on jerseys** — the helmet-vs-jersey asymmetry is the best lead (see §5.1).

---

## 2. What's FIXED this session (committed; do NOT redo or reopen)

All three were the **same failure mode**: `untileBindings` (in `nhl_command_processor.cpp`) stubs any
texture whose **format or dimension** we don't handle as a **2×2 magenta placeholder**, silently breaking
whatever material sampled it. The fixes:

- **C-5f — per-binding sampler filter/clamp** (packet **v9**, `SamplerPacketDesc`). The jersey nameplate
  LAYOUT map is **POINT**-sampled by the guest; we were forcing LINEAR on all PS textures. Now we honor
  each `SamplerBinding`'s guest filter+clamp (resolved from the fetch constant). `plume_present.cpp` builds
  one `RenderSampler` per binding. Confirmed working (players stopped exploding = VS bone-palette POINT).
- **C-5g — CUBE maps** (packet **v10**, `TexturePacketDesc.array_layers`). The env reflection cube was a
  2×2 placeholder bound to a cube descriptor = garbage; NHL materials use the cube **alpha as a material
  factor** (per a parallel NHL12 D3D12 decomp's docs). `untileBindings` detects `FetchOpDimension::kCube` →
  emits a 6-face cube (currently NEUTRAL dark + **alpha=1**, bring-up); plume builds a real
  `RenderTextureFlag::CUBE` texture + `TextureCube` view. **Result: helmet decals + reflections returned.**
  (TODO: replace the neutral cube with real untiled 6 faces for correct reflection color.)
- **C-5g — BC5 normal maps** (`kTexBC5`→`BC5_UNORM`). `k_DXN` (format 49, 804 stubbed bindings) is BC5
  two-channel normal maps; was stubbed → flat magenta normals → broken specular. Same untile path as BC3.
  **Result: surfaces shade correctly, referee number renders.**

Packet is now **v10**. The python decoder (`tools/highcut_packet_decode.py`) handles v10 (`TEX_DESC_V10="<10I"`).

---

## 3. The remaining bug — PROVEN diagnosis (don't re-derive)

**The jersey number is composited in the player über-shader but its result NEVER REACHES THE FRAMEBUFFER —
it is painted over / discarded by a later composition stage.** This is the user's "layering" point: the
number should be the top layer, and something is on top of it. It is **NOT** a texture, format, positioning,
or mask problem — all ruled out below.

How the number composites in the über-shader (traced from a `ps_tex=20`, surface depth=736 player draw;
SSA IDs below are from one capture's PS — **they renumber every capture, re-extract**):
- `registers[7]` = base jersey (slot1 / DXT1 sample).
- font (slot8 / BC3 atlas) sampled with **explicit LOD** (the unique `xe_sampler8_fff_a0`) at a UV computed
  from the name map (slot6) + the **dynamic-addressed** float constants `float_constants[addr+104]`
  (addr = `xe_var_address_register` = the per-character index). This is the digit-placement machinery.
- blend: `registers[8] = base + (font_rgb − base) × mask`. (The `mask` = `registers[1].y` in the trace —
  but see the caveat: it may be the name-map coverage, not the font alpha. Texture forcing couldn't pin it.)
- `registers[8]` is later read at `_3086` (squared = sRGB→linear) and fed into the lighting; the final
  output = `sqrt(registers[0].yzw)`.

**Empirical proof it doesn't reach the output** (texture-patch the live v10 capture in place, `_c5render`,
NO redump — see §6 for the patch recipe):
- font atlas recolored solid magenta (keeping glyph alpha) → **no number**.
- font atlas **fully opaque** magenta (force mask-from-font = 1 everywhere) → **no change**.
- name map (both 256² RGBA8) forced all-**white** (= "background / draw nothing") → **no change**.
- name map forced all-**yellow** (B=0 = "draw the glyph everywhere") → **no change**.

The jersey number is **completely insensitive** to the font's color, the font's alpha, AND the name-map
content. If any of those fed a live path to the framebuffer, *something* would have changed. Nothing does
⇒ the entire number-compositing path dead-ends before the output.

One earlier claim was **DISPROVEN**: "the atlas samples gold, so the number is in the albedo and the
lighting eats it." That rested on `_1266` (the font texture read = gold) — an **intermediate** register,
not a rendered pixel. A direct RenderDoc read of `registers[8]` (the composited albedo, right after the
blend) measured **= base color**, not gold, at the test pixel. So the number is NOT making it into the
albedo that reaches the output.

---

## 4. Things RULED OUT (do NOT re-investigate)

- **Texture formats / stubs** — full format scan of all player draws: every texture maps correctly
  (DXT1→BC1, DXT5→BC3, A8R8G8B8→RGBA8, k_DXN→BC5, cube→ok). No stubs remain.
- **POINT sampling, cube, BC5** — fixed (§2). The fonts/normals/reflections are correct now.
- **Name-map content / positioning / coverage** — forced to every value (white, yellow, all-0xFF), no change.
- **Font content / alpha** — forced opaque magenta, no change.
- **System constants** — the PS reads members 0/17/19/23; all correct or don't gate the number
  (`texture_swizzled_signs` legitimately 0 since guest texture signs are 0).
- **Float constants** — full 256-entry bank provided for the dynamic-addressing PS; `float_constants[addr+104]`
  digit-placement data is populated and meaningful.
- **Referee numbers** — baked into the ref texture, a different system; irrelevant to the player path.
- **Caveat on the yellow test**: it assumed the name-map "blue" channel = blob byte 2. If the A8R8G8B8
  byte order through untile+endian differs, that one test is non-conclusive — but the opaque-magenta-font
  test independently supports the "doesn't reach output" conclusion.

---

## 5. Proposed next steps (ranked)

### 5.1 — Diff the HELMET PS vs the JERSEY PS (fastest; START HERE)
Numbers render on **helmets** but not jerseys. If both use the font+name composite, diff their translated
pixel shaders to find the **extra layer / predicate that covers the jersey number**. The blocker is just
**identifying which draws are helmets vs jerseys** — ask the user, or:
- Decode candidate draws' base texture (slot1, the DXT1) to PNG and eyeball (helmet vs jersey).
- Helmet draws and jersey draws may be the SAME `ps_tex=20` über-shader (then the difference is data/UV,
  not the shader) or DIFFERENT shaders (then diff reveals the covering layer). Confirm which first.
- Extract both PSes (`tools/_extract_spirv.py`), `spirv-dis`, diff. Look for a blend/`OpStore` to the
  albedo register AFTER the font blend that uses the base (not the number) — that's the cover-up.

### 5.2 — SPIR-V instrument the jersey PS (definitive)
Read the number layer directly, bypassing all texture/mask/channel ambiguity:
- Extract the jersey PS, `spirv-dis`. Find the **font-blend store** to `registers[8]`
  (`OpStore <ptr to xe_var_registers[8]> <blend result>`, right after the font `ImageSampleExplicitLod`).
- Add a `Function` debug variable, `OpStore %dbg <blend result>` right after it, and at the end redirect
  `OpStore %xe_out_fragment_data_0` to `OpLoad %dbg`. Reassemble (`spirv-as`).
- To replay it without repacking the packet: add a small **plume override hook** — an env var
  (e.g. `NHL_HIGHCUT_DEBUG_PS=<path.spv>` + `NHL_HIGHCUT_DEBUG_PS_DRAW=<idx>`) that makes
  `BuildRenderableDraw` load that `.spv` instead of `hdr.ps_spirv` for the chosen draw.
- Render one jersey draw isolated (`-MinDraw N -MaxDraw N+1`). If the number SHAPE appears in `registers[8]`
  → it IS composited and a later stage discards it (then walk forward storing later registers to find which
  stage covers it). If it's flat base → the blend itself never stamps the glyph (re-examine the mask source).

### 5.3 — Compare against the BETA (DXBC/D3D12) execution
Beta renders the number (real game). Beta = DXBC, high-cut = SPIR-V (different translators). If 5.1/5.2
show the same shader composites differently, the divergence is in the SPIR-V translation/execution of the
number-blend or a predicate. Beta has RenderDoc support; capture the same player draw and compare the
albedo register flow.

---

## 6. Tooling + recipes

- **Build:** `_build_beta.bat` → `BUILD_EXIT=0` (vcvars64 + LLVM on PATH; build from a real shell, agent
  cmd.exe-from-bash was flaky).
- **Capture (user, interactive):** `_c5dump.ps1` → drive to a STATIC face-off → **F10** → hold ~5s. Beta-
  live FLAT path dumps every owned draw to `highcut_frame_<N>.bin` (+ `.count`, + `highcut_resolves.bin`).
  Packets are **v10**; re-dump after any packet bump. **A redump wipes any texture-patch test edits.**
  NOTE: the capture catches a variable frame — draw indices + texture addresses **renumber every dump**.
  Re-find the player über-draws each time (scan: `vs_tex>0`, `ps_tex>=18`, `surface_depth_base==736`).
- **Replay:** `_c5render.ps1` (default renders the scene; do NOT pass `-FlipFace`). `-MinDraw N -MaxDraw M`
  isolates draws.
- **RenderDoc:** `_c5renderdoc.ps1 [-Draw N]` (Vulkan layer via env). Quirks learned: **breakpoints /
  Run-to-Cursor are NOT reliable** in these traces; **single-step is hidden**. The reliable way to position
  the debugger is **Accessed Resources tab → double-click a "Step N Access" row** to jump to that
  instruction, then read live values in **High-level Variables** (`xe_var_registers[8]`, etc.). Run
  "Execute forwards" once first to populate Accessed Resources. Register reuse makes single-pixel reads
  ambiguous (the value at a later step ≠ the value at the blend).
- **Offline decode:** `python tools/highcut_packet_decode.py <build_dir> [--png]` — per-draw state + textures.
- **Extract a draw's shaders:** `python tools/_extract_spirv.py <build_dir>/highcut_frame_<N>.bin` →
  `.vs.spv`/`.ps.spv`. `spirv-dis` at `C:/VulkanSDK/1.4.350.0/Bin/spirv-dis.exe`.
- **Per-binding sampler dump:** `python tools/_inspect_samplers.py <build_dir> <drawIdx>` (v9 descs).
- **Per-texture clamp+filter dump:** `python tools/_inspect_clamp.py <build_dir> <surf_depth> <lo> <hi>`.
- **Format scan (find stubs) — the recurring high-value check.** For each texture, read the guest fetch
  constant `dword_1` bits 0-5 (format) + `dword_5` bits 9-10 (dimension); flag anything `untileBindings`
  stubs. (See the inline python used this session — walk `TEX_DESC_V10`, read `fetch[(slot*6+1)*4]` etc.)
- **Texture-patch test (in-place, no header change).** Read a v10 packet, walk to texture descs
  (`TEX_DESC_V10="<10I"`, fields = `TEX_FIELDS_V10`), overwrite a texture blob in place (same byte size),
  write back. Identify the target by `fetch_base_addr` (find via the gold-digits PNG) or `tex_format`.
  Then `_c5render` (NOT `_c5dump`). A redump restores everything.

---

## 7. Key files
- Texture untile + format/dimension → stub fallback: `renderer/core/nhl_command_processor.cpp`
  (`untileBindings` ~2083-2200; the cube branch + the format `switch`; the manual `spv_sys` ~2000;
  `pack_floats` ~2050; the per-sampler desc build).
- Plume replay: `gpu/hooks/plume_present.cpp` (`BuildRenderableDraw`; `createTextures` incl. the cube path;
  `buildSamplers`; `mapFmt`; `xenosSwizzleMapping`/`xenosFilter`/`xenosClamp`; the per-draw render loop
  ~1354). **This is where a `NHL_HIGHCUT_DEBUG_PS` override hook (§5.2) would go.**
- Packet format (v10): `gpu/hooks/highcut_draw_packet.h` (`TexturePacketDesc.array_layers`,
  `SamplerPacketDesc`, `PacketTexFormat` incl. `kTexBC5`).
- Decoder: `tools/highcut_packet_decode.py`. Helpers: `tools/_extract_spirv.py`, `_inspect_samplers.py`,
  `_inspect_clamp.py`.
- Memory: `[[highcut-c5f-jersey-number-sampler-fix]]` (full blow-by-blow), `[[highcut-c-plume-renderer]]`.

---

## 8. The one-line summary for the next session
> Three stub bugs fixed (POINT/cube/BC5) → rink, surfaces, ref + helmet numbers render. The **jersey**
> number is composited in the player über-shader but **discarded before the framebuffer** (proven: the
> jersey is insensitive to font color/alpha and name-map content). It's a **layering/composition** problem,
> not a texture/format/positioning one. **Start with §5.1** (diff helmet vs jersey PS) — ask the user how to
> tell a helmet draw from a jersey draw — then §5.2 (instrument the PS) if needed.
