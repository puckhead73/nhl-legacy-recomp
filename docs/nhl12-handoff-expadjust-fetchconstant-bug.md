# Hand-off to the NHL12 Vulkan-renderer project: the rexglue `exp_adjust` fetch-constant bug

*From the NHL Legacy (NHL14-era) high-cut renderer project, 2026-06-15. We just root-caused why our
jersey back **names + numbers** never rendered. The bug is in the **shared rexglue shader translator**,
so it almost certainly affects your build too — and it's a strong candidate for the **black/dark part
of your long-running equipment corruption.** This is a bit-exact writeup so you can verify it in ~10
minutes and fix it at the source (you build rexglue from source; we only have the prebuilt SDK).*

---

## TL;DR

The rexglue texture-fetch translator applies a texture's **`exp_adjust`** (a 6-bit signed per-texture
texel scale: `texel *= 2^exp_adjust`) but reads it from the **wrong fetch-constant dword** — it reads
**dword_4** (where `lod_bias` lives) instead of its real home, **dword_3**. For any texture with a
non-zero **`lod_bias`** (LOD-sharpened atlases — fonts, decals, crisp equipment maps), the lod_bias
bits get reinterpreted as a large-negative `exp_adjust`, so the texel is multiplied by `2^(big neg)`
≈ 0 → the texture renders **black**. Textures with `lod_bias = 0` (the majority) read `exp_adjust = 0`
→ `×1` → unaffected, which is why only *some* textures vanish.

For us this crushed the jersey **font/number atlas** (which carries `lod_bias = -0.75`) to black.
Fixing it made "BACKES 42" render. For you, check whether it explains your **equipment going
black/dark** and any missing LOD-biased **numbers / nameplates / logos / decals**.

---

## 1. Our symptom

Player jersey **back name + number** never reached the framebuffer in our renderer. The player
über-shader *did* composite them — we instrumented the pixel shader and confirmed the gold glyph
("42") was sampled correctly — but the font sample came out **black** before lighting. Base jersey,
crests, stripes, ref numbers (baked) all rendered fine. Only the runtime-composited, LOD-biased font
layer vanished.

## 2. Root cause (bit-exact)

The Xenos texture fetch constant (6 dwords, `xe_gpu_texture_fetch_t`) lays these two fields in
**different dwords** (from the shared `xenos.h`):

```
dword_3:  num_format:1 (+0)  swizzle:12 (+1)  exp_adjust:int6 (+13)  mag/min/mip filters …
dword_4:  vol filters (+0..1)  mip_min/max_level (+2/+6)  aniso_walk (+10/11)
          lod_bias:int10 (+12)  grad_exp_adjust_h:5 (+22)  grad_exp_adjust_v:5 (+27)
```

So **`exp_adjust` = dword_3 bits 13-18**, and **`lod_bias` = dword_4 bits 12-21** — different dwords,
but their bit ranges *overlap within a dword*.

The translator emits tfetch code that scales the fetched texel by `exp2(exp_adjust)`. But it reads
`exp_adjust` from **dword_4**. In our translated SPIR-V the *same* loaded dword was used for both:

```spirv
%v   = OpLoad fetch_constants[..]            ; = the texture's dword_4
%lod = OpBitFieldSExtract %v, 12, 10         ; lod_bias  -> feeds the sample LOD   (CORRECT: dword_4)
%exp = OpBitFieldSExtract %v, 13, 6          ; "exp_adjust" -> Ldexp -> texel scale (WRONG: dword_3)
%s   = OpExtInst Ldexp %float_1, %exp         ; 2^exp_adjust
%t   = OpFMul <decoded texel>, %s             ; texel *= 2^exp_adjust   <-- crushes it
```

The lod_bias read is correct (it samples the right mip — our font glyph was sharp). The **`exp_adjust`
read is from the lod_bias dword instead of dword_3.** Concretely for our font texture (fetch
constant 8):

