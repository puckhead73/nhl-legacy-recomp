# NHL12 VP6 Green Video Fix Plan

Date: 2026-06-14

Status: investigation and patch plan only. This document does not mean the fix has already been
implemented.

## Purpose

NHL12 intro and frontend movies can show large green corruption during animated frames, especially
in the EA logo movie. The confirmed source asset is VP6, so the fix must target the decoded movie
presentation path in ReXGlue, not a DXT movie decoder and not the cubemap reflection shader path.

The other non-negotiable goal is to preserve the working cubemap reflection fix. Helmets, goalie
gear, ice, and goals must keep their reflections and opacity after the video patch.

## Confirmed Facts

The exact bad movie supplied by the user is:

```text
C:\Users\thrif\Downloads\ealogo.vp6
```

It is byte-identical to the game asset:

```text
extracted\cache_hdd\fe\movies\ntsc\ealogo.vp6
```

Header facts from the file:

```text
Magic: MVhd
Codec: vp60
Width: 1280
Height: 720
Likely frame count: 130
Likely frame rate: 30000 / 1000 = 30 fps
```

Therefore:

- the source movie is VP6;
- the green corruption is not because the `.vp6` file is DXT;
- the green corruption is probably in decoded-frame upload, Xenos texture format mapping,
  YUV/packed-video sampling, or movie shader presentation.

## Important Renderer Separation

There are two separate bugs that can both look like color corruption:

1. Gameplay material/cubemap corruption.
2. Frontend/movie green corruption.

The working gameplay cubemap fix lives in the shader translator:

```text
RexGlue\src\graphics\pipeline\shader\dxbc_translator_fetch.cpp
RexGlue\src\graphics\flags.cpp
RexGlue\include\rex\graphics\flags.h
RexGlue\include\rex\graphics\pipeline\shader\dxbc_translator.h
```

The video fix should live primarily in the D3D12 texture/movie presentation path:

```text
RexGlue\src\graphics\d3d12\texture_cache.cpp
RexGlue\src\graphics\pipeline\texture\info_formats.cpp
RexGlue\include\rex\graphics\xenos.h
```

Do not use a cubemap workaround to fix video. Do not use a video workaround to fix cubemaps.

## Current Code State To Re-check Before Patching

As of this document, the D3D12 host-format table still maps these Xenos movie/MPEG formats to
`DXGI_FORMAT_UNKNOWN`:

```text
k_16_MPEG
k_16_16_MPEG
k_16_MPEG_INTERLACED
k_16_16_MPEG_INTERLACED
```

`FormatInfo` already describes them as uncompressed decoded planes:

```text
k_16_MPEG               16 bpp
k_16_16_MPEG            32 bpp
k_16_MPEG_INTERLACED    16 bpp
k_16_16_MPEG_INTERLACED 32 bpp
```

D3D12 also has packed 4:2:2 video-like formats already represented:

```text
k_Cr_Y1_Cb_Y0_REP
k_Y1_Cr_Y0_Cb_REP
```

Those packed formats are different from the VP6 source codec. They are possible decoded-frame or
presentation formats used after the game has decoded VP6.

Any existing doc claiming MPEG formats are already fully mapped should be treated as stale unless
the code has been changed and rebuilt.

## Most Likely Fix

The likely D3D12 patch is to make the decoded MPEG/video plane formats real sampleable textures
instead of null/unknown textures.

Expected first-pass mapping:

```text
k_16_MPEG
k_16_MPEG_INTERLACED
  resource format: DXGI_FORMAT_R16_TYPELESS
  unsigned SRV:    DXGI_FORMAT_R16_UNORM
  load shader:     kLoadShaderIndex16bpb
  swizzle:         RRRR

k_16_16_MPEG
k_16_16_MPEG_INTERLACED
  resource format: DXGI_FORMAT_R16G16_TYPELESS
  unsigned SRV:    DXGI_FORMAT_R16G16_UNORM
  load shader:     kLoadShaderIndex32bpb
  swizzle:         RGGG
```

