# NHL12 Renderer Regression Guardrails

Date: 2026-06-14

This file is the short "do not break this again" checklist for NHL12 renderer work. ReXGlue in this repository is the NHL12 runtime, so NHL12-specific fixes are allowed and expected.

## Known-Good Renderer Requirements

- D3D12 must use the ROV render target path. RTV can show frontend/UI while gameplay is black.
- `native_2x_msaa` must remain enabled unless a test explicitly says otherwise. Disabling it did not fix performance and was part of the route that exposed old material bugs again.
- Generated hot game code must compile with `-fno-strict-aliasing -ffp-contract=off`.
- `app/generated/default/nhl12_recomp.20.cpp` must additionally compile with
  `-O0 -fno-strict-aliasing -ffp-contract=off`; this TU contains the vector-heavy
  runtime equipment compose/mip loops suspected of producing the 512x512 generated
  pad/glove/helmet material textures.
- NHL12 internal draw resolution must stay 1x. Larger launch-menu resolutions resize the host
  window/presenter only; do not enable Xenia-style draw-resolution texture scaling.
- `nhl_force_stacked_texture3d` must default to `true`.
- `nhl_force_native_draw_resolution` must default to `true`.
- `nhl_fix_cube_reflection_fetches` must default to `true`.
- `nhl_cube_material_2d_fallback` must default to `false` until the first-3D-scene crash is fixed.
  The experimental runtime cube-vs-2D fallback is a useful lead, but enabling it globally caused
  `0xc0000005` at first 3D scene load.
- `nhl_zero_cube_reflection_fetches` must default to `false`. This is diagnostic only.
- `nhl_rebase_mip_min_textures`, `nhl_fix_base_only_packed_bc_textures`, and
  `nhl_preserve_equipment_mips_without_forced_aniso` must stay enabled.
- `app/src/nhl12_app.h` must explicitly force the NHL12 renderer flags above before GPU backend
  setup; do not rely only on generic RexGlue defaults.
- If shader translation behavior changes, bump `DxbcShaderTranslator::Modification::kVersion`.
- VP6 movie fixes must stay in the video/texture presentation path, not the cubemap reflection path.
- D3D12 MPEG texture formats must be mapped to real R16/R16G16 resources so videos do not sample null green YUV planes. See `docs/nhl12_vp6_green_video_fix_plan.md`.
- Treat NHL12 equipment as composed DB-color/material data, not as simple diffuse textures.
  Goalie equipment uses DB color zones plus template/special maps, and skater helmets use DB RGB
  material color plus logo/recolor texture inputs.

## Blue Equipment / Green Artifact Fix

The blue/green material bug had two different causes during this investigation.

First cause: Release generated-code undefined behavior.

- Symptom: Debug looked correct, Release showed blue/green equipment corruption.
- Fix: compile `app/generated/default/nhl12_recomp.*.cpp` with `-fno-strict-aliasing -ffp-contract=off`.
- Do not remove those flags.
- Follow-up for the normal-play green/black speckle: compile
  `app/generated/default/nhl12_recomp.20.cpp` at `-O0` with the same safety flags.
  This targets the guest-generated equipment texture path and does not disable
  cubemap reflections or rewrite texture-cache bindings.

Second cause: reflective cubemap material path.

- Symptom: helmets, gloves, pads, goalie gear, and other glossy regions turned electric blue while base jerseys mostly remained textured.
- Diagnosis: zeroing `tfetchCube` removed the blue, proving the reflection/specular cubemap path was involved.
- Bad workaround: `nhl_zero_cube_reflection_fetches=true`. This removes helmet reflections and can make goal parts or reflective meshes transparent.
- Correct fix: keep cube fetches live, sample full RGB for any cubemap color use, clamp/cap the
  neutral reflection term so broken cubemap chroma cannot tint equipment, and force cube alpha to
  `1.0`.

Third cause: normal-play cube instruction binding a 2D equipment material map.

- Symptom: goalie pads, blockers/trappers, gloves, and some helmet/glove surfaces show black,
  green, or purple speckled blocks; goalie mitts can look transparent.
- Diagnostic log signature:
  `shader_dim=cube resource_dim=2DOrStacked null=true compatible=false fmt=k_DXT1 size=512x512x1`
  on a sampled fetch, without a matching valid `shader_dim=2D` binding for the same `fetch_dw`.
