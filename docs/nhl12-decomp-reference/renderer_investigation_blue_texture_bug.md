# Renderer Investigation: Blue / Green Texture Corruption

Date: 2026-06-14

## Purpose

NHL12 currently renders the 3D scene, but some player and goalie materials can turn matte blue or show green artifacts. The bug was previously fixed in Release at 1080p windowed, then returned after renderer experiments around MSAA/fullscreen/resolution. This document records what is known about the renderer path before making another source patch.

The working assumption is that this is a renderer state, shader translation, or texture binding issue. It should not be patched by random toggles.

## Known Timeline

- Debug was visually correct but slow.
- Release was fast but had blue/green player and equipment corruption.
- `docs/Conversation WIth Claude.md` identified the first Release corruption as generated-code undefined behavior: the `nhl12_recomp.*.cpp` files reinterpret guest memory, so Release needs `-fno-strict-aliasing`; Debug survived because it used `-O0`.
- `app/CMakeLists.txt` now scopes `-fno-strict-aliasing -ffp-contract=off` to `nhl12_recomp.*.cpp`, while `nhl12_init.cpp` and `nhl12_register.cpp` stay at `-O0` to avoid compiler memory/ICE issues.
- The current Release build file confirms the generated recompiled units are built with `-O3 -fno-strict-aliasing -ffp-contract=off`.
- ROV is forced because the default D3D12 RTV path shows UI but produces a black 3D scene for this title.
- `native_2x_msaa` was temporarily disabled during a performance A/B. That did not solve FPS and correlated with the user seeing the texture corruption return.
- A later D3D12 descriptor / 3D-as-2D wrapper invalidation experiment made gameplay black while the stadium still appeared. That was a regression and was rolled back.
- Current source still contains an experimental raw fetch descriptor reload guard in the generic texture cache. It was plausible, but user testing says it did not fix the blue bug. Treat it as unproven, not as the solution.

## Current App Renderer Settings

`app/src/nhl12_app.h` currently does the following for NHL12:

- Defaults `game_data_root` to `extracted` if not supplied.
- Uses `C:\Users\thrif\Documents\nhl12\cache_rov_windowed` as the runtime cache root.
- Calls `ResetNhl12ShaderStorage(paths.cache_root)`, which renames/removes `cache_rov_windowed\shaders` every launch.
- Forces `render_target_path_d3d12=rov`.
- Allows invalid fetch constants with `gpu_allow_invalid_fetch_constants=true`.
- Restores `native_2x_msaa=true`.
- Disables vsync and caps guest swaps at 60 FPS.
- Shows a resolution-only launch dialog unless auto-progress is enabled.
- The current resolution dialog resizes the host window only. It does not currently write the old guest `video_mode_*` cvars or force fullscreen presentation state.

Latest smoke log:

- `build/nhl12_regression_rollback.log`
- Cache root: `C:\Users\thrif\Documents\nhl12\cache_rov_windowed`
- Shader storage initialized for title `45410964`
- Frontend swap logs hover around 58-60 FPS

That smoke test proves launch/frontend presentation still works. It does not prove gameplay textures are correct.

## Renderer Architecture Map

Active renderer stack:

- Recompiled game code writes Xbox 360 GPU registers and command buffers.
- `RexGlue/src/graphics/command_processor.cpp` consumes guest command packets.
- `RexGlue/src/graphics/d3d12/command_processor.cpp` implements the D3D12 backend.
- `RexGlue/src/graphics/d3d12/pipeline_cache.cpp` owns shader translation and D3D12 pipeline creation/storage.
- `RexGlue/src/graphics/pipeline/shader/dxbc_translator*.cpp` translates Xenos shaders to DXBC.
- `RexGlue/src/graphics/pipeline/texture/cache.cpp` normalizes guest texture fetch constants into `TextureKey` objects, loads texture memory, and tracks binding state.
- `RexGlue/src/graphics/d3d12/texture_cache.cpp` creates D3D12 resources and SRV descriptors for those texture bindings.
- `RexGlue/src/graphics/d3d12/render_target_cache.cpp` emulates EDRAM render target behavior.
- `D3D12CommandProcessor::IssueSwap` resolves/presents the guest frontbuffer and logs `[SWAP-FPS]`.

