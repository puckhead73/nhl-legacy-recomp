# RE: Equipment Color-Zone (CZ) Recolor Material — Function Map

**Purpose:** locate, in the recompiled binary, the code that feeds textures and
shaders for the recolorable hard equipment (goalie pads, helmets, gloves, blocker,
trapper) — i.e. the path responsible for the **green/black equipment speckle**. This
folder pins that path from anonymous `sub_<addr>` functions to a named, understood
subsystem so it can be breakpointed and fixed.

> **For Codex:** everything here is reproducible with `tools/find_string_xref.py`
> (string→function xrefs) and `RexGlue/tools/binutils/powerpc-none-elf-objdump.exe`.
> Addresses are CONFIRMED by disassembly; struct-offset *meanings* are INFERRED and
> marked. The diagnosis this supports is in
> [`../../graphics/textures-and-shaders-feeding.md`](../../graphics/textures-and-shaders-feeding.md).

---

## 1. TL;DR — what was found

The equipment recolor is an **in-shader Color-Zone (CZ) system**, not a
pre-generated-texture system (this *corrects* the earlier "runtime-generated texture"
lean):

- **Classes (RTTI):** `CZRenderable`, `CZHelmetJerseyRenderable`, `FERenderable`,
  each with a `RecolorCBuffer`. `[RTTI]`
- **Shaders:** `shaders/equipment_cz.fxo`, `equipment_diffuse_cz.fxo`,
  `equipment_no_ao_cz.fxo`, `helmet_cz.fxo`, `helmet_nofont_nospotlight_cz.fxo`,
  `alphaequipment_nospotlight_cz.fxo`, `jersey_cz.fxo`, `skin_cz.fxo`, `eyes_cz.fxo`,
  … (the `*_cz` family). `[STR]`
- **Recolor inputs (sampler/param names):** `diffuseSampler`, `recolor_template1`,
  `recolor_template2`, `recolor_detail`, `recolor_logo_template[2]`,
  `recolor_logo_detail`, `recolor_font_template`. `[STR]`
- **Zone colours** come from the DB (`exhibitiongoalieequipment`,
  `padszone1..9color_*`, etc.) uploaded into **`RecolorCBuffer`** constants, **not**
  baked into a texture. So recolor happens **in the `*_cz` pixel shader**, sampling
  the on-disk diffuse + template/detail maps and selecting per-texel zone colours from
  the constant buffer.

**The CZ material module is at `~0x82BAB7F0 – 0x82BBE000`, in
`app/generated/default/nhl12_recomp.52.cpp`.** This is (was) one original source file
— most likely `CZRenderable.cpp` / the color-zone material implementation.

## 2. Pinned functions (CONFIRMED addresses)

| Function | File | Role (CONFIRMED by disasm unless noted) |
|---|---|---|
| `sub_82BB6CF8` | `nhl12_recomp.52.cpp` | **CZ material input-handle cache.** Resolves `diffuseSampler` + `recolor_template1` sampler handles and 2 constant slots (ids `0x000D0001`,`0x000D0002`) into the material object at offsets `+60/+64/+72/+76/+80/+84`. See [`cz-material-functions.md`](cz-material-functions.md). |
| `sub_82BBB670` (calls cache at `+0x18`=`0x82BBB688`) | `nhl12_recomp.52.cpp` | **CZ material init #1** — zero-fills a 24-byte region at `this+172`, then calls the handle cache. Construct/init (INFERRED: `CZRenderable`). |
| `sub_82BBD868` (calls cache at `0x82BBD87C`) | `nhl12_recomp.52.cpp` | **CZ material init #2** — the other CZ material init (INFERRED: `CZHelmetJerseyRenderable`). |
| `sub_82BAB7F0`, `sub_82BABA80`, `sub_82BABE58`, `sub_82BAC4A0` | `nhl12_recomp.52.cpp` | **`equipment_cz.fxo` shader/effect build** — load the cz shader by name and construct the effect/material. Each references a distinct `equipment_cz.fxo` string. |
| **`sub_82BB98B8`** (vtable `0x8211FB54[+0x38]`) | `nhl12_recomp.52.cpp` | **CZ Render/Apply (the per-frame draw, 0x410).** Loops the sub-mesh list at `this+0x38`; per draw binds the **4 material maps** (diffuse / recolor_template1 / **normal=DXN** / shine) via `0x82ae5780/0x82ae4dc8/0x82ae2900` and submits via `0x82b284f0`. **No float/vector ops ⇒ recolour colours are precomputed params, not packed here.** See [`cz-material-functions.md`](cz-material-functions.md) §2b. |