```
dword_3 = 0x00A80D10  ->  real exp_adjust (bits13-18) = 0      (correct: no scale)
dword_4 = 0x003E8143  ->  lod_bias (bits12-21)        = -0.75  (real: slight sharpen)
                          bits13-18 of dword_4          = -12    <-- what the shader used as exp_adjust
                          2^-12 = 0.000244  ->  texel ~= black
```

`lod_bias = 0` textures have bits13-18 = 0 in dword_4 → `exp_adjust` reads 0 → `×1` → fine. That's the
whole "only some textures go black" behavior.

## 3. How to verify in your build (~10 min)

1. Find a texture that renders **black/dark** that shouldn't (a missing number/nameplate, or a dark
   equipment map). Get its fetch-constant slot.
2. Dump that fetch constant's **dword_3** and **dword_4**. Compute:
   - real `exp_adjust` = `sign_extend(dword_3 >> 13, 6)` — should be **0** for a normal material map.
   - `lod_bias` = `sign_extend(dword_4 >> 12, 10)` / 32.0 — non-zero if it's a sharpened atlas.
   - the **misread** value = `sign_extend(dword_4 >> 13, 6)`.
3. If `dword_3` exp_adjust is 0 but the **misread (dword_4)** value is a large negative AND the
   surface is black → **same bug.** (A RenderDoc pixel-debug of the tfetch will show the
   `exp2(...) * texel` collapsing the value, same as ours did.)

## 4. The fix

You build rexglue from source, so fix it at the root (it fixes both projects — please upstream):

- In the rexglue texture-fetch translation, the **per-texture exponent bias applied to the fetched
  texel** must be read from **fetch-constant dword_3 (bits 13-18)**, not dword_4. Find where the
  translator emits the `exp2(exp_adjust) * texel` (search the tfetch codegen for `exp_adjust` /
  `ExpAdjust` / `Ldexp` / `exp2`) and correct the fetch-constant **dword index** it reads.
- Likely files in your tree: `RexGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp` (your
  D3D12/DXBC path) and the SPIR-V equivalent if you run Vulkan. We confirmed it on the **SPIR-V**
  backend; check whichever backend(s) you ship.
- Sanity-cross-check against Xenia's reference texture-fetch translation (rexglue descends from it) —
  the `exp_adjust` field offset is the thing to confirm.

> Our workaround (prebuilt SDK only, no source): we patched the fetch-constant **data** we feed the
> shader — inject the *real* `exp_adjust` (dword_3 bits 13-18) into the bits the buggy shader reads
> (dword_4 bits 13-18) and clear lod_bias there: `d4 = (d4 & ~(0x3FF<<12)) | (((d3>>13)&0x3F)<<13)`.
> That neutralizes it (real exp_adjust is ~0 → `×1`) at the cost of dropping lod_bias. You can do the
> real translator fix instead.

## 5. Why this is worth checking before more equipment-material patches

Your renderer notes show a long campaign against **green/purple/black equipment corruption** layered
with cube-reflection, mip-rebase, stacked-3D, and swizzle fixes. This `exp_adjust` bug is **more
fundamental** than any of those: it scales the texel **value** to ~0 *regardless* of the cube/mip/
swizzle path. If your equipment atlases carry a `lod_bias` (crisp helmet/glove/pad/number atlases
commonly do), this is silently crushing them to black underneath everything else.

Scope it precisely:
- It explains the **black/dark** component of equipment corruption, and any **missing LOD-biased
  decals** (numbers, nameplates, logos, sharpened detail maps).
- It does **not** explain green/purple **noise** — that's your separate normal/cube/specular path.
  But it may be the dark layer *under* the noise. Fix this first, then re-judge the noise.

## 6. One-line

The rexglue tfetch translator reads texture **`exp_adjust` from dword_4 (lod_bias)** instead of
**dword_3** → any **LOD-biased** texture is multiplied by `2^(garbage) ≈ 0` → **black**. Verify by
comparing `dword_3[13:18]` vs `dword_4[13:18]` on any wrongly-black equipment texture; fix the dword
in the source translator and upstream it.