For NHL12, ROV is the known-good 3D path. RTV is not a safe fallback because it can leave the gameplay scene black while UI still renders.

## Shader Storage Behavior

`RexGlue/src/graphics/d3d12/pipeline_cache.cpp` initializes persistent shader storage under:

```text
<cache_root>\shaders\shareable
```

It stores D3D12 pipeline descriptions separately for ROV and RTV:

```text
45410964.rov.d3d12.xpso
45410964.rtv.d3d12.xpso
```

The file header also includes a D3D12 API magic value:

- `DXRO` for ROV
- `DXRT` for RTV

So direct ROV/RTV pipeline file contamination is less likely.

However, guest shader storage is shared:

```text
45410964.xsh
```

When shader storage is loaded, the translator is configured with:

- adapter vendor id
- bindless-resource mode
- whether ROV is used
- gamma render target mode
- MSAA support
- draw resolution scale
- graphics-analysis/debug mode

The important question is whether every visual-affecting mode is reflected in the shader modification key used for translation. If a setting changes shader behavior but is not encoded in the key, a cached translation may be reused in a state it was not built for.

The app now clears `cache_rov_windowed\shaders` each launch. If the bug survives that, either:

- the wrong cache root is being cleared,
- the corruption is not from persistent shader storage,
- another content/runtime cache is involved,
- the bug is recreated deterministically every run by current renderer logic,
- or the exe being launched is not the rebuilt exe we think it is.

## Texture Fetch And Stacked 3D Path

This is the highest-value code path for the current artifact.

Xbox 360 texture fetch constants store stacked textures as `DataDimension::k2DOrStacked`, while shader `tfetch3D` instructions can access either true 3D or stacked 2D data.

Relevant definitions:

- `RexGlue/include/rex/graphics/xenos.h`
- `DataDimension::k2DOrStacked`
- `FetchOpDimension::k3DOrStacked`

NHL12 has a dedicated cvar:

```text
nhl_force_stacked_texture3d = true
```

Defined in:

```text
RexGlue/src/graphics/flags.cpp
```

Description:

```text
Treat tfetch3D texture data as stacked 2D for NHL color lookup workarounds
```

The DXBC translator uses this flag in `dxbc_translator.cpp` by writing `force_stacked_texture3d` into both vertex and pixel shader modification bits. Then `dxbc_translator_fetch.cpp` uses that bit to force `tfetch3D` shaders onto the stacked branch:

## 2026-06-14 NHL12 Equipment Mip Rebase Patch

Normal gameplay artifacts on helmets, gloves, and goalie gear eventually looked less like cubemap
tint and more like shifted source data. User research also separated the material families:

- goalie pads/blockers/trappers use DB color zones plus template/special maps;
- skater helmets use DB/team RGB for most of the shell, with logo textures and env/spec inputs.

The patch therefore targets texture view addressing, not reflection removal and not blanket texture
decode overrides.

Active behavior:

- `nhl_rebase_mip_min_textures=true` lets `TextureCache::BindingInfoFromFetchConstant` rebase
  non-cubemap mip-window texture keys when the requested mip offset is page-clean.
- The cache records the exact successful offset in `TextureBinding::rebased_mip_min_level`.
- `D3D12CommandProcessor` packs that value into `SystemConstants::texture_rebased_mip_min_levels`.
- `dxbc_translator_fetch.cpp` subtracts the offset from explicit non-cubemap LOD fetches and clamps
  to zero.
- DXBC translator version `0x20260620` prevents stale shader reuse across the new behavior.

