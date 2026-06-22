# How NHL 12 Feeds Textures & Shaders — and the Equipment Corruption Root Cause

**This doc does two things:**
1. Documents *how* NHL 12 actually feeds texture and shader data — the equipment
   material stack, mipmaps/LODs, the DB recolor "weights", fetch constants, and the
   runtime compositing path.
2. Gives a **corrected, evidence-grounded root-cause diagnosis** of the green/black
   speckle on goalie pads, helmets, and gloves, with a decisive experiment that ends
   the guessing and a fix plan per branch.

It is written against, and partly *corrects*, the prior investigation in
[`../nhl12_renderer_system_notes.md`](../nhl12_renderer_system_notes.md),
[`../renderer_investigation_blue_texture_bug.md`](../renderer_investigation_blue_texture_bug.md),
and [`../nhl12_equipment_db_color_notes.md`](../nhl12_equipment_db_color_notes.md).
Where this doc disagrees with those, the disagreement is called out explicitly with
the evidence.

> **Evidence base for this doc:** the runtime binding log
> `build/nhl12_normalplay_equipment_sampler.log` (2,856 `[NHL-TEX]` entries, 1,594
> `[NHL-SAMPLER]` entries) and its analyzer output
> `build/nhl12_normalplay_equipment_sampler_analysis_latest/log_analysis.json`,
> cross-referenced with the offline RX2 catalog. `[RT]` Confidence markers per
> [`../README.md`](../README.md) §3.

---

## Part 1 — How equipment textures & shaders are fed

### 1.1 Equipment is a multi-map *material stack*, not one texture (CONFIRMED)
Each piece of hard equipment (goalie pad, blocker, trapper, glove, helmet, skate) is
rendered from a **stack of maps**, confirmed by the extracted asset families and the
compiled `.fxo` sampler names `[INV][P4-asset]`:

| Map (suffix) | Purpose | On-disk format (CONFIRMED) |
|---|---|---|
| `_dm` diffuse/base | base albedo | `k_DXT2_3` 512×512, **base-only (1 mip)** |
| `_tm` template / tint-mask | per-texel **zone id** for recolor | `k_DXT1` 512×512, **base-only (1 mip)** |
| `_nm` normal | wrinkles/shape | `k_DXN` (BC5) 512×512, **10 mips** |
| `_sm` shine/spec | shadow/reflection response | `k_DXT1` 128×128, 8–9 mips |
| `_am` alpha/aux | cutout/aux | `k_DXT1`, 7–8 mips |

Helmet shaders (`helmet*.fxo`) additionally expose `diffuseLogoSampler[1..3]`,
`diffuseFontSampler`, `aoSampler`, `normalSampler`, `specSampler`, `envSampler`; the
recolor variants (`helmet*_cz.fxo`, `equipment*_cz.fxo`) expose `recolor`,
`recolor_detail`, `recolor_template1`. So a helmet is shell-color (from DB) + logo +
font + AO + normal + spec + environment-cube — **composited in-shader**, not a single
diffuse. `[INV]`

### 1.2 The DB drives per-zone colors ("weights") (CONFIRMED)
Equipment color is **data-driven from the roster DB**, not baked into textures.
`nhlng-meta.xml` defines `exhibitiongoalieequipment` with up to **9 colour zones per
piece**: `padszone1color_r/g/b … padszone9color_r/g/b`, `blockerzone1..9`,
`trapperzone1..9`, `stickzone1..3`, plus `showcustomcolors`/`showcustomstick`. `[P4-asset]`

The compositing model (INFERRED from the maps + shader samplers):
- the `_tm` **template mask** encodes, per texel, *which zone (1..9)* that texel
  belongs to (the "weight"/selector);
- the shader (or a runtime pass) **looks up the DB zone colour** for that zone and
  multiplies it against the `_dm` diffuse, modulated by `_sm` shine and `_nm` normal.

This is why the static `_tm`/`_dm` maps look near-monochrome offline — the *colour*
comes from the DB at runtime, selected by the mask.