### Shader-parameter API (the helpers the CZ material calls) — CONFIRMED targets
| Address | Inferred role |
|---|---|
| `0x82adcb38` | **get shader param / sampler handle by name** (`(effect, char* name) -> handle`). |
| `0x82adc920` | **resolve handle → slot/index** (`(effect, handle) -> {…, slot@+8}`). |
| `0x82adcaa8` | **get constant handle by id** (`(effect, u32 id) -> handle`); used for `0xD0001/0xD0002`. |

These three are the RenderWare/EA effect-parameter API. Pinning their bodies further
would name the whole material layer (open task).

## 3. How this connects to the bug

The CZ pixel shader (`equipment_cz.fxo`) does, per texel (INFERRED from inputs):
```
zoneId   = sample(recolor_template1 / recolor_detail)     // which of 9 zones
zoneColor= RecolorCBuffer.zone[zoneId]                    // DB colour (constant)
base     = sample(diffuseSampler)                         // _dm base
out      = combine(base, zoneColor, shine, normal)        // composited colour
```
The **green/black per-texel speckle** must enter at one of (in likelihood order — see
§4 for the routing):
1. **DXN/BC5 normal `_nm` reconstructed wrong** (sign/swizzle/view) → per-texel garbage
   normal → green (BC5 = R,G channels), worst on dark surfaces. *Equipment-specific:
   only equipment uses DXN; jerseys use DXT5.* ← leading.
2. **The `equipment_cz.fxo` shader mistranslated to DXBC** → speckle even with correct
   inputs (A/B against the working `jersey_cz.fxo`).
3. **`recolor_template1` sampled wrong** (explicit-LOD/mip) → garbage `zoneId` per texel.

> **Eliminated by the render-method RE:** the per-frame draw `sub_82BB98B8` has **no
> colour packing in guest code** — recolour colours are set as precomputed params, so a
> **`RecolorCBuffer` fill miscompile would produce wrong *solid* zone colours, not
> per-texel speckle.** The speckle is therefore a per-texel **map or shader** problem.

The inits (`sub_82BBB670`/`sub_82BBD868`) + handle-cache (`sub_82BB6CF8`) *wire* the 4
maps; the draw (`sub_82BB98B8`) *binds* them; the `equipment_cz.fxo` shader *combines*
them. The fault is in the bind/sample of one map (esp. the DXN normal) or the shader.

## 4. FIX PLAYBOOK — do this, in order

The pipeline is now fully pinned, so the remaining work is a **3-step routing
procedure** that ends at one exact RexGlue source file. No more speculative host flags.

### What the RE settled (so you can skip re-deriving it)
- Recolour is **in-shader** (`equipment_cz.fxo` + `RecolorCBuffer` constants), **not** a
  pre-generated diffuse. The per-frame draw **`sub_82BB98B8`** binds **4 on-disk maps**
  (diffuse `_dm` DXT2_3 · template `recolor_template1` DXT1 · **normal `_nm` DXN/BC5** ·
  shine `_sm` DXT1) and submits — with **no colour packing in guest code**.
- ⇒ The per-texel speckle is a **sampled-map or shader-math** problem, *not* a wrong
  colour constant. This **eliminates the `RecolorCBuffer` fill** as the speckle source
  and focuses everything on the 4 bound maps + the `equipment_cz.fxo` translation.

### STEP 0 — Capture the broken draw's 4 map bindings (~10 min)
Run with `--nhl_log_texture_bindings=true`; reach a broken goalie-pad close-up. In
`build/…sampler.log`, find the equipment material's 4 `[NHL-TEX]` lines (diffuse /
template / **DXN normal** / shine). For each record `base=`, `fmt`, `mip_min/max`,
`tiled`, `packed`, `signs`, `host_swizzle`, `shader_signed`, and from `[NHL-SAMPLER]`
the `final_aniso` / `preserved_nhl_mips` / explicit-LOD use. (To pin *which* draw is the
pad, breakpoint the guest render **`sub_82BB98B8`**; its bind calls are
`0x82ae5780/0x82ae4dc8/0x82ae2900`.)

### STEP 1 — The decisive dump (splits the whole problem in one shot)
Dump **the DXN normal `_nm`** and **the DXT1 `recolor_template1`** from guest RAM at
their `base=`, untile, decode with `tools/nhl12_texture_proof.py`:
- **Branch A — bytes corrupt in RAM** → texture data is wrong *before* sampling.
- **Branch B — bytes clean in RAM** → corruption is in **sampling/view** or **shader**.