Guardrails:

- cube textures are excluded from both the texture-key rebase and shader LOD normalization;
- failed packed/unaligned mip-tail rebases record 0, so shaders do not compensate blindly;
- computed/implicit LOD fetches are not changed;
- this must coexist with `nhl_fix_cube_reflection_fetches=true` and
  `nhl_zero_cube_reflection_fetches=false`.

- if not forced, the generated shader reads the fetch constant dimension and chooses true 3D vs stacked at runtime;
- if forced, it writes the "is 3D" temporary as false, so the shader samples the stacked path.

D3D12 texture binding then needs to match that shader choice. In `RexGlue/src/graphics/d3d12/texture_cache.cpp`:

- `RequestTextures` pre-creates 3D-as-2D wrapper resources for 3D texture bindings.
- `WriteActiveTextureBindfulSRV` / `GetActiveTextureBindlessSRVIndex` may force a special 2D-array view when a 3D guest texture is used by a 1D/2D shader binding.
- `FindOrCreateTextureDescriptor` can create a `TEXTURE2DARRAY` SRV over a 3D-as-2D wrapper resource.
- `GetOrCreate3DAs2DResource` creates a single-slice 2D wrapper and loads it via the texture loading path with `SetForceLoad3DTiling(true)`.

The failed regression came from invalidating this wrapper/descriptors too aggressively. That produced black gameplay, which means this path is fragile and should be instrumented before being changed again.

## Texture Cache Behavior

`TextureCache::RequestTextures`:

- reads each used fetch constant from the register file;
- converts it to a normalized `TextureKey` using `BindingInfoFromFetchConstant`;
- computes host swizzle and signedness;
- finds/creates texture objects;
- queues pending texture loads;
- updates backend descriptors when bindings change.

The current source does not store raw fetch dwords in `TextureBinding`; binding identity is the
normalized `TextureKey`, host swizzle, and swizzled signedness. That means two fetch descriptors that
normalize to the same key still share the same loaded texture resource and descriptors.

That is usually correct for generic Xenos behavior, but NHL12's close-up equipment artifacts may
still need a more title-specific check around template/special-map material inputs if a raw fetch
state affects shader LOD, recolor overlay selection, or the intended view of a mip-only asset.

## 2026-06-15 Normal-Play Equipment Cube/2D Binding Mismatch

Fresh normal-play diagnostics in `build/nhl12_normalplay_equipment_sampler.log` exposed a separate
runtime renderer failure after the static RX2 upload path had already passed offline proof.

The bad live binding looked like this:

```text
fetch=2 shader_dim=cube resource_dim=2DOrStacked descriptor_dim=2DOrStacked
compatible=false null=true fmt=k_DXT1 size=512x512x1 mip_min=0 mip_max=9 packed=true
fetch_dw=84024802,13D0B052,003FE1FF,00A80D10,00000243,13D2BA00
```

That means the shader instruction was `tfetchCube`, but the runtime fetch constant pointed at a
regular 2D mipped DXT1 material map. D3D12 correctly rejected the cube SRV because cube bindings are
only compatible with `DataDimension::kCube`, so the translated shader sampled a null descriptor.
This explains green/black/transparent corruption on gloves, pads, and glossy equipment without
requiring the extracted RX2 files themselves to be corrupt.

The NHL12-specific renderer fix is in
`RexGlue/src/graphics/pipeline/shader/dxbc_translator_fetch.cpp`:

- keep `nhl_fix_cube_reflection_fetches=true` and keep real cubemap sampling alive;
- for NHL12 cube fetches, read fetch-constant word 5 bits 9..10 at runtime;
- if the runtime data dimension is `kCube`, use the existing cubemap coordinate and cube SRV path;
- if the runtime data dimension is not `kCube`, also compile a 2D SRV fallback for the same fetch
  constant and signedness;