### 1.3 Mipmaps & LODs (CONFIRMED behaviour, with a critical subtlety)
- Real mipped maps (`_nm`, `_sm`) **must keep their mip chains**; dropping them makes
  close-up/replay sampling read the wrong level. RexGlue preserves them
  (`nhl_preserve_equipment_mips_without_forced_aniso`). `[INV]`
- Xenos packs small mip tails; base-only BC maps must **not** be uploaded through
  packed-tail addressing (`nhl_fix_base_only_packed_bc_textures`). `[INV]`
- Some fetches set `mip_min_level > 0` (a mip-only view); RexGlue rebases so the
  requested mip becomes logical LOD 0 (`nhl_rebase_mip_min_textures`), and for
  explicit-LOD shader fetches subtracts the rebase offset. `[INV]`

> **The subtlety that turns out to be the whole ballgame (see Part 2):** the
> equipment maps **on disk** are *base-only* at 512×512 (`_dm`, `_tm`). But the
> equipment textures **bound at runtime** are 512×512 **with a full 10-level mip
> chain** (`mips=0..9`). A base-only source cannot *be* a 10-mip binding. Therefore
> the bound texture is **generated at runtime**, not loaded from disk. `[RT]`

### 1.4 Fetch constants are the source of truth (CONFIRMED)
The renderer keys every texture off the Xenos **fetch constant** (`fetch_dw=...` in
the log): base/mip address, format, dimension (2D/stacked/cube), pitch, tiled bit,
packed-mips bit, mip window, swizzle, and **sign** (signed vs unsigned). Filename
heuristics are unreliable — the same `_tm` family appears as `k_DXT1`, but jersey
"normal" maps are `k_DXT4_5` and goalie normals are `k_DXN`. Always trust the fetch
constant. `[INV]`

### 1.5 Signed/unsigned twin SRVs (CONFIRMED — and a known red herring)
The translated shader declares **two SRV slots per texture** — one signed, one
unsigned — and samples only the one matching the runtime `TextureSign`. In the log
this shows as paired entries: the *unused* twin is bound to the null descriptor
(`null=true desc=20`). **This `null=true` is expected and is NOT corruption.** Codex's
analyzer correctly demotes it to a note; don't chase it. `[RT]`

### 1.6 The special paths (CONFIRMED)
- **Stacked Texture3D:** NHL12 uses `tfetch3D` as *stacked 2D* for a colour-lookup
  path (`nhl_force_stacked_texture3d`). Shader translation and the D3D12 descriptor
  must agree (2D-array, not true-3D), or player textures go flat-wrong. `[INV]`
- **Cube fetches:** environment reflection (`tfetchCube`, e.g. the
  `fetch=3 k_8_8_8_8 128x128x6` cube in the log). NHL12 sometimes points a cube fetch
  at a 2D material map; RexGlue sanitizes cube RGB (clamp→neutral→cap 0.25, alpha=1)
  to stop a blue/green reflection flood. **This is the helmet-reflection path, which
  is separate from the pad speckle** (see Part 2, H4). `[RT][INV]`

---

## Part 2 — Why the equipment corrupts (corrected diagnosis)

### 2.1 The symptom (CONFIRMED from screenshots)
Fine, high-frequency **green/black (sometimes purple) speckle** localised to **hard,
dark equipment**: goalie leg pads, helmet shells, glove/blocker backs. Jerseys,
faces, skin, ice, boards, and crowd render **clean**. The artifact is strongest on
surfaces that should be near-black.

### 2.2 The two facts that rewrite the diagnosis (CONFIRMED `[RT]`)

**Fact A — the on-disk equipment maps bind *correctly* at runtime.**
The analyzer reports **406 strict catalog matches**: the smaller equipment maps
(`k_DXT1` 128×128 mips0..7, 256×256 mips0..8, `k_DXN` normals, `k_DXT1` shine) bind
with `compatible=true`, correct format/size/mips/pitch/sign. The static bytes were
also proven clean offline (2,886 RX2 passes, 0 failures, upload-math byte-identical).
**The on-disk assets are not the problem and have been exhaustively cleared.**

