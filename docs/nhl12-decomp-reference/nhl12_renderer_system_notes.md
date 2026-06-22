# NHL12 Renderer System Notes

Date: 2026-06-14

This document records project-specific renderer knowledge as we learn it. ReXGlue in this repo should be treated as the NHL12 renderer runtime, not a generic Xbox 360 emulator compatibility layer.

For the short regression checklist, see:

```text
docs/nhl12_renderer_regression_guardrails.md
```

## D3D12 Path Required For Gameplay

NHL12 needs the D3D12 ROV render-target path.

The default D3D12 RTV path can show 2D UI while the gameplay scene is black. For this title, that is not a useful fallback. The app forces:

```text
render_target_path_d3d12=rov
```

in `app/src/nhl12_app.h` before the GPU backend is created.

## Release Codegen Flags

The generated recompiled game code must be compiled differently from normal C++.

The hot recompiled files:

```text
app/generated/default/nhl12_recomp.*.cpp
```

reinterpret guest memory heavily. Release builds must use:

```text
-fno-strict-aliasing
-ffp-contract=off
```

Without those flags, Release can miscompile guest-memory and floating-point behavior. The symptom was blue/green player and equipment corruption while Debug looked correct.

The giant registration files are intentionally different:

```text
app/generated/default/nhl12_init.cpp
app/generated/default/nhl12_register.cpp
```

They are compiled with `-O0` to avoid huge clang memory usage. They are startup tables, not hot gameplay code.

## RexGlue Nightly Codegen Experiment

On 2026-06-14, we tested one targeted RexGlue nightly change without upgrading the whole SDK:

```text
nightly-20260605-f22cd9dc
649a3b9 Revert "feat(codegen): support mask=3 and mask=2 zero-clear for vpkd3d128 float16_4"
```

Why this is relevant to NHL12:

- NHL12 generated code uses `vpkd3d128 ...,5,2,2` in `app/generated/default/nhl12_recomp.21.cpp`
  and `app/generated/default/nhl12_recomp.49.cpp`.
- The reverted local heuristic zero-cleared half of the destination vector before writing packed
  float16 data.
- Equipment corruption looked like shifted or garbage packed material/texture data, so this codegen
  path was plausible enough to test.

Implementation:

- `RexGlue/src/codegen/builders/vector.cpp` now matches the nightly behavior for `float16_4`.
- RexGlue Release was rebuilt and installed.
- NHL12 codegen was rerun so the generated C++ no longer emits the `ctx.v*.u64[0] = 0` clear before
  `vpkd3d128 ...,5,2,2`.
- The Release app was rebuilt.

Status: user testing showed this did not fix the close-up helmet/glove/pad artifacts. Keep the note
because it was a plausible lead and explains why the generated code differs from the earlier local
heuristic, but do not treat it as the renderer solution.

## NHL12 Stacked Texture3D Behavior

NHL12 uses a color lookup path where shader `tfetch3D` instructions need to be treated as stacked 2D data on D3D12.

The project cvar is:

```text
nhl_force_stacked_texture3d=true
```

defined in:

```text
RexGlue/src/graphics/flags.cpp
```

This is NHL-specific. In generic Xenos terms:

- fetch constants store stacked textures as `DataDimension::k2DOrStacked`;
- shader instructions access them through `FetchOpDimension::k3DOrStacked`;
- true 3D textures and stacked 2D textures normally share the same `tfetch3D` instruction family.

For NHL12, the D3D12 renderer should force this path to stacked 2D.

## DXBC Translator Patch

Files:

```text
RexGlue/include/rex/graphics/pipeline/shader/dxbc_translator.h
RexGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp
```

The DXBC shader translator now treats `nhl_force_stacked_texture3d` as a shader modification bit and forces `tfetch3D` translation onto the stacked branch.

Important detail: it is not enough to set the runtime "is 3D" branch condition to false. The translator must avoid emitting the true-3D SRV path when NHL's forced-stacked mode is active. Otherwise translated shaders can still carry 3D SRV bindings/descriptors for a path NHL12 should not use.

Current behavior in forced-stacked mode:

- `tfetch3D` skips the true-3D branch;
- shader texture bindings use the stacked/2D-array path;
- the DXBC modification version was bumped so stale pipeline storage is invalidated.

## Native Internal Draw Resolution

NHL12 must keep ReXGlue's internal draw-resolution scale at 1x.

The launch dialog resolutions are host window sizes only. They should not enable Xenia-style
internal render/texture upscaling.

Why: NHL12's player, goalie, helmet, pad, and glove textures are sensitive to the generic scaled
resolve / scaled texture-offset path. When draw-resolution texture scaling is enabled, equipment
atlases can sample from shifted texels or neighboring atlas regions. The visual symptom is
green/purple/black corruption on equipment while the rink, UI, and base jerseys may still look
mostly correct.

The renderer guard is:

```text
nhl_force_native_draw_resolution=true
```

implemented in:

```text
RexGlue/src/graphics/pipeline/texture/cache.cpp
```

When this flag is active, `TextureCache::GetConfigDrawResolutionScale` returns 1x even if generic
`resolution_scale`, `draw_resolution_scale_x`, or `draw_resolution_scale_y` were supplied by config
or command line.