- use the cube instruction SC/TC operands as face-local 0..1 material UVs by subtracting 1 from the
  1..2 cube operands, and force the 2D array layer to 0.

This is not reflection disabling. It is a binding-shape correction for NHL12 equipment/material
fetches that arrive through a cube-shaped shader instruction while binding 2D color/shine/normal
maps.

Status update: the first implementation crashed at first 3D scene load with `0xc0000005` before it
reached the old `512x512x1` equipment mismatch. The code remains available behind
`nhl_cube_material_2d_fallback`, but that cvar now defaults to `false` so the known helmet
reflection fix can stay enabled without taking the crashy experimental fallback path.

Analyzer behavior:

- old logs without the matching valid 2D fallback remain failures;
- new logs may contain a rejected/null cube descriptor for the same fetch, but they must also contain
  a valid non-null compatible `shader_dim=2D` binding for the same `fetch_dw` and signedness;
- 128x128x6 real cubemap sampler lines are no longer treated as equipment material sampler warnings.

## Ranked Hypotheses

1. Stacked 3D / color lookup path mismatch.

The visual symptom is player/equipment-specific blue/green corruption, not an entire black screen or broken presenter. The existing NHL-specific `nhl_force_stacked_texture3d` cvar explicitly mentions color lookup workarounds. This is the most relevant renderer path.

2. Shader translation cache key misses a mode bit.

Fullscreen or MSAA changes may have caused shaders/pipelines to be translated under one renderer state and reused under another. ROV/RTV pipeline files are separated, but `.xsh` is shared guest shader storage, and translations are reconstructed from stored pipeline modification bits. We need confirm all relevant bits are encoded, especially `force_stacked_texture3d`, MSAA, resolution scale, bindless mode, and ROV.

3. Cache root mismatch or stale storage outside `cache_rov_windowed`.

Current app clears `cache_rov_windowed\shaders`, but older exes may have written to `C:\Users\thrif\Documents\nhl12\cache\shaders` or another root. If the launched exe is older or the cache root differs, stale shader storage can still be reused.

4. D3D12 SRV descriptor lifetime or dimension mismatch.

The shader can request a 3D SRV, 2D array SRV, cube SRV, signed SRV, or unsigned SRV. If the texture cache returns the null descriptor or a descriptor with the wrong dimension/swizzle, the result can become flat wrong color rather than total renderer failure.

5. Texture upload/load interpretation issue.

`GetOrCreate3DAs2DResource` uses a derived 2D `TextureKey`, forces 3D tiling load behavior, and uploads a wrapper. If that wrapper is stale or loaded with the wrong slice/depth/swizzle/sign state, skater equipment textures or lookup tables can corrupt.

6. Recompiled game-code flags missing in the actually launched exe.

The current build files look correct. But if the user launched an older exe, the original Release strict-aliasing bug would produce the exact same color corruption. Any proof step must verify the exact exe path and build timestamp.

7. MSAA itself.

MSAA is less likely as the direct cause because disabling it did not improve performance and the artifact survived after restoring it. But MSAA changes render target/shader modification behavior, so it can still matter indirectly through cache keys or stale shader storage.

## Things Not To Do Next

- Do not blindly disable MSAA again in the shared cache.
- Do not switch to RTV as a test for gameplay correctness; RTV is already known to black out the 3D scene.
- Do not invalidate 3D-as-2D wrapper resources/descriptors without proving the synchronization and descriptor lifetime rules.
- Do not assume the compile flag fix is absent; current Release build metadata shows it is present.
- Do not rely on frontend `[SWAP-FPS]` logs as proof of gameplay texture correctness.

## Next Investigation Plan

Before the next renderer code patch:

1. Verify the exact exe being launched.

Record:

- path
- modified timestamp
- whether `TextureCache: fetch-descriptor reload guard active` appears
- cache root printed in the log

2. Force a fully clean renderer cache test.

Temporarily quarantine all likely shader storage roots:

```text
C:\Users\thrif\Documents\nhl12\cache\shaders
C:\Users\thrif\Documents\nhl12\cache_rov_windowed\shaders
```

Then launch 1080p windowed and check the first gameplay frame. If clean cache fixes it once, persistent shader storage is implicated. If it immediately returns, current renderer logic is implicated.

3. Add instrumentation, not behavior changes, around stacked texture fetch.

Log a small capped sample when:

- `nhl_force_stacked_texture3d` is used in a translated shader;
- a `FetchOpDimension::k3DOrStacked` shader binding is created;
- a fetch constant with `DataDimension::k3D` or `k2DOrStacked` is bound;
- `FindOrCreateTextureDescriptor` creates a 3D SRV vs 2D-array wrapper SRV;
- `GetOrCreate3DAs2DResource` creates or reuses a wrapper;
- a null SRV is used for a shader texture binding that should be real.

The log should include fetch index, shader type, guest dimension, shader fetch dimension, format, width, height, depth/array size, base page, mip page, host swizzle, signedness, descriptor dimension, and whether the special 3D-as-2D path was used.

4. Add shader-cache instrumentation.

Log the pipeline and shader storage file names at open, plus the translator mode bits used to replay stored shaders:

- ROV vs RTV
- bindless mode
- MSAA support
- draw resolution scale
- `nhl_force_stacked_texture3d`
- gamma render target mode
- graphics-analysis mode

If a visual-affecting bit is not part of the modification/version key, either add it to the key/version or disable persistent storage for NHL12 until the key is correct.

5. Reproduce with only one variable changed at a time.

Baseline should be:

- fresh shader cache
- 1080p windowed
- ROV
- `native_2x_msaa=true`
- `nhl_force_stacked_texture3d=true`
- exact rebuilt exe

Only after that baseline is visually confirmed should resolution or fullscreen/presentation be changed.

## Final Resolution

The investigation found two separate blue/green material failure modes.

The original Release-only corruption was generated-code undefined behavior. The fix is still required:

```text
-fno-strict-aliasing
-ffp-contract=off
```

The later glossy-material corruption was caused by the cubemap reflection path. It appeared mostly on helmets, gloves, pads, goalie gear, and other reflective materials.

Diagnostic result:

```text
nhl_zero_cube_reflection_fetches=true
```

removed the saturated blue, proving `tfetchCube` was involved. That was not a valid fix because it also removed helmet reflections and made goals/reflective meshes transparent.

Final gameplay fix:

```text
nhl_fix_cube_reflection_fetches=true
nhl_zero_cube_reflection_fetches=false
```

The DXBC translator keeps cube texture samples live, samples full RGB when any cubemap color channel is used, clamps the broken blue-only reflection contribution, and forces cube fetch alpha to `1.0`. The user confirmed the earlier version restored the expected look in one gameplay path: blue tint fixed, reflections preserved, and goals no longer transparent.

The DXBC shader modification version was bumped to:

```text
0x20260618
```

so stale translated shaders cannot hide or reintroduce the old cube-fetch behavior.

## Later Equipment Block-Noise Finding

After the cube reflection fix, close-up replay and normal gameplay screenshots still showed
green/purple/black block noise on helmets, gloves, pads, and goalie equipment. That artifact is
different from the cubemap blue tint: it can remain even when cube RGB is neutralized and alpha is
forced to `1.0`.

Failed experiment:

```text
nhl_force_block_texture_decompression=true
```

This forced DXT1, DXT2/3, DXT4/5, DXN, DXT5A, and DXT AS guest textures through RexGlue's existing
decode load shaders instead of uploading aligned textures as native host BC resources. User testing
showed no improvement and introduced transparent goalie gloves, so this was removed as an active
fix. Keep this separate from reflection fixes: disabling cube samples removes material shine, while
forced block decompression can break material alpha.

## Later Equipment Composition Finding

