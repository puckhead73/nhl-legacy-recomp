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

The app also explicitly sets:

```text
resolution_scale=1
draw_resolution_scale_x=1
draw_resolution_scale_y=1
draw_resolution_scaled_texture_offsets=false
```

in `app/src/nhl12_app.h` before the GPU backend is created. This keeps 1080p, 1440p, and 4K launch
options as presentation/window sizes rather than internal texture upscaling modes.

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

This fix is intentionally title-specific. Do not replace the cubemap sanitizer with this, and do not
disable cubemap reflections to hide equipment artifacts. These are separate bugs:

- cubemap bug: saturated blue/green reflective contribution;
- mip-window bug: shifted atlas/warped equipment texture patches.

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