The app also explicitly sets the NHL12 renderer contract before backend setup:

```text
nhl_force_stacked_texture3d=true
nhl_force_native_draw_resolution=true
nhl_fix_cube_reflection_fetches=true
nhl_zero_cube_reflection_fetches=false
nhl_rebase_mip_min_textures=true
nhl_fix_base_only_packed_bc_textures=true
nhl_preserve_equipment_mips_without_forced_aniso=true
resolution_scale=1
draw_resolution_scale_x=1
draw_resolution_scale_y=1
draw_resolution_scaled_texture_offsets=false
```

in `app/src/nhl12_app.h` before the GPU backend is created. This keeps 1080p, 1440p, and 4K launch
options as presentation/window sizes rather than internal texture upscaling modes, preserves the
known-good cubemap reflection path, and keeps the equipment material mip/sampler guards active even
if RexGlue defaults change later.

## NHL12 Cube Fetches With 2D Equipment Maps

2026-06-15 normal-play logs showed that NHL12 can send glossy equipment/material data through a
`tfetchCube` shader instruction while the fetch constant points at a regular 2D mipped material map.
The clearest bad binding was:

```text
fetch=2 shader_dim=cube resource_dim=2DOrStacked null=true compatible=false
fmt=k_DXT1 size=512x512x1 mip_min=0 mip_max=9 packed=true
```

On D3D12, a cube SRV is only valid for `DataDimension::kCube`, so that binding produced a sampled
null descriptor. This matches the live symptoms: helmets may partly work through the real cubemap
reflection path, while gloves, blocker/trapper surfaces, or goalie pads become green/black/purple or
transparent when the equipment material map is sampled through the wrong descriptor shape.

The DXBC translator now treats `nhl_fix_cube_reflection_fetches` as two cooperating NHL12 behaviors:

- true cubemap resources still use the cube coordinate transform and cube SRV, preserving helmet
  reflections and the prior Winter Classic cubemap fix;
- non-cube resources bound to a cube fetch also get a 2D SRV fallback for the same fetch constant and
  signedness;
- the fallback uses cube SC/TC operands as face-local material UVs by converting the 1..2 range to
  0..1 and forcing array layer 0.

Do not remove this path when optimizing cubemap reflections. It is a title-specific bridge between
NHL12's material shader pattern and D3D12's stricter SRV dimension compatibility.

Crash status: enabling this fallback globally caused an access violation at first 3D scene load
before the old equipment mismatch appeared in the log. The fallback is therefore gated by
`nhl_cube_material_2d_fallback=false` by default. Leave `nhl_fix_cube_reflection_fetches=true`; that
is the stable helmet/reflection path. Enable `nhl_cube_material_2d_fallback` only for controlled A/B
logs until the first-scene crash is understood.

## NHL12 Equipment Color Composition

2026-06-14 finding: NHL12 equipment materials are not simple one-diffuse-texture surfaces. The
renderer needs to preserve the game's compositing paths instead of trying broad texture decode or
reflection-disabling workarounds.

Goalie pads, blockers, trappers, and sticks are DB-colorable equipment. `nhlng-meta.xml` confirms
that `exhibitiongoalieequipment` has many per-zone 8-bit RGB fields, such as:

```text
padszone1color_r/g/b ... padszone9color_r/g/b
blockerzone1color_r/g/b ... blockerzone9color_r/g/b
trapperzone1color_r/g/b ... trapperzone9color_r/g/b
stickzone1color_r/g/b ... stickzone3color_r/g/b
showcustomcolors
showcustomstick
```

The asset layout matches this. `extracted/cache_hdd/rendering/goaliepad` contains:

```text
164 *_dm.rx2 diffuse maps
448 *_tm.rx2 template / tint-mask maps
82  *_nm.rx2 normal maps
82  *_sm.rx2 special / specular maps
2   *_am.rx2 ambient maps
```

So green/purple/black corruption on goalie pads or gloves is likely to involve a template/special
map, swizzle/sign interpretation, alpha handling, or mip-window/addressing issue in the recolor
material path. It should not be fixed by blanket DXT decompression or by disabling reflections.

Latest equipment material map breakdown from user testing:

```text
colormap   -> logos and colors
shine map  -> shadows and light reflection
normal map -> wrinkles and shape
```

Jerseys and general equipment follow the same pattern conceptually, and these assets need their mip
chains. Do not hardcode only the shorthand names, though. Some tools describe the colormap and
normal-map families as DXT1/DXT5-like, while the embedded Xbox fetch constants in current extracted
assets may identify them as `k_8_8_8_8`, `k_DXN`, or `k_DXT4_5`. The RX2 fetch constants are the
renderer's source of truth. The offline verifier added on 2026-06-15 proves these real extracted
goalie/glove equipment formats:

```text
*_dm.rx2 diffuse/base maps      -> k_DXT2_3, 512x512, base-only
*_tm.rx2 template/color maps    -> k_DXT1,   512x512, base-only
*_nm.rx2 normal maps            -> k_DXN,    512x512, 10 packed mips
*_sm.rx2 shine/specular maps    -> k_DXT1,   128x128, 8 packed mips
```