- Correct fix: keep the real cubemap path for true `DataDimension::kCube` resources, but compile an
  NHL12-only 2D SRV fallback for cube fetches whose runtime fetch constant is not a cube. The
  fallback uses the cube SC/TC operands as 0..1 material UVs and forces 2D array layer 0.
- Current status: the fallback is gated by `nhl_cube_material_2d_fallback=false` by default because
  the first global attempt crashed before reaching the old equipment mismatch. Keep the diagnostic
  code and analyzer checks, but do not ship it enabled until the first-scene crash is fixed.
- Bad workaround: disabling cubemap reflection or zeroing cube fetches. That hides helmet tint bugs
  by removing material inputs and breaks shiny helmets, goals, or reflective surfaces.

Failed experiment: forced native host BC bypass for NHL12 DXT equipment textures.

- Symptom: green/purple/black block noise on helmets, gloves, pads, and goalie gear while base jerseys and rink art mostly remain correct.
- Test: force DXT1, DXT2/3, DXT4/5, DXN, DXT5A, and DXT AS textures through RexGlue's DXT-to-RGBA decode shaders even when aligned.
- Result: no visual improvement, and goalie gloves could become transparent.
- Do not restore `nhl_force_block_texture_decompression`; it was a failed experiment, not a fix.

Failed experiment: RexGlue nightly reverted a `vpkd3d128 float16_4` codegen heuristic that
zero-cleared half of packed float16 vectors. NHL12 uses `vpkd3d128 ...,5,2,2` in generated gameplay
code, so this was backported and the app was regenerated/rebuilt on 2026-06-14. User testing showed
it did not fix the close-up helmet/glove/pad artifacts, so do not treat that path as the renderer
solution.

Implementation files:

```text
RexGlue/src/graphics/flags.cpp
RexGlue/include/rex/graphics/flags.h
RexGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp
RexGlue/include/rex/graphics/pipeline/shader/dxbc_translator.h
```

Current expected defaults:

```text
nhl_force_stacked_texture3d=true
nhl_force_native_draw_resolution=true
nhl_fix_cube_reflection_fetches=true
nhl_zero_cube_reflection_fetches=false
nhl_rebase_mip_min_textures=true
nhl_fix_base_only_packed_bc_textures=true
native_2x_msaa=true
render_target_path_d3d12=rov
resolution_scale=1
draw_resolution_scale_x=1
draw_resolution_scale_y=1
draw_resolution_scaled_texture_offsets=false
```

Equipment mip-window fix:

- Symptom: helmets, gloves, pads, and goalie gear show shifted rectangular black/green/purple chunks,
  as if the texture atlas is warped.
- Active fix: `nhl_rebase_mip_min_textures=true`.
- This rebases non-cubemap `mip_min_level` fetches so the first requested mip is uploaded and sampled
  as logical LOD 0.
- Explicit LOD shaders must stay synchronized with that rebase. `TextureBinding` records the
  successful rebase offset, `SystemConstants::texture_rebased_mip_min_levels` passes it to shaders,
  and DXBC translation subtracts it from explicit non-cubemap LOD fetches.
- Cubemaps are intentionally excluded. Do not use this path to change helmet reflection behavior.

Equipment DXT1/DXT5 packed-state fix:

- User-level equipment map breakdown: colormap carries logos/colors, shine map controls shadows and
  light reflection, and normal map carries wrinkles/shape. Jerseys follow the same pattern, and
  these material maps need mips.
- Embedded RX2 proof is more specific for current goalie/glove samples: diffuse/base maps are often
  `k_DXT2_3`, template/color maps are `k_DXT1`, normal maps are mipped `k_DXN`, and shine/spec maps
  are mipped `k_DXT1`. Jersey/body `texlib` samples are predominantly mipped `k_DXT4_5`, matching
  the user-level "normal map is DXT5" report. Trust embedded fetch constants over filename
  assumptions.
- The 2026-06-15 extracted goalie-equipment sweep proved 2886 real pad/glove/blocker/trapper
  texture RX2 entries: 600 `*_dm` `k_DXT2_3` base-only maps, 1628 `*_tm` `k_DXT1` base-only maps,
  300 `*_nm` `k_DXN` 10-mip packed maps, 300 `*_sm` `k_DXT1` 8/9-mip packed maps, and 58 `*_am`
  `k_DXT1` 7/8-mip packed maps. The same sweep also passed 0 D3D12 runtime-upload layout
  mismatches and 0 renderer-view failures, so static RX2 packed-tail copy boxes and D3D12
  resource/SRV/swizzle choices are currently proven coherent.