This is deliberately conservative. It lets the game's own movie shader sample decoded luma/chroma
planes instead of receiving null SRVs, zeros, or garbage. A green movie usually means the shader got
valid luma but broken/missing chroma, or it sampled the wrong channel order.

If the interlaced variants still corrupt after this mapping, investigate field layout and pitch
before inventing a CPU-side video decoder.

## Investigation Before Any Behavior Patch

Do this around the EA logo animation, not the first static frame and not the main menu:

1. Log movie texture fetches only while `ealogo.vp6` is being displayed.
2. Capture texture format, base address, width, height, pitch, endianness, tiled/linear bit, swizzle,
   signedness, and shader fetch dimension.
3. Log whether each movie texture gets a real SRV or a null SRV.
4. Log whether the movie shader samples `k_16_MPEG`, `k_16_16_MPEG`, packed 4:2:2 formats, or plain
   RGBA.
5. If the format table says the texture is supported but the video is still green, dump one decoded
   frame plane and inspect whether the chroma values are plausible.

The goal is to patch the exact decoded texture path NHL12 uses. Do not assume DXT just because a
runtime texture log has a DXT-looking descriptor.

## What Not To Do

- Do not add or keep a CPU DXT movie decoder for `.vp6` files.
- Do not zero cubemap samples.
- Do not set `nhl_zero_cube_reflection_fetches=true` for gameplay.
- Do not disable `nhl_fix_cube_reflection_fetches`.
- Do not use `native_2x_msaa=false` as a persistent video or performance fix.
- Do not trust a clean first frame or final frame as proof that the animation is fixed.
- Do not trust main-menu screenshots as proof that the EA logo animation is fixed.
- Do not call the video source DXT in future docs.

## Cubemap Reflection Guardrails

These values must remain the normal gameplay defaults:

```text
nhl_fix_cube_reflection_fetches=true
nhl_zero_cube_reflection_fetches=false
nhl_force_stacked_texture3d=true
native_2x_msaa=true
render_target_path_d3d12=rov
```

The active cubemap behavior is:

- keep `tfetchCube` live;
- clamp the broken saturated blue reflection contribution;
- force cube fetch alpha to `1.0`;
- preserve helmet reflections;
- preserve goal opacity.

If any future optimization changes shader translation, texture binding, MSAA behavior, or shader
cache keys, gameplay must be rechecked against these visual requirements:

- helmets are reflective, not matte;
- helmets/gloves are not electric blue;
- goalie gear does not show green artifacts;
- goals and reflective rink objects are not transparent;
- gameplay is not black.

If shader translator output changes, bump:

```text
DxbcShaderTranslator::Modification::kVersion
```

If only the D3D12 host-format table changes and shader code does not change, a shader-version bump
may not be required, but the shader cache should still be cleared for verification.

## Verification Plan

After the video patch is implemented and rebuilt:

1. Launch 1080p windowed first.
2. Watch the EA logo animation until the movement/corruption period, not just the opening frame.
3. Capture a screenshot during the animation on the white background.
4. Confirm the logo video has no green wash, checkerboard, purple/green channel split, or lag spikes.
5. Enter live gameplay.
6. Confirm cubemap reflections still match the known-good renderer:
   - helmet reflections visible;
   - goals opaque;
   - no matte blue equipment;
   - no black gameplay scene.
7. Repeat at the higher fullscreen/resolution mode that previously triggered regressions.

Frontend `[SWAP-FPS]` is not enough. The proof must include the animated EA logo and live gameplay
materials.

## Backup Rule

The protected renderer backup is:

```text
backups\WORKING_RENDERER_NEVER_DELETE
```

Do not delete it. Do not overwrite it. If a video patch breaks gameplay materials or blackens the
3D scene, compare against that backup before making more changes.