The broader 2026-06-15 sweep over `goaliepad`, `glove`, `blocker`, and `trapper` found:

```text
2886 texture RX2 entries passed
24 non-texture RX2 entries skipped
0 failures
0 D3D12 runtime-upload layout mismatches

600  *_dm diffuse/base maps      -> k_DXT2_3, 1 mip, base-only
1628 *_tm template/color maps    -> k_DXT1,   1 mip, base-only
300  *_nm normal maps            -> k_DXN,    10 packed mips
216  *_sm shine/specular maps    -> k_DXT1,   8 packed mips
84   *_sm shine/specular maps    -> k_DXT1,   9 packed mips
56   *_am alpha/aux maps         -> k_DXT1,   8 packed mips
2    *_am alpha/aux maps         -> k_DXT1,   7 packed mips
```

Additional 2026-06-15 jersey/equipment sampling matches the user-level "normal map is DXT5"
description for body materials, while also showing why filename-only assumptions are risky:

```text
300 jersey texlib/body samples passed:
299 -> k_DXT4_5, 512x512, 10 packed mips
1   -> k_DXT1,   1024x1024, 11 packed mips

300 jersey nameplate *_cm samples passed:
300 -> k_8_8_8_8, 256x256, 9 packed mips

400 helmet/stick/general-equipment sample:
275 textured entries passed, 125 non-texture containers skipped
213 mipped k_DXT1 goaliehelmet/color equipment maps preserved
49  base-only k_DXT2_3 helmet diffuse maps preserved
12  base-only k_DXT1 template maps preserved
1   tiny 4x4 k_DXT1 packed-base map preserved
```

So NHL12's renderer contract is not "all equipment color maps are DXT1" at the file level.
The safer rule is: preserve real mip chains for all NHL12 material maps, including mipped DXT1
shine/color maps, mipped DXN goalie normal maps, mipped DXT4/5 jersey/body normal-like maps, and
mipped 8.8.8.8 nameplate/color-support maps.

Default unattended renderer gate:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --nhl12-regression-suite --out build\nhl12_renderer_regression_suite_full
```

Latest full-suite result: PASS in about 5.5 minutes, 0 failed sections:

```text
source contract:              43 checks, 0 failures
renderer-key self-test:        70 cases, 0 failures
material contract:             3856 files, 0 failures, 0 warnings
material stack previews:       40 DB-colored stacks, 40 passes, 30 exact DB color matches
goalie equipment upload proof: 2910 files, 2886 texture passes, 24 non-texture skips, 0 failures
jersey texlib upload sample:   300 files, 0 failures
jersey colormap upload sample: 300 files, 0 failures
```

The source contract audits the NHL12 app defaults and RexGlue renderer defaults before any asset
proof runs. It guards the ROV path, invalid-fetch tolerance, native 1x draw resolution, cube
reflection preservation, real equipment mip preservation, 2D-only sampler guard, DXN BC5 view path,
explicit-LOD mip rebase wiring, the base-only generated BC packed-state fix, and the protected
material formats used by the colormap/logo, shine/reflection, and normal/shape maps. If this fails,
treat it as a renderer regression even if the extracted texture bytes still decode cleanly.

The material stack previews render simplified pad/glove/blocker/trapper composites at multiple LODs
using decoded diffuse, template, normal, and shine layers. They are not a replacement for NHL12's
full shader, but they catch offline neon-green, purple, and near-black layer-alignment artifacts.
The artifact check accounts for DB colors, so saturated green only fails when it is not part of the
selected equipment palette. Preview PNGs are written under
`build\nhl12_renderer_regression_suite_full\material_stack_composites`.

Fast iteration gate:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --nhl12-regression-suite --nhl12-regression-suite-fast --out build\nhl12_renderer_regression_suite_fast
```

Latest fast-suite result: PASS in about 2.0 minutes. It keeps the 43-check source contract, the
70-case renderer-key self-test, the full 3856-file material contract scan, renders 8 representative
material-stack previews, and samples the goalie-equipment byte-upload proof instead of running all
2910 goalie equipment containers.

