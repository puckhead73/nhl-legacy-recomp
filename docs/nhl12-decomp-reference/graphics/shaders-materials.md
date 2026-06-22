# Graphics — Shaders, Materials & Textures

> **Hunting the green/black equipment speckle (goalie pads, helmets, gloves)?** Go
> straight to **[`textures-and-shaders-feeding.md`](textures-and-shaders-feeding.md)** —
> it documents how equipment textures are fed and gives the corrected, evidence-based
> root-cause diagnosis (the bound equipment textures are *runtime-generated*, not the
> on-disk maps) plus the decisive localization experiment.

Structured summary; the deep detail is in
[`../nhl12_renderer_system_notes.md`](../nhl12_renderer_system_notes.md) and
[`../renderer_investigation_blue_texture_bug.md`](../renderer_investigation_blue_texture_bug.md)
`[INV]`.

## 1. Shaders: Xenos → DXBC (CONFIRMED `[INV]`)
The game ships **compiled Xenos shader microcode** (`cache:\shaders\*.fxo`, produced
originally by the XDK `xgraphics\ucode` compiler `[P4]`). At runtime RexGlue's
**`dxbc_translator*.cpp`** translates Xenos shaders to **DXBC** and
`pipeline_cache.cpp` builds/stores D3D12 PSOs.

### Shader storage & the translation key (CONFIRMED, important)
Persistent shader storage lives under `<cache_root>\shaders\shareable`:
- `45410964.xsh` — guest shaders (shared across paths).
- `45410964.rov.d3d12.xpso` / `45410964.rtv.d3d12.xpso` — translated PSOs, split by
  path (headers `DXRO` / `DXRT`), so ROV/RTV PSO files don't cross-contaminate.

The translator is configured per-launch with: adapter vendor id, bindless mode,
**ROV on/off**, gamma RT mode, MSAA support, draw-resolution scale, debug mode.
**Key risk (from `[INV]`):** every visual-affecting mode must be encoded in the shader
**modification key**, or a cached translation can be reused in a state it wasn't built
for → corruption. The app clears `…\shaders` each launch during investigation.

## 2. Materials & the attribute DB (CONFIRMED storage, INFERRED use)
Materials are data-driven via **`cache:\AttribDB\renddb.{bin,vlt}`** `[RT]` — the
**render attribute database** (`.vlt` = EA hashed "vault"). This binds meshes to
shaders, textures, and render states (the RenderWare material concept). Reading actual
material definitions needs the `.vlt` hashing scheme (UNKNOWN; open task — see
[`../assets/asset-pipeline.md`](../assets/asset-pipeline.md)).

## 3. Textures (CONFIRMED path + bugs `[INV]`)
- Guest texture fetch constants → `TextureKey` (`pipeline/texture/cache.cpp`) →
  D3D12 resources + SRVs (`d3d12/texture_cache.cpp`).
- Texture libraries ship as `cache:\rendering\boot\texlib_*.rx2` (RenderWare) `[RT]`.
- **Equipment textures** were a major investigation: the game uses **stacked Texture3D
  / cube fetches with 2D equipment maps**, with composition, mip-window rebasing, and
  DXT1/DXT5 handling that produced the **blue/green corruption** bugs. The known-good
  state is captured in a backup; specific hacks (movie-DXT, disabling MSAA) reintroduce
  blue. Full timeline + the equipment cube/2D binding-mismatch analysis are in the
  investigation docs and
  [`../nhl12_equipment_db_color_notes.md`](../nhl12_equipment_db_color_notes.md).

## 4. The blue & green bugs (status)
| Bug | Cause (as understood) | Status |
|---|---|---|
| Blue equipment | equipment cube/2D fetch + mip/DXT handling; some movie-DXT hacks + MSAA-off reintroduce it | **FIXED** in `WORKING_RENDERER_NEVER_DELETE` backup |
| Green intro video | DXT1 movie texture, GPU-side, video-specific (source data clean) | **OPEN / paused** |

See [`../nhl12_vp6_green_video_fix_plan.md`](../nhl12_vp6_green_video_fix_plan.md),
[`../renderer_investigation_blue_texture_bug.md`](../renderer_investigation_blue_texture_bug.md),
[`../nhl12_renderer_regression_guardrails.md`](../nhl12_renderer_regression_guardrails.md).

## 5. Lighting & post (INFERRED)
Arena lighting (bright, even ice + spotlit crowd), player specular/cloth shading, and
post (color grade, bloom) are INFERRED standard for the genre. The color-grade path
was specifically ruled out as the black-scene cause (it was ROV resolve). Concrete
lighting model UNKNOWN pending shader/material reads.

## Open questions
- `.vlt` vault hashing (to read materials).
- The equipment texture model end-to-end (cube vs 2D binding, intended composition).
- Lighting model and post chain.
- Whether all visual modes are in the shader mod-key.

See [`rendering-pipeline.md`](rendering-pipeline.md) and
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).