User-provided NHL12 content research plus extracted DB/shader evidence shows that the remaining
helmet and goalie-equipment artifacts must be treated as material-composition bugs, not only as
generic texture-format bugs.

Goalie equipment:

- `exhibitiongoalieequipment` exposes many per-zone RGB fields for pads, blocker, trapper, and stick.
- `showcustomcolors` and `showcustomstick` control whether those custom colors are shown.
- `rendering/goaliepad` contains many `*_tm.rx2` template/tint maps and `*_sm.rx2` special/specular
  maps in addition to diffuse and normal maps.
- The visible green artifacts on goalie pads/gloves are therefore likely in the recolor/template or
  special-map fetch path, not necessarily in the base diffuse texture.

Skater helmets:

- User research says the shell color is DB/RGB driven, while the logo is the changeable texture.
- `helmet*.fxo` exposes separate `diffuseLogoSampler*`, `diffuseFontSampler`, `specSampler`,
  `envSampler`, `aoSampler`, and `normalSampler` inputs.
- `helmet*_cz.fxo` exposes `recolor`, `recolor_detail`, and `recolor_template1`.
- `stockteamcolorstable` exposes `color_r`, `color_g`, and `color_b`.

Practical consequence: a white/black helmet with shifted black or colored patches is probably a
wrong overlay/template/logo/recolor/material fetch on top of a DB-colored shell. A future patch
should identify which fetch binding supplies the bad overlay before changing generic BC decode,
reflection, or all texture-cache behavior.

## Later Equipment DXT1/DXT5 Mip Finding

The next narrowed symptom was: goalie helmets looked correct, but goalie pads and player gloves
remained black/green/purple corrupted. That points away from cubemap helmet reflection and toward the
equipment template/special-map path.

User-provided equipment research later clarified that the broken equipment stack is conceptually:

```text
color map  -> DXT1
shine map  -> DXT1
normal map -> DXT5
```

Jerseys and general equipment follow the same pattern and need mip chains. The renderer must still
trust embedded RX2 fetch constants over shorthand names. The 2026-06-15 offline proof found the
current goalie/glove sample set as `k_DXT2_3` diffuse/base maps, `k_DXT1` template maps, mipped
`k_DXN` normals, and mipped `k_DXT1` shine/spec maps. That makes the current pad/glove artifact a
mipped material binding/sampler problem first, not a signed DXN/DXT5A problem by default.

The expanded 2026-06-15 verifier sweep checked the extracted `goaliepad` and `glove` directories
and passed 1346 real texture RX2 entries with 0 decode/layout failures. The distribution was:

```text
272 diffuse/base maps      -> k_DXT2_3, 1 mip, base-only
748 template/color maps    -> k_DXT1,   1 mip, base-only
136 normal maps            -> k_DXN,    10 packed mips
136 shine/specular maps    -> k_DXT1,   8 packed mips
54  alpha/aux maps         -> k_DXT1,   8 packed mips
```

The same sweep now includes a D3D12 runtime-upload simulation. It creates the padded upload
footprint, applies the packed-tail `CopyTextureRegion` source boxes, and compares those bytes to the
direct decoded mip data. The goalie/glove corpus passed with 0 runtime-upload mismatches.

The verifier now includes a renderer-key normalization proof as well as byte/upload proof. The latest
full unattended gate is:

```powershell
python tools\nhl12_texture_proof.py --self-test-renderer-key --out build\texture_proof_goalie_equipment_renderer_key
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --glob "goaliepad\*.rx2" --glob "glove\*.rx2" --glob "blocker\*.rx2" --glob "trapper\*.rx2" --limit 5000 --out build\texture_proof_goalie_equipment_renderer_key --no-images
```