The broad material-contract gate inside the suite is:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --scan-material-contract-dir goaliepad --scan-material-contract-dir glove --scan-material-contract-dir blocker --scan-material-contract-dir trapper --scan-material-contract-dir jersey --out build\texture_material_contract_equipment_jersey
```

Latest result: 3856 material-map files checked in about 32 seconds, 0 failures, 0 warnings:

```text
970  colormap/support maps -> k_8_8_8_8, real mips preserved
300  normal maps           -> k_DXN,      real mips preserved
300  shine maps            -> k_DXT1,     real mips preserved
58   alpha/support maps     -> k_DXT1,     real mips preserved
600  diffuse/recolor masks  -> k_DXT2_3,   base-only by design
1628 template recolor masks -> k_DXT1,     base-only by design
```

This gate is a fast fetch/key/view/mip contract scan. Use the full regression suite when you need
the contract scan plus byte-for-byte D3D12 upload equivalence coverage before launching the game.

The verifier now also mirrors the NHL12 renderer-key normalization rules. The latest unattended
synthetic edge-case gate was:

```powershell
python tools\nhl12_texture_proof.py --self-test-renderer-key --out build\texture_proof_equipment_renderer_key
```

It must pass the stale non-tiny base-only DXT case, the tiny packed-base DXT case, a mipped DXT1
shine case, a mipped DXN normal case, a mipped DXT5 normal case, a mipped 8.8.8.8 support-map
case, plus 64 historical normal-play generated texture fetches. The historical cases came from the
old normal-play diagnostic where base-only BC runtime textures kept stale packed state. They cover
2 `k_DXT1`, 5 `k_DXT2_3`, and 57 `k_DXT4_5` generated fetches from 32x32 through 2048x1024. The
latest run passed all 70 cases. The latest unattended corpus gate was:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --glob "goaliepad\*.rx2" --glob "glove\*.rx2" --glob "blocker\*.rx2" --glob "trapper\*.rx2" --limit 5000 --out build\texture_proof_goalie_equipment_renderer_key --no-images
```

Result: 2910 RX2 containers scanned, 2886 textured entries passed, 24 non-texture containers
skipped, 0 failures, 0 renderer-key failures, and 0 renderer-view failures. The renderer-key proof
protected 658 real mipped equipment material maps: 300 `k_DXN` normal maps and 358 mipped `k_DXT1`
shine/alpha maps.

If pads, gloves, or jerseys show shifted green/purple/black blocks, treat it first as a mipped
equipment material binding/sampler problem. Do not assume the RX2 bytes are corrupt without running
the proof tool.

The 2026-06-15 verifier also simulates RexGlue's D3D12 upload path: it builds the padded upload
footprint for each stored level, slices packed tails with the same source-box math used by
`D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl`, and byte-compares the result against the
direct decoded mip. It now handles the packed-level-0 special case used by tiny textures, where the
base and mip-tail loops both refer to logical level 0 but separate source ranges. The full
goalie/glove corpus passed this runtime-upload comparison. That means the static extracted
pad/glove bytes and normal D3D12 packed-tail copy math agree; remaining normal play corruption
should be hunted in runtime-generated equipment maps, fetch binding state, sampler state, or shader
material logic.

The verifier also mirrors D3D12 renderer-view decisions: host resource format, UNORM/SNORM SRV
formats, decompression path, host-format swizzle, final host swizzle, and post-swizzle signs. The
latest goalie-equipment corpus passed with 0 renderer-view failures:

```text
1986 k_DXT1   -> BC1_UNORM resource/SRV, RGBA host swizzle
600  k_DXT2_3 -> BC2_UNORM resource/SRV, RGBA host swizzle
300  k_DXN    -> BC5_TYPELESS resource, BC5_UNORM + BC5_SNORM SRVs, RGGG host swizzle
```

DXN normal maps do not require a separate signed resource. They use UNORM/SNORM SRV views over one
typeless BC5 resource. If live `[NHL-TEX]` diagnostics ever show a separate signed resource for
these aligned DXN maps, that is a renderer divergence to investigate.

Important NHL12-specific packed-state rule: do not strip `packed_mips` from a base-only BC fetch if
level 0 itself is packed. Very small Xenos textures can store the base level in a packed tail, and
NHL12's colorized equipment special maps are exactly the kind of data that may be small. The current
`nhl_fix_base_only_packed_bc_textures` guard only ignores stale packed state for base-only BC maps
whose level 0 is not packed.

Skater helmets are also composed materials. User-provided DB research says the helmet shell is
driven mostly by RGB values from the DB, while the logo is the changeable texture. The extracted data
supports that model:

```text
extracted/cache_hdd/rendering/helmet       -> *_dm.rx2 and *_tm.rx2 helmet material assets
extracted/cache_hdd/rendering/helmet_common -> shared texlib_*.rx2 helmet assets
extracted/cache_hdd/rendering/logo         -> many texlib_* logo textures
extracted/cache_hdd/db/nhlng-meta.xml       -> stockteamcolorstable color_r/color_g/color_b
```

The compiled helmet shaders expose the same split:

```text
helmet*.fxo:
  diffuseSampler
  diffuseLogoSampler / diffuseLogoSampler2 / diffuseLogoSampler3
  diffuseFontSampler
  aoSampler
  normalSampler
  specSampler
  envSampler

helmet*_cz.fxo:
  recolor
  recolor_detail
  recolor_template1
```

This means the corrupted white/black helmet screenshots should not be read as "the helmet diffuse
texture is bad" by default. The artifact can be a wrong logo/template/recolor fetch landing on top of
an otherwise DB-colored helmet shell. The next renderer work should log and compare the fetches used
by `helmet*.fxo`, `helmet*_cz.fxo`, `equipment*_cz.fxo`, and `alphaequipment*.fxo`, especially the
`*_tm`, `*_sm`, logo, and recolor-template bindings.

## Failed D3D12 Block Texture Decode Experiment

Files tested and reverted:

```text
RexGlue/include/rex/graphics/flags.h
RexGlue/src/graphics/flags.cpp
RexGlue/src/graphics/d3d12/texture_cache.cpp
app/src/nhl12_app.h
```