- Additional 2026-06-15 proof gates:
  Default unattended renderer suite:
  `python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --nhl12-regression-suite --out build\nhl12_renderer_regression_suite_full`.
  Previous full result before the cube-to-2D fallback patch: PASS in about 5.5 minutes, with 0 failed
  sections. It ran the then-current 43-check source contract, the 70-case renderer-key self-test,
  the 3856-file material contract, 40 DB-colored material-stack previews, the full 2910-container
  goalie equipment upload proof, and 300-file jersey texlib and colormap upload samples. The preview
  section produced 40 passes, 30 exact DB color matches, and PNGs under
  `build\nhl12_renderer_regression_suite_full\material_stack_composites`.
  The source contract must stay green because it protects the app/RexGlue flags that preserve
  colormap logo/color maps, shine/reflection maps, and normal shape maps without disabling cubemap
  reflections, and it now guards the C++ base-only generated BC branch that strips stale packed
  state for DXT1/DXT2/3/DXT4/5 while preserving tiny packed-base textures, plus the DXBC
  cube-to-2D fallback used by NHL12 equipment material fetches.
  Fast iteration suite:
  `python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --nhl12-regression-suite --nhl12-regression-suite-fast --out build\nhl12_renderer_regression_suite_fast`.
  Latest result after the cube-to-2D fallback patch: PASS in about 2.1 minutes, with the 47-check
  source contract, 70 renderer-key self-test cases, and 0 failed sections. Use this while iterating,
  then run the full suite before handing off renderer/material changes. Saturated green/purple in
  these preview metrics is allowed only when it comes from the selected DB color palette.
  `python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --glob "jersey\texlib*.rx2" --glob "jersey\jersey*.rx2" --glob "czjersey\*.rx2" --limit 300 --out build\texture_proof_jersey_texlib_material_maps --no-images`
  passed 300/300, including 299 mipped `k_DXT4_5` 512x512 10-mip maps and one mipped `k_DXT1`
  1024x1024 11-mip map.
  `python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --glob "jersey\name_*_cm.rx2" --limit 300 --out build\texture_proof_jersey_nameplate_k8888 --no-images`
  passed 300/300 mipped `k_8_8_8_8` 256x256 nameplate/color-support maps.
- Fast material-contract gate:
  `python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --scan-material-contract-dir goaliepad --scan-material-contract-dir glove --scan-material-contract-dir blocker --scan-material-contract-dir trapper --scan-material-contract-dir jersey --out build\texture_material_contract_equipment_jersey`.
  Latest result: 3856 material-map files checked in about 32 seconds, 0 failures, 0 warnings.
  Coverage: 970 colormap/support maps, 300 normal maps, 300 shine maps, 58 alpha/support maps,
  600 base-only diffuse/recolor masks, and 1628 base-only template recolor masks.
- The verifier also mirrors the NHL12 renderer-key normalization rule. The full unattended gate is:
  `python tools\nhl12_texture_proof.py --self-test-renderer-key --out build\texture_proof_goalie_equipment_renderer_key`,
  then
  `python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --glob "goaliepad\*.rx2" --glob "glove\*.rx2" --glob "blocker\*.rx2" --glob "trapper\*.rx2" --limit 5000 --out build\texture_proof_goalie_equipment_renderer_key --no-images`.
  Latest result: 70/70 renderer-key cases passed, 0 corpus failures, 0 renderer-key failures, 0
  renderer-view failures, and 658 real mipped goalie-equipment material maps protected from losing
  packed mip state. The synthetic cases include mipped DXN normals, mipped DXT5 normals, a mipped
  8.8.8.8 support map, and 64 old normal-play generated base-only BC fetches that must strip stale
  packed state without requiring a human to leave Winter Classic. The generated coverage is
  2 `k_DXT1`, 5 `k_DXT2_3`, and 57 `k_DXT4_5` fetches from 32x32 through 2048x1024.
- Use the material-stack proof when changing equipment upload/sampler behavior:
  `python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --material-set goaliepad:0_11 --material-set glove:0_15 --out build\texture_material_artifact_metrics --material-size 512 --no-images`.
  Latest representative result: `goaliepad:0_11` and `glove:0_15` passed with zero neon-green,
  purple, and near-black artifact ratio across LOD 0, 1, 2, and the lowest mip. Transparency is
  reported but not treated as failure because these previews include legitimate alpha/cutout UV
  space.