### STEP 2A — bytes corrupt in RAM (guest load/codegen)
These maps are *loaded from disk*, so corrupt-in-RAM = a **recompiled guest function
mistranslated the RX2 load/untile/decode**. Precedent: Release needed
`-fno-strict-aliasing -ffp-contract=off` to stop blue/green equipment corruption.
**Fix path:** (1) rebuild the asset-decode TUs + `nhl12_recomp.52.cpp` with those flags;
if fixed → done. (2) else write-watch that `base`, find the writer, audit its
`vpkd3d128`/`vupkd3d128`/`vcsxwfp`/`vctuxs` emitters → **edit
`RexGlue/src/codegen/builders/vector.cpp`** (codegen, not a host flag).

### STEP 2B — bytes clean in RAM (sampling/view or shader) — three sub-branches:

| If the STEP 0 capture shows… | Root cause | **Fix in this file** |
|---|---|---|
| **B1** — DXN `_nm`: `shader_signed` ≠ the sampled view, or `host_swizzle` ≠ `RGGG`, or a separate signed resource | BC5 normal reconstructed wrong → per-texel garbage normal → **green** (BC5 packs into R,G) | `RexGlue/src/graphics/d3d12/texture_cache.cpp` (DXN UNORM/SNORM view + swizzle) + sign handling in `dxbc_translator_fetch.cpp` |
| **B2** — `recolor_template1` sampler uses **explicit LOD** with `rebased`≠0 / wrong offset, or `final_aniso`>0 on a mipped map | wrong mip read → block/zone speckle | `RexGlue/src/graphics/pipeline/texture/cache.cpp` (rebase) + `dxbc_translator_fetch.cpp` (explicit-LOD normalise) |
| **B3** — all 4 maps clean, draw still wrong | `equipment_cz.fxo` **shader math** mistranslated | `RexGlue/src/graphics/pipeline/shader/dxbc_translator*.cpp` — **dump the DXBC for `equipment_cz.fxo`, diff vs `jersey_cz.fxo`** (works), localise the divergent op |

> Bump the DXBC modification version after any translator change, or stale pipeline
> storage hides your fix (renderer notes).

### Ranked most-likely — commit your first attempt to B1
1. **B1 — DXN normal sign/swizzle.** Best explains *all* the evidence at once:
   **equipment-only** (jersey/body normals are `k_DXT4_5`/BC3; *only* equipment uses
   `k_DXN`/BC5), **green** (BC5 stores its two channels into R and G), and **worst on
   dark surfaces** (a wrong normal dominates the look exactly where albedo is near-black
   — the pads/helmet shells). The runtime log now **does** show NHL12 binding
   signed/unsigned DXN in the equipment draw — i.e. the binding diagnostic the prior
   notes demanded *before reopening this lead* **now exists.** Start in
   `d3d12/texture_cache.cpp` DXN view selection + the translator's TextureSign path.
2. **B3 — `equipment_cz.fxo` vs `jersey_cz.fxo` DXBC diff.** Same recolour system,
   one works → diffing the two translated shaders isolates the broken instruction.
3. **2A — guest load miscompile** (the documented precedent).

### Why this is tractable now (vs. the last ~10 attempts)
Every prior fix was a **host-side guess applied blind**. With the guest path pinned
(`sub_82BB98B8` binds exactly 4 maps; colours are *not* packed in code), the speckle is
**provably** a per-texel map/shader problem, and STEP 1's single dump routes you to one
file. The DXN-normal lead (B1) was *deprioritised before only for lack of a binding
diagnostic* — that diagnostic now exists, and the format differential
(BC5-equipment vs BC3-jersey) is exactly the asymmetry the symptom shows.

## 5. Reproduce / extend (tooling)

```bash
# string -> function xrefs (inline lis+ori/addi and data-pool words)
python tools/find_string_xref.py "shaders/equipment_cz.fxo" "recolor_template1" "CZRenderable::RecolorCBuffer"

# disassemble a pinned function
RexGlue/tools/binutils/powerpc-none-elf-objdump.exe -D -b binary -m powerpc:common64 \
  -EB --adjust-vma=0x82000000 --start-address=0x82BB6CF8 --stop-address=0x82BB6E08 \
  extracted/nhlzf_image.bin

# map a VA to its containing sub_ (bisect PPCFuncMappings in nhl12_init.cpp)
# find bl callers of an address (24-bit signed bl decode) — see snippet in cz-material-functions.md
```

See [`cz-material-functions.md`](cz-material-functions.md) for the per-function
disassembly analysis and the material struct offset map.