NHL12 equipment textures use many Xbox DXT/BC-style formats. When the D3D12 texture cache sees
aligned sizes such as 512x512, the generic path uploads those textures as native host BC resources.
For NHL12 this can show as green, purple, or black block corruption on helmets, gloves, pads, and
goalie equipment while jerseys and rink art still look mostly correct.

Experiment:

```text
nhl_force_block_texture_decompression=true
```

Result: this did not fix the equipment corruption and introduced material regressions, including
transparent goalie gloves. Do not restore this as an active guard. The issue is deeper than simply
choosing D3D12 decompressed host formats for all aligned DXT/BC textures.

The useful lesson is negative: NHL12's corrupt equipment path is likely tied to fetch descriptors,
shader binding interpretation, cubemap/specular inputs, or texture swizzle/sign metadata, not just
native BC resource upload.

## D3D12 Texture Cache Patch

Files:

```text
RexGlue/include/rex/graphics/d3d12/texture_cache.h
RexGlue/src/graphics/d3d12/texture_cache.cpp
```

The D3D12 texture cache now also knows about NHL12's forced-stacked `tfetch3D` rule.

When `nhl_force_stacked_texture3d` is active:

- a `FetchOpDimension::k3DOrStacked` shader binding is treated as a 2D binding for compatibility checks;
- descriptor creation requests `DataDimension::k2DOrStacked`;
- null fallback uses the 2D-array null SRV, not the 3D null SRV.

This matters because shader translation and D3D12 descriptor binding must agree. If the shader samples a 2D-array path but the texture cache rejects the binding as "not true 3D" or falls back to a 3D null descriptor, player textures can become flat wrong colors instead of crashing.

## Metallic Reflection / Cube Fetch Patch

Files:

```text
RexGlue/include/rex/graphics/flags.h
RexGlue/src/graphics/flags.cpp
RexGlue/include/rex/graphics/pipeline/shader/dxbc_translator.h
RexGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp
```

The blue equipment bug appears strongest on glossy/metallic regions such as helmets, gloves, pads, and goalie equipment while base jersey textures remain mostly intact. That points to the reflection/specular path rather than total base texture corruption.

NHL12 uses cube texture fetches for environment-style reflection. The current NHL-tailored workaround is:

```text
nhl_fix_cube_reflection_fetches=true
```

The diagnostic flag:

```text
nhl_zero_cube_reflection_fetches=false
```

proved that `tfetchCube` was the source of the saturated blue contribution, but it is too blunt for gameplay. Zeroing cube fetches removes helmet reflections and can make reflective meshes, including goal parts, transparent because NHL12 appears to use the cube sample alpha as part of the material equation.

The active fix keeps `tfetchCube` samples enabled, then applies a small NHL-specific correction in DXBC translation:

- reflection intensity stays live;
- if any cubemap RGB channel is requested, the translator samples all RGB channels so single-channel
  cubemap fetches cannot bypass the sanitizer;
- cube RGB is clamped to `[0, 1]`, collapsed to a neutral reflection value using the minimum RGB
  channel, then capped to `0.25`, suppressing both the Winter Classic blue flood and normal-play
  green/purple block noise on close-up helmets, gloves, pads, and goalie gear;
- cube fetch alpha is forced to `1.0` so reflective materials do not become transparent.

This is intentionally title-specific. It preserves helmet/ice/equipment environmental shine while
preventing broken cubemap/specular data from flooding equipment with electric blue or multicolor
speckle.

Regression note: do not make `nhl_zero_cube_reflection_fetches` the default fix again. It was useful as a proof that the cube path caused the tint, but it regressed helmets by removing reflection and regressed goals by making reflective geometry transparent.

The DXBC modification version was bumped to `0x20260619` after this change so old shader pipeline
storage cannot reuse shaders that still sample the cube reflection path with the older channel
behavior.

## Equipment Texture Mip-Window Rebase

Files:

```text
RexGlue/include/rex/graphics/pipeline/texture/cache.h
RexGlue/src/graphics/pipeline/texture/cache.cpp
RexGlue/src/graphics/d3d12/texture_cache.cpp
RexGlue/src/graphics/flags.cpp
app/src/nhl12_app.h
```

2026-06-14 finding: close-up normal gameplay screenshots showed helmets, gloves, pads, and goalie
gear with shifted black/green/purple texture patches. This looked like a warped atlas or wrong mip
window, not like the earlier cubemap blue-tint bug.

RexGlue's generic texture key previously normalized texture fetch constants with `mip_min_level`
mostly living in sampler state. For NHL12 equipment materials that can be unsafe because the game can
bind a mip-only view through `mip_address` while shaders still sample as if the first visible mip is
LOD 0. The result is sampling data from the wrong logical mip or from an uninitialized/full-chain
slot, which appears as shifted rectangular helmet/glove/pad texture chunks.

The NHL12-specific fix is:

```text
nhl_rebase_mip_min_textures=true
```

When a non-cubemap fetch has `mip_min_level > 0`, `TextureCache::BindingInfoFromFetchConstant` now
tries to rebase the `TextureKey` so the requested first mip becomes logical mip 0:

- the base page is moved from the original `mip_page` plus the page-aligned offset of
  `mip_min_level`;
- width, height, 3D depth, pitch, and mip count are reduced to the rebased mip window;
- D3D12 sampler creation reads the normalized/rebased key so `MinLOD` is `0` for the rebased view;
- cubemaps are excluded so the working helmet/reflection cubemap fix is not touched;
- packed mip-tail cases that are not page-clean fail closed and keep the old generic path.

2026-06-14 follow-up: the texture-cache rebase alone is not enough for shaders that use explicit
LOD (`sample_l` / guest explicit mip fetches). Those shaders can still ask for guest LOD 1 even
though the host resource has already made guest mip 1 become host mip 0. RexGlue now records the
actual successful rebase offset per active fetch constant:

```text
TextureBinding::rebased_mip_min_level
SystemConstants::texture_rebased_mip_min_levels[8]
```

The D3D12 command processor packs one byte per fetch constant into the system constants. The DXBC
translator version is bumped to `0x20260620`, and the shader modification key includes
`normalize_rebased_texture_lod` so cached shaders cannot be reused across this behavior change.

For explicit LOD texture fetches only, the translator subtracts that recorded offset from the shader
LOD and clamps the result to 0. It does this only when the texture cache says the rebase succeeded;
failed packed/unaligned mip-tail cases carry offset 0 and therefore keep the generic path. Computed
or implicit LOD fetches are left alone because their gradients already operate against the rebased
host dimensions.

Cubemaps are excluded twice:

- `TextureCache::TryRebaseNHL12MipMinTextureKey` refuses cube textures.
- `dxbc_translator_fetch.cpp` refuses to apply explicit LOD normalization to cube fetches.

This is deliberate. Helmet/environment reflections are the cubemap path and must stay alive.

This fix is intentionally title-specific. Do not replace the cubemap sanitizer with this, and do not
disable cubemap reflections to hide equipment artifacts. These are separate bugs:

- cubemap bug: saturated blue/green reflective contribution;
- mip-window bug: shifted atlas/warped equipment texture patches.

## Equipment Mip Proof And Sampler Guard

Files:

```text
tools/nhl12_texture_proof.py
RexGlue/include/rex/graphics/flags.h
RexGlue/src/graphics/flags.cpp
RexGlue/src/graphics/pipeline/texture/cache.cpp
RexGlue/src/graphics/d3d12/texture_cache.cpp
```

NHL12 equipment should be understood as a multi-map material stack, not one diffuse texture:

```text
diffuse/base      -> embedded fetch format, often k_DXT2_3
template/color    -> embedded fetch format, often k_DXT1
shine/specular    -> embedded fetch format, often mipped k_DXT1
normal            -> embedded fetch format, often mipped k_DXN
```

The important behavior is mip correctness. Real mipped equipment textures must keep their mip chains;
otherwise close-up and replay material sampling can read the wrong logical level. The offline proof
command for pads/gloves is:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --glob "goaliepad\*.rx2" --glob "glove\*.rx2" --limit 500 --out build\texture_proof_equipment --no-images
```

Full unattended renderer-key gate:

```powershell
python tools\nhl12_texture_proof.py --self-test-renderer-key --out build\texture_proof_goalie_equipment_renderer_key
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --glob "goaliepad\*.rx2" --glob "glove\*.rx2" --glob "blocker\*.rx2" --glob "trapper\*.rx2" --limit 5000 --out build\texture_proof_goalie_equipment_renderer_key --no-images
```

Focused all-mip preview command:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --glob "goaliepad\goaliepad_0_11_nm.rx2" --glob "goaliepad\goaliepad_0_11_sm.rx2" --out build\texture_proof_mips --all-mip-pngs
```

Current proof results:

```text
1346 texture RX2 entries passed
12 non-texture RX2 entries skipped
0 failures
```

Material-stack composite proof command:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --material-set goaliepad:0_11 --material-set glove:0_15 --out build\texture_material_artifact_metrics --material-size 512 --no-images
```

This writes:

```text
build\texture_material_artifact_metrics\material_goaliepad_0_11_lod0.png
build\texture_material_artifact_metrics\material_goaliepad_0_11_lod1.png
build\texture_material_artifact_metrics\material_goaliepad_0_11_lod2.png
build\texture_material_artifact_metrics\material_goaliepad_0_11_lod9.png
build\texture_material_artifact_metrics\material_glove_0_15_lod0.png
build\texture_material_artifact_metrics\material_glove_0_15_lod1.png
build\texture_material_artifact_metrics\material_glove_0_15_lod2.png
build\texture_material_artifact_metrics\material_glove_0_15_lod9.png
```

Those composites are an offline alignment proof for the equipment material stack. They are not a
replacement for NHL12's shader, but they prove that the extracted diffuse/template/normal/shine
layers can be decoded, mipped, and sampled together coherently before opening the game.
The proof also records artifact ratios for neon green, purple, transparency, and near-black coverage
in `material_report.json`. The latest representative run passed both `goaliepad:0_11` and
`glove:0_15` with `green=0.0000`, `purple=0.0000`, and `black=0.0000` across LOD 0, 1, 2, and the
lowest mip. High transparency in these UV-sheet previews is expected and is reported as a note, not a
failure.

Full discovered material-stack gate:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --discover-material-dir goaliepad --discover-material-dir glove --out build\texture_material_discovery_full --material-size 64 --no-images
```