- Use the discovered material proof for broad unattended coverage. For current goalie-equipment
  corruption, include pads, gloves, blockers, and trappers:
  `python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --discover-material-dir goaliepad --discover-material-dir glove --discover-material-dir blocker --discover-material-dir trapper --discover-material-limit 40 --out build\texture_material_goalie_equipment_db_sample --material-size 64 --no-images --db-file extracted\cache_hdd\db\nhlng.db --db-meta extracted\cache_hdd\db\nhlng-meta.xml --db-bit-order msb`.
  Latest result: 40 interleaved complete material stacks passed with 0 failures, including exact DB
  color matches for pad, blocker, and trapper/glove equipment rows.
- Use DB-colored material proof before changing recolor/template behavior:
  `python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --material-set goaliepad:0_11 --material-set glove:0_15 --out build\texture_material_db_colors --material-size 256 --no-images --db-file extracted\cache_hdd\db\nhlng.db --db-meta extracted\cache_hdd\db\nhlng-meta.xml --db-bit-order msb`.
  Latest representative result: 0 failures with exact DB matches for `goaliepad:0_11` and
  `glove:0_15`. A 40-stack DB-colored discovery sample also passed; see
  `docs\nhl12_equipment_db_color_notes.md`.
- Active fix: `nhl_fix_base_only_packed_bc_textures=true`.
- For NHL12 only, base-only 2D BC texture fetches such as DXT1, DXT2/3, and DXT4/5 ignore a stale
  packed-mips bit only when level 0 is not itself packed. Real mipped maps keep their mip chains,
  and tiny base-only BC maps keep packed layout because Xenos can store level 0 inside the packed
  tail.
- Active sampler guard: `nhl_preserve_equipment_mips_without_forced_aniso=true`.
- This keeps real mips available while preventing RexGlue's host-side anisotropic override from being
  forced onto mipped NHL12 equipment/jersey material maps. Covered maps include DXT1 color/shine,
  DXT4/5 or DXN normals, and the proven mipped 8.8.8.8 support/nameplate maps.
- Cubemaps are intentionally excluded. Do not use this path to change reflection behavior.
- Runtime normal-play proof must include both `[NHL-TEX]` and `[NHL-SAMPLER]` diagnostics:
  `.\app\out\build\win-amd64-release\nhl12.exe --game_data_root extracted --log_file build\nhl12_normalplay_equipment_sampler.log --log_level info --mnk_mode=false --nhl_log_texture_bindings=true`,
  then
  `python tools\nhl12_texture_proof.py --analyze-log build\nhl12_normalplay_equipment_sampler.log --catalog-report build\texture_proof_goalie_equipment_renderer_key\report.json --out build\nhl12_normalplay_equipment_sampler_analysis`.
- Runtime log analysis should report catalog strict/loose/unmatched counts. Strict catalog matches
  prove the live binding agrees with extracted RX2 format, dimensions, mip count, packed state,
  tiled state, and pitch. Loose matches or unmatched generated textures are investigation leads, not
  automatic proof of corruption.
- Runtime log analysis filters unused null signed/unsigned SRV slots. A null descriptor is fatal
  only when the shader can actually sample that slot according to the runtime `signs` field. It also
  re-decodes `fetch_dw` and compares the logged packed state against current NHL12 normalization.
- Runtime log analysis accepts a rejected/null cube descriptor only if the same fetch constant and
  signedness also have a valid non-null compatible 2D fallback binding, and the runtime fetch
  constant dimension is not `DataDimension::kCube`. Old logs without that fallback must still fail.
- Prefer `strict+shape` catalog matches when diagnosing pad/glove/blocker/trapper corruption. Shape
  matching decodes logged `fetch_dw` values and compares non-address raw fetch metadata, including
  swizzle and signs. If a live binding is strict but not shape, investigate raw fetch interpretation
  before changing RX2 upload or packed-tail code.
- The analyzer is expected to fail old logs from before the NHL12 packed-state/sampler fixes. Use
  those old failures only as negative controls, not as proof that the current executable is wrong.

## What Not To Do

- Do not fix blue textures by disabling reflections.
- Do not set `nhl_zero_cube_reflection_fetches=true` for normal gameplay.
- Do not disable MSAA as a persistent performance fix unless visuals are revalidated.
- Do not use `resolution_scale`, `draw_resolution_scale_x`, or `draw_resolution_scale_y` above 1
  for NHL12 gameplay. This can shift equipment/goalie texture atlases and reintroduce green/purple
  artifacts.