The renderer-key self-test now passes 70 cases: 6 hand-written material edge cases plus 64 generated
normal-play fetches harvested from the old diagnostic log. Those generated cases cover 2 `k_DXT1`,
5 `k_DXT2_3`, and 57 `k_DXT4_5` base-only BC fetches from 32x32 through 2048x1024; every one must
strip stale packed state. The corpus run scans 2910 RX2 containers, skips 24 non-texture containers,
passes 2886 textured entries, and reports 0 renderer-key failures. The key proof explicitly protects
658 mipped material maps from losing packed mip state: 300 `k_DXN` normal maps and 358 mipped
`k_DXT1` shine/alpha maps.

`tools\nhl12_texture_proof.py` also has a material-stack mode now:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --material-set goaliepad:0_11 --material-set glove:0_15 --out build\texture_material_artifact_metrics --material-size 512 --no-images
```

That command writes LOD composite PNGs for a goalie pad and glove stack. These are not NHL12 shader
outputs, but they prove that the same UVs can sample the decoded diffuse/template/normal/shine maps
together coherently. If gameplay still corrupts while this proof is clean, the bug is likely in
runtime fetch binding, shader LOD/sign/swizzle handling, or game-generated colorized equipment data,
not in the static RX2 bytes.

The material-stack proof now records artifact metrics for the generated composites. The latest
representative run passed `goaliepad:0_11` and `glove:0_15` with `green=0.0000`, `purple=0.0000`,
and `black=0.0000` across LOD 0, 1, 2, and the lowest mip. It reports transparent UV-sheet coverage
as a note because that alpha/cutout space is expected in the static material sheet.

The proof can also discover complete material stacks automatically:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --discover-material-dir goaliepad --discover-material-dir glove --out build\texture_material_discovery_full --material-size 64 --no-images
```

The latest full-family run found and passed 264 complete stacks: 164 goaliepad and 100 glove. Across
1056 generated composite metrics, max neon-green ratio was 0 and max purple ratio was 0. This is
strong evidence that the static extracted pad/glove material sheets are not intrinsically green or
purple-corrupt.

The proof now has an optional DB-color path for the goalie equipment color zones:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --material-set goaliepad:0_11 --material-set glove:0_15 --out build\texture_material_db_colors --material-size 256 --no-images --db-file extracted\cache_hdd\db\nhlng.db --db-meta extracted\cache_hdd\db\nhlng-meta.xml --db-bit-order msb
```

That path decodes `exhibitiongoalieequipment` from `nhlng.db` using `nhlng-meta.xml`, then records
which DB equipment rows supplied the tint colors. Latest representative result passed with one exact
`pads` row for `goaliepad:0_11` and three exact `trapper` rows for `glove:0_15`. A 40-stack
DB-colored discovery sample also passed with 27 exact DB matches and 13 DB global fallbacks. See
`docs\nhl12_equipment_db_color_notes.md`.

Active source fixes for this path:

```text
nhl_rebase_mip_min_textures=true
nhl_fix_base_only_packed_bc_textures=true
nhl_preserve_equipment_mips_without_forced_aniso=true
```

`nhl_rebase_mip_min_textures` keeps mip-window views coherent with shader LOD. The newer
`nhl_fix_base_only_packed_bc_textures` ignores a stale packed-mips bit on base-only 2D BC fetches,
while preserving real mipped maps and preserving tiny base-only maps whose level 0 is genuinely
stored in a packed tail. `nhl_preserve_equipment_mips_without_forced_aniso`
keeps mips live but stops RexGlue's host-side anisotropic override from being forced onto mipped
NHL12 equipment/jersey material maps. In user-facing terms, the colormap carries logos/colors, the
shine map controls shadow/reflection response, and the normal map carries wrinkles/shape. In extracted
RX2 terms, this includes DXT1 shine maps, DXT4/5 or DXN normal maps, and the mipped 8.8.8.8
colormap/support/nameplate maps found in jersey assets. Cubemaps are excluded from these fixes so the
reflection sanitizer remains independent.

The earlier signed DXN/DXT5A work is now only a failed or unproven lead for this symptom. Revisit it
only if a fresh binding diagnostic proves NHL12 is sampling signed DXN/DXT5A in the broken draw.

2026-06-15 diagnostic upgrade: `nhl_log_texture_bindings=true` now logs both `[NHL-TEX]` texture
binding state and `[NHL-SAMPLER]` sampler state. Use it during normal gameplay, then run:

```powershell
python tools\nhl12_texture_proof.py --analyze-log build\nhl12_normalplay_equipment_sampler.log --out build\nhl12_normalplay_equipment_sampler_analysis
```

This checks the exact runtime fetch/sampler pairing for the broken draw class. It specifically guards
against the two known ways to regress this bug: losing real packed mips on equipment material maps, or
keeping stale packed state on larger base-only BC maps. It also reports forced anisotropy on mipped
NHL12 material maps so sampler changes cannot silently undo the pad/glove fix.

Fast offline contract gate before launching the game:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --scan-material-contract-dir goaliepad --scan-material-contract-dir glove --scan-material-contract-dir blocker --scan-material-contract-dir trapper --scan-material-contract-dir jersey --out build\texture_material_contract_equipment_jersey
```