Latest result: 264 complete material stacks discovered, 164 in `goaliepad` and 100 in `glove`.
All 264 passed, producing 1056 composite artifact metrics. Max neon-green ratio was 0, max purple
ratio was 0, and max near-black ratio was 0.1963. Transparency reached 0.9897 in UV sheets and is
expected to be high for cutout/material space.

DB-colored material proof is also available. It reads `exhibitiongoalieequipment` from
`extracted\cache_hdd\db\nhlng.db` using field layout from `nhlng-meta.xml` and records the bit order,
equipment kind, equipment id, matched DB rows, and tint colors in the material report:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --material-set goaliepad:0_11 --material-set glove:0_15 --out build\texture_material_db_colors --material-size 256 --no-images --db-file extracted\cache_hdd\db\nhlng.db --db-meta extracted\cache_hdd\db\nhlng-meta.xml --db-bit-order msb
```

Latest representative result: `goaliepad:0_11` matched one DB `pads` row, `glove:0_15` matched
three DB `trapper` rows, and both passed artifact metrics. A broader 40-stack discovered sample with
DB colors also passed with 27 exact DB equipment-color matches and 13 DB global color fallbacks. See
`docs\nhl12_equipment_db_color_notes.md`.

The broader goalie-equipment DB sample now uses interleaved discovery across `goaliepad`, `glove`,
`blocker`, and `trapper` so a limited sample cannot accidentally cover only the first directory:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --discover-material-dir goaliepad --discover-material-dir glove --discover-material-dir blocker --discover-material-dir trapper --discover-material-limit 40 --out build\texture_material_goalie_equipment_db_sample --material-size 64 --no-images --db-file extracted\cache_hdd\db\nhlng.db --db-meta extracted\cache_hdd\db\nhlng-meta.xml --db-bit-order msb
```

Latest result: 40 material stacks passed with 0 failures. Exact DB color matches were found for 6
pad stacks, 15 trapper/glove stacks, and 9 blocker stacks; the remaining 10 used DB global fallback
colors. This reinforces that static RX2 data, packed-tail upload layout, and DB-colored material
layer alignment are coherent offline for the currently corrupted goalie equipment families.

The renderer has NHL-specific guards for this:

```text
nhl_rebase_mip_min_textures=true
nhl_fix_base_only_packed_bc_textures=true
nhl_preserve_equipment_mips_without_forced_aniso=true
```

`nhl_rebase_mip_min_textures` handles non-cubemap fetches that start at `mip_min_level > 0`.
`nhl_fix_base_only_packed_bc_textures` handles base-only 2D BC equipment fetches such as DXT1,
DXT2/3, and DXT4/5 when they carry a stale packed-mips bit. Those base-only maps should not be
uploaded through packed-tail addressing.
`nhl_preserve_equipment_mips_without_forced_aniso` keeps real mips enabled but prevents RexGlue's
host-side anisotropic override from being forced onto mipped NHL12 material maps. This now covers
the equipment/jersey stack the game actually uses: DXT1 color/shine maps, DXT4/5 and DXN normal
maps, and the mipped 8.8.8.8 support/nameplate maps observed in jersey assets. Cubemaps are
intentionally excluded so the working helmet/reflection path is not touched.

The earlier signed DXN/DXT5A theory is a failed or unproven lead for the current pad/glove issue.
Do not make it the primary explanation unless a future diagnostic log shows NHL12 binding signed
normal/spec maps in the broken draw.

Runtime proof path for normal gameplay:

```powershell
.\app\out\build\win-amd64-release\nhl12.exe --game_data_root extracted --log_file build\nhl12_normalplay_equipment_sampler.log --log_level info --mnk_mode=false --nhl_log_texture_bindings=true
python tools\nhl12_texture_proof.py --analyze-log build\nhl12_normalplay_equipment_sampler.log --catalog-report build\texture_proof_goalie_equipment_renderer_key\report.json --out build\nhl12_normalplay_equipment_sampler_analysis
```

The first command writes `[NHL-TEX]` and `[NHL-SAMPLER]` lines while a human navigates to the
broken normal-play equipment case. The analyzer looks for null texture descriptors, resource/shader
dimension mismatches, mipped NHL12 material fetches that lost packed mip state, base-only BC fetches
that kept stale packed state when level 0 is not packed, non-cube mip-window fetches that were not
rebased, and mipped material samplers that still had RexGlue's forced anisotropic override. This is
the preferred way to turn a live pad/glove screenshot into a renderer-side proof instead of another
guess.

When `--catalog-report` is supplied, the analyzer also compares runtime `[NHL-TEX]` bindings against
the extracted RX2 proof catalog. A strict match means the runtime binding agrees with a known asset
on format, dimensions, mip count, packed state, tiled state, and pitch. A loose match means the core
format/size/mip identity agrees but one renderer-key detail differs and needs inspection. Runtime
DB-generated or recolored textures can remain unmatched without automatically being corrupt.