**Fact B — the textures actually *sampled* for equipment are runtime-GENERATED.**
The same log shows equipment-sized, **fully-mipped** bindings that match **no** disk
asset (`match_type: none`): `k_DXT1 512×512 mips=0..9`, `512×256 mips=0..9`,
`512×1024 mips=0..10`, `k_DXN 512×256 mips=0..9`, `k_DXT2_3 512×128`, etc. Of 2,856
runtime texture entries, **1,019 are unmatched**. A 512×512 **10-mip** DXT1 cannot be
the base-only (1-mip) on-disk `_tm`/`_dm` map — **the game composites the recoloured
equipment texture and generates its mip chain at runtime**, and *that* generated
texture is what the gameplay/close-up shaders sample.

> **This is the correction.** The prior investigation kept proving the *on-disk* maps
> clean (true) and adding *host-side* texture-cache guards. But the corrupted thing is
> the **runtime-generated recolour composite**, which no host-side asset/upload guard
> can fix because it is produced *by the game*, in guest memory, at runtime. That is
> precisely why ~9 NHL-specific host guards reduced but never eliminated the speckle.

### 2.3 Why GREEN specifically (CONFIRMED mechanism)
The equipment colour/template maps are **DXT1 (BC1)**, whose colour endpoints are
**RGB-565**. Green has **6 bits**; red and blue have **5**. Under *any* corruption of
the block bytes — garbled endpoints, a one-block address misalignment, a bad
2-bit-per-texel selector, or wrong mip data — the reconstructed colour is **biased
toward green** (the extra green bit dominates), with black where selectors collapse.
On a **dark** surface (black pads/helmet) this reads as the exact **green-on-black
speckle** seen. (BC5/`k_DXN` normals also reconstruct G as a primary channel, adding
green where normals are wrong.) So "green speckle on dark equipment" is the textbook
signature of **corrupt BC block data in the generated equipment texture** — not a
tint, not a reflection, not a colour-space issue.

### 2.4 Ranked root-cause hypotheses