Latest result: 3856 material-map files checked, 0 failures, 0 warnings. This verifies that mipped
colormap/support, shine, normal, and alpha/support maps keep their renderer key/view contract, while
base-only DB diffuse/template recolor masks remain base-only by design.

Default unattended renderer suite:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --nhl12-regression-suite --out build\nhl12_renderer_regression_suite_full
```

Latest result: PASS, 0 failed sections. This wraps the 43-check source contract, 70-case renderer-key
self-test, material contract, DB-colored material-stack previews, full goalie equipment byte-upload
proof, and jersey texlib/colormap samples into one report:
`build\nhl12_renderer_regression_suite_full\nhl12_regression_suite.json`.

The source contract fails if future edits remove the NHL12 app defaults or RexGlue flags that keep
normal-play equipment material maps mipped while preserving cube reflections. That means a local
texture decode pass is no longer enough; the renderer wiring itself must also match the NHL12
contract before anyone launches the game. It also checks that the C++ base-only generated BC branch
still strips stale packed state only for non-tiny 2D DXT1/DXT2/3/DXT4/5 fetches.

Runtime-log analysis also filters expected null signed/unsigned SRV slots. The translated shader
creates both slots, but it samples only the slot required by runtime TextureSign values. After this
filter, the old normal-play diagnostic's remaining errors are base-only BC bindings whose logged
packed state differs from the current NHL12 renderer-key normalization.

The preview section renders 40 representative pad/glove/blocker/trapper stacks at multiple LODs
using decoded diffuse, template, normal, and shine layers. The latest run produced 40 passes and 30
exact DB color matches, with PNGs under
`build\nhl12_renderer_regression_suite_full\material_stack_composites`.

## Permanent Lessons

- `tfetchCube` zeroing is a diagnostic, not a fix.
- Reflection alpha matters in NHL12 materials.
- The blue tint can come from a specular/reflection contribution even when base jersey textures are correct.
- Single-channel cubemap fetches must be sanitized too; otherwise winter-classic and close-up material shaders can bypass the RGB clamp.
- Green/purple/black equipment block noise is not fixed by forcing all DXT/BC textures through the D3D12 decompression path.
- Goalie pad artifacts and skater helmet artifacts can share a visual symptom while using different
  NHL12 material paths.
- Current equipment evidence says to preserve color/shine/normal material maps and their mips, while
  trusting embedded RX2 fetch constants. Do not treat signed DXN/DXT5A as the active explanation
  without new proof.
- DB-color/recolor/template inputs must be preserved; bypassing them can hide the artifact while
  breaking customization.
- Any future shader translator behavior change must bump the shader modification version.
- Gameplay visual confirmation is required; frontend FPS logs cannot prove material correctness.