The analyzer also reports raw fetch-shape matches. These decode the logged `fetch_dw` constants as
hex dwords and compare the non-address metadata that should survive runtime relocation: raw/base
format, endianness, pitch, tiled/stacked state, dimensions, dimension type, packed-mips bit,
fetch-level mip window, swizzle, and signs. A `strict+shape` match is the strongest unattended proof
currently available that the live binding resembles an extracted equipment map before shader
material constants are considered.

2026-06-15 analyzer refinement: the translated shader declares unsigned and signed SRV slots, but
samples only the slot required by runtime TextureSign values. The log analyzer now treats null
unused signed/unsigned slots as notes instead of material failures, and it re-decodes `fetch_dw`
values through the current NHL12 renderer-key rules. Old logs from before the packed-state fix now
collapse to the real signal: base-only BC runtime bindings whose logged packed state differs from
current normalization.

Positive smoke for the catalog bridge:

```powershell
python tools\nhl12_texture_proof.py --analyze-log build\nhl12_catalog_smoke.log --catalog-report build\texture_proof_equipment_renderer_key\report.json --out build\texture_log_analysis_catalog_smoke
```

Latest result: a synthetic runtime binding for `goaliepad_0_11_sm.rx2` matched the extracted mipped
`k_DXT1` shine map as `strict+shape`, preserved 8 mips, and reported no risky sampler state. The
shape match fans out to many equivalent equipment assets because multiple pads/gloves/blockers can
share identical fetch metadata; path identity still needs runtime addresses or asset-load logging.

## Shader Storage

Shader storage lives under:

```text
C:\Users\thrif\Documents\nhl12\cache_rov_windowed\shaders
```

The app currently clears that shader storage at launch. This is intentional while the renderer is being stabilized. Old shader translations can preserve bad assumptions even after source fixes.

The D3D12 persistent pipeline files are separated by ROV/RTV:

```text
45410964.rov.d3d12.xpso
45410964.rtv.d3d12.xpso
```

Guest shader storage is:

```text
45410964.xsh
```

## Video Green Artifact / MPEG Texture Formats

Files:

```text
RexGlue/src/graphics/d3d12/texture_cache.cpp
```

Detailed fix plan:

```text
docs/nhl12_vp6_green_video_fix_plan.md
```

NHL12 frontend movies are VP6 assets. The exact EA logo asset checked on 2026-06-14 is `vp60`,
1280x720, likely 130 frames at 30 FPS. ReXGlue should not treat the source movie as DXT.

After the game decodes VP6, the renderer may see decoded movie planes through Xenos video/MPEG
texture formats:

```text
k_16_MPEG
k_16_16_MPEG
k_16_MPEG_INTERLACED
k_16_16_MPEG_INTERLACED
```

Current warning: as of this note, the D3D12 `host_formats_` table still maps these MPEG entries to
`DXGI_FORMAT_UNKNOWN`. If a movie shader samples those planes as null or unsupported SRVs, YUV
conversion can produce green video artifacts because luma/chroma data is missing or neutral chroma
is not available.

The intended D3D12 NHL12 mapping should treat them like real normalized 16-bit video planes:

- `k_16_MPEG` and `k_16_MPEG_INTERLACED` use `R16_TYPELESS` resources with `R16_UNORM` views;
- `k_16_16_MPEG` and `k_16_16_MPEG_INTERLACED` use `R16G16_TYPELESS` resources with `R16G16_UNORM` views;
- regular 16bpb / 32bpb texture loading shaders are used.

This is intentionally conservative: it enables existing movie plane data to reach the game's own
video shader instead of adding a separate host decoder path. It must be implemented without touching
the cubemap reflection workaround: `nhl_fix_cube_reflection_fetches=true` and
`nhl_zero_cube_reflection_fetches=false` remain required.

## Current Renderer Backups

Renderer backups made during this investigation:

```text
backups/renderer_20260614_150339
backups/renderer_nhl12_pass_20260614_151204
backups/renderer_cube_reflection_20260614_151726
backups/renderer_cube_fix_20260614_152259
```

Use these as file snapshots if a renderer patch causes black gameplay or crashes.

## Testing Notes

Frontend `[SWAP-FPS]` logs prove the app is presenting, but they do not prove gameplay materials are correct.

Useful proof log from the current NHL-specific D3D12 texture-cache patch:

```text
build/nhl12_nhl_forced_stacked_texturecache.log
```

That smoke run launched without a crash file and presented around 60 FPS in the frontend. Frontend presentation is useful for crash/performance checks, but material correctness still requires live gameplay verification.

Gameplay confirmation after the cube reflection fix:

```text
2026-06-14: user confirmed the fix works after restoring cube reflections with nhl_fix_cube_reflection_fetches=true and leaving nhl_zero_cube_reflection_fetches=false.
```

Do not count a future smoke test as equivalent to this visual confirmation. The artifact is material-specific and must be checked on helmets, gloves, goalie gear, and goals in live gameplay.