**H1 — Recompiled guest code that builds the recolour composite / mip chain is
mistranslated. (LEADING — has direct precedent.)**
The composite + DXT (re)assembly + mip downsample is done in *guest* code, which is
**recompiled**. There is documented precedent that recompiled guest code produces
*exactly this symptom*: Release builds needed `-fno-strict-aliasing` and
`-ffp-contract=off` to stop **"blue/green player and equipment corruption"** that
Debug didn't show — i.e. guest-memory reinterpretation / FP contraction in the
recompiled code **already caused this class of bug once.** The composite/mip path is
saturated with vector pack/convert instructions (`vcsxwfp`×2763, `vupkhsb128`×684,
`vcfpsxws`×420, `vupkd3d128`×184, `vpkd3d128`, `vpkshus128`, `vctuxs`… across the
generated TUs). Codex already found NHL12 uses `vpkd3d128 …,5,2,2` and tested **one**
related codegen fix (didn't fix it) — the right instinct, wrong/insufficient scope.
**If any pack/convert/saturate emitter used in the equipment-composite or mip-downsample
function is wrong, the generated BC texture is garbage → green speckle — and every
host-side guard is blind to it.** This best explains: equipment-only (only equipment
uses the runtime recolour composite), Debug/Release sensitivity, and the
generated-texture evidence.

**H2 — The composite is a GPU render-to-texture pass that RexGlue mis-resolves.**
If NHL composites the recolour on the GPU and resolves it into a tiled/packed BC
texture, an EDRAM-resolve/tiling/format error would corrupt the generated texture.
Plausible but secondary (BC is usually CPU-assembled). Distinguishable from H1 by the
dump test (§2.5): H1 corrupts in guest RAM before any GPU op; H2 corrupts only after
the resolve.

**H3 — A DXBC translation bug in the recolour shader (`*_cz.fxo`), not the cube path.**
If the generated texture is clean but the `recolor`/`recolor_detail`/`recolor_template1`
sampling math is mistranslated (wrong swizzle/sign/explicit-LOD on the template
selector), the shader picks wrong per-texel zone colours → speckle. Separate from the
cube sanitizer. Distinguishable by the dump test.

**H4 — Cube reflection contribution. (REAL but SEPARATE and already mitigated.)**
`tfetchCube` caused the *blue flood* and the close-up *reflection* tint; the sanitizer
(clamp→0.25, alpha=1) fixed that and was user-confirmed on 2026-06-14. It is the
helmet **reflection**, not the pad **speckle**. Do **not** re-attribute the residual
speckle to the cube path — the binding log shows the speckle-prone equipment is the
generated BC material binding (H1/H3), and the cube path is already clamped. Keep the
sanitizer; it is not the remaining bug.

### 2.5 The decisive experiment (this is the step that ends the guessing)
The entire reason this bug has survived ~10 fixes is that **nobody localised whether
the runtime-generated equipment texture is corrupt at *generation* or at *sampling*.**
One test settles it:

> **Dump the runtime-generated equipment texture from guest memory and decode it
> offline.** During a broken close-up (goalie pad), take a known bad fetch from
> `[NHL-TEX]` (an equipment-sized `match_type: none` binding, e.g. `k_DXT1 512×512
> mips=0..9` at its `base=`), read `width×height` of BC bytes straight from guest RAM
> at `base`, untile, and decode with the existing `tools/nhl12_texture_proof.py`
> decoder.

- **If the dumped runtime bytes are already green/garbage → corruption is in
  GENERATION (H1 or H2).** Then bisect generation:
  - Build **Debug** (or the specific composite TUs at `-O0 -fno-strict-aliasing
    -ffp-contract=off`) and repeat. **Debug-clean / Release-corrupt ⇒ H1 confirmed:**
    a recompiled vector-pack/convert/saturate op in the composite/mip function is
    wrong, or those TUs need the safety flags. Fix by (a) extending the
    `-fno-strict-aliasing -ffp-contract=off` flag set to the composite TUs, then if it
    persists (b) auditing every `vpkd3d128`/`vupkd3d128`/`vcsxwfp`/`vcfpsxws`/`vctuxs`/
    `vpkshus128` emitter against a reference Xenon implementation, focusing on the
    function that writes the generated equipment texture (find it by the store address
    range matching the bound `base`).
  - **Debug also corrupt ⇒ H2:** the corruption is in the GPU resolve/tiling of the
    composite RT → BC texture; debug RexGlue's resolve/tiling for that resource.
- **If the dumped runtime bytes are CLEAN → corruption is in SAMPLING/SHADING (H3).**
  The generated texture is fine but the recolour shader samples it wrong. Dump the
  translated DXBC for the `*_cz.fxo` recolour shader, verify the template/detail fetch
  swizzle/sign/LOD, A/B with the DXBC modification bit, and fix the translator.

This is a **2-outcome → 4-leaf** decision tree. Each leaf has a concrete fix. It will
terminate in the root cause instead of adding another speculative flag.

> **The recolor code is now pinned — and there is a step-by-step FIX PLAYBOOK.**
> See **[`../re/equipment-recolor/`](../re/equipment-recolor/README.md)** §4. The RE
> *refined* the picture above: recolour is **in-shader** (`equipment_cz.fxo` +
> `RecolorCBuffer`), and the per-frame draw **`sub_82BB98B8`** binds **4 on-disk maps**
> (diffuse / template / **DXN normal** / shine) with **no colour packing in guest
> code** — so the "runtime-generated texture" angle is secondary (much of the
> `match_type:none` count was simply families the catalog didn't cover, e.g.
> helmet/logo). The speckle is provably a **per-texel map/shader** problem, and the
> **leading suspect is the DXN/BC5 normal map** (equipment-only; jerseys use BC3 and are
> clean; BC5 packs into R,G → green). Pinned entry points: `sub_82BB6CF8` (input-handle
> cache), `sub_82BBB670`/`sub_82BBD868` (inits), `sub_82BB98B8` (render/draw),
> `sub_82BAB7F0`+siblings (`equipment_cz.fxo` build) — all in `nhl12_recomp.52.cpp`.
> Follow the playbook's STEP 0→1→2 routing to land on one RexGlue file.

### 2.6 Highest-probability single fix (if you want the bet)
Based on the precedent (Debug-correct, Release-needs-safety-flags), the
equipment-only locality, and the generated-texture evidence: **H1 — a recompiled
vector-pack/convert miscompile in the runtime equipment-composite/mip path.** The
highest-EV action before anything else:
1. Identify the guest function that writes the bound generated equipment texture
   (store-address range == bound `base`).
2. Compile its TU(s) with `-O0 -fno-strict-aliasing -ffp-contract=off` and retest.
3. If still corrupt, diff its `vpkd3d128`/`vupkd3d128`/`vcsxwfp`/`vcfpsxws` emissions
   against XenonRecomp/RexGlue reference semantics for the exact pack modes used
   (`…,5,2,2` etc.) — these are the DXGI-pack modes used to assemble colour/BC data.

---

### 2.7 Applied source-side guard (2026-06-15)
The first concrete H1 patch is intentionally narrow:

- `app/generated/default/nhl12_recomp.20.cpp` is now compiled with
  `-O0 -fno-strict-aliasing -ffp-contract=off`.
- This TU contains dense vector image/composite/downsample loops
  (`vupkhsb128`, `vcsxwfp128`, `vpkshus128`, `stvewx128`) around the
  `sub_827F8C80`, `sub_827FBB48`, `sub_827FD2C0`, and `sub_82805AB8` clusters.
  That shape matches runtime material generation/mip work far more closely than
  ordinary gameplay logic.
- The patch does not change cubemap reflection handling, texture-cache matching,
  BC decompression, MSAA, or the VP6/movie path.
- `tools/nhl12_texture_proof.py --nhl12-regression-suite` now fails the source
  contract if this `nhl12_recomp.20.cpp` compile guard is removed.

If this fixes the normal-play pads/gloves/helmet speckle, the root cause is a
Release optimizer interaction in the guest equipment generator. If it does not,
the next step remains the dump test from section 2.5, followed by auditing the
exact pack or store instruction in the function that writes the bad texture base.

## Part 3 — What to STOP doing (so effort stops being wasted)

- **Stop adding host-side texture-cache guards before running §2.5.** The on-disk
  assets and upload path are *proven clean and matched*; more guards there cannot fix
  a guest-generated texture. (Keep the existing guards — they fixed real *adjacent*
  bugs — but the residual speckle is not in that layer.)
- **Stop re-proving the static RX2 bytes.** 2,886 passes, 0 failures, byte-identical
  upload. That question is closed. The unmatched 1,019 runtime entries are the
  frontier, not the matched ones.
- **Stop attributing the speckle to the cube path.** It's clamped; the residual is the
  generated BC material binding.
- **Don't disable reflections, force block-decompression, or zero cube fetches** —
  all tried, all regress other things (transparent gloves, lost reflections), none fix
  the speckle.

## Confidence
- **CONFIRMED:** the material-stack composition, the DB zone model, the mip facts, the
  signed/unsigned twin SRVs, and **Facts A & B** (on-disk maps bind clean; the sampled
  equipment textures are runtime-generated and mostly unmatched) — all from the
  binding log + offline catalog.
- **CONFIRMED mechanism:** green = DXT1/565 green-bit dominance under BC corruption on
  dark surfaces.
- **INFERRED (ranked):** H1 leading, with strong precedent; H2/H3 secondary; H4
  separate and already mitigated. The §2.5 dump experiment converts this from inference
  to proof — it has **not yet been run**, and is the single most valuable next action.

## Cross-references
- Pipeline overview: [`rendering-pipeline.md`](rendering-pipeline.md)
- Shader/texture host path: [`shaders-materials.md`](shaders-materials.md)
- Prior investigation (authoritative for what was tried):
  [`../nhl12_renderer_system_notes.md`](../nhl12_renderer_system_notes.md),
  [`../renderer_investigation_blue_texture_bug.md`](../renderer_investigation_blue_texture_bug.md),
  [`../nhl12_equipment_db_color_notes.md`](../nhl12_equipment_db_color_notes.md)
- Why guest miscompiles cause this: [`../recompilation/xenondecomp-notes.md`](../recompilation/xenondecomp-notes.md) §6,
  [`../quirks/gotchas.md`](../quirks/gotchas.md) §B
- Asset formats: [`../assets/asset-pipeline.md`](../assets/asset-pipeline.md)