- Do not restore forced DXT/BC decompression as an equipment fix. It did not fix the corruption and
  caused transparent material regressions.
- Do not collapse all equipment corruption into one fix. Check the skater helmet DB-color/logo path
  separately from the goalie pad/blocker/trapper zone-color/template/special-map path.
- Do not "fix" goalie pad artifacts by bypassing `*_tm` or `*_sm` maps. Those maps are part of the
  color-customization/material path and must stay available to the shader.
- Do not treat signed DXN/DXT5A as the active pad/glove explanation without fresh diagnostic proof.
  The latest confirmed RX2 proof shows mipped DXN normals and DXT1 shine/spec maps for goalie pads,
  while jersey/body `texlib` samples prove mipped DXT4/5 needs the same mip-preservation protection.
- Do not treat DXN as requiring a separate signed resource. The proven D3D12 path is one BC5
  typeless resource with BC5 UNORM and BC5 SNORM SRV views.
- Do not disable or bypass equipment mips. Use `tools\nhl12_texture_proof.py` to prove the RX2 mip
  chain before changing texture upload or sampler behavior.
- Do not "fix" helmet artifacts by removing logo/recolor/env inputs. The helmet shell may be DB RGB,
  but the logo, recolor template, specular, and cubemap reflection inputs still need to work.
- Do not switch NHL12 to RTV for gameplay testing.
- Do not trust frontend `[SWAP-FPS]` alone as proof that gameplay rendering is correct.
- Do not change shader translation without bumping the DXBC shader modification version.
- Do not remove `texture_rebased_mip_min_levels` or `normalize_rebased_texture_lod`; without the
  shader-side LOD correction, mip-rebased equipment can still sample shifted helmet/glove/pad data.
- Do not apply the explicit LOD correction to cubemaps. That risks breaking the helmet reflection fix.
- Do not leave renderer docs saying cube zeroing is the active fix.
- Do not continue chasing the `vpkd3d128 float16_4` nightly backport as the material fix; user
  testing says it did not fix the artifact.
- Do not disable `nhl_rebase_mip_min_textures` when testing equipment artifacts unless you are doing
  an explicit A/B of mip-window behavior.

## Visual Regression Checklist

After renderer changes, verify these in live gameplay:

- helmets have visible reflection, not flat matte color;
- helmets and gloves are not saturated blue, green, purple, or speckled;
- goalie pads and gear do not show green artifacts;
- goals and reflective rink objects are not transparent;
- the 3D rink/gameplay scene is not black;
- base jerseys remain textured;
- skater and goalie equipment does not show shifted green/purple/black atlas patches;
- close-up equipment does not show BC-like block noise in helmets, gloves, pads, or goalie gear;
- skater helmets preserve DB/team color, logo placement, and reflection without speckled template
  contamination;
- goalie pads/blockers/trappers preserve custom zone colors without green/purple special-map bleed;
- 1080p windowed and fullscreen/high-resolution paths both use the same material behavior.

After video/texture-cache changes, verify:

- intro and transition videos do not have a green cast;
- luma/chroma motion looks aligned, not purple/green or checkerboarded;
- menu/gameplay 3D materials still match the confirmed working renderer backup.
- cubemap reflections remain enabled: helmets are shiny and goals are opaque.

## Build And Smoke Commands

Rebuild ReXGlue and the app:

```powershell
cmake --build RexGlue\out\build\win-amd64 --config Release --target install -j 1
cmake --build app\out\build\win-amd64-release --target nhl12 -j 4
```

Smoke launch:

```powershell
Get-Process nhl12 -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item -ErrorAction SilentlyContinue build\nhl12_renderer_smoke.log, build\nhl12_crash.txt
$args = @('--game_data_root','extracted','--log_file','build\nhl12_renderer_smoke.log','--log_level','info','--mnk_auto_progress=true')
$p = Start-Process -FilePath 'app\out\build\win-amd64-release\nhl12.exe' -ArgumentList $args -WorkingDirectory (Get-Location) -PassThru
```

This only proves launch and presentation. A human gameplay check is still required for material/reflection correctness.

## Last Confirmed Good Fix

On 2026-06-14, the user confirmed the active cube reflection fix worked:

```text
Yes !! Nice work !
```

That confirmation means the renderer state at this point had:

- blue tint fixed;
- helmet reflections restored;
- goals no longer transparent;
- `nhl_zero_cube_reflection_fetches=false`;
- `nhl_fix_cube_reflection_fetches=true`;
- DXBC shader modification version `0x20260618`.
