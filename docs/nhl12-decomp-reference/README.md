# NHL12 Decomp — Cross-Reference Notes

**Provenance:** these three docs come from the *sibling NHL12 PPC-recompilation
project* and its developer (2026-06-14). They are **reference material, not
authoritative guidance for NHL Legacy.**

## Caveat — read before applying any of this

There are ~4 years of engine and toolchain progression between NHL 12 and
NHL Legacy. Per the NHL12 dev: NHL12's engine is **RenderWare 4**; NHL Legacy is
**likely RenderWare 5**. Treat everything here as *potential insight into EA
Canada's standard development process* — useful for orienting hypotheses — and
**not** as literal answers to issues we hit (e.g. the high-cut jersey-number
problem). Our renderer stack differs fundamentally: NHL12 uses ReXGlue on a
D3D12-ROV texture-cache path; our high cut uses the plume / CP-decode path.

## Contents

| File | What it covers |
|---|---|
| [`jump_tables.md`](jump_tables.md) | EA toolchain jump-table dispatch patterns in `nhlzf.exe` and how their recomp detects them (nop-tolerant structural walk; stock XenonAnalyse misses them). |
| [`asset_formats.md`](asset_formats.md) | NHL12 disc asset formats — BigEB v3 / BIG4 containers, EA-XMA audio, RenderWare-descendant render assets, VP6 cinematics. |
| [`nhl12_renderer_system_notes.md`](nhl12_renderer_system_notes.md) | **Most relevant.** NHL12 ReXGlue renderer findings — ROV path requirement, Release codegen flags, forced-stacked `tfetch3D`, composited equipment materials, cube-reflection sanitizer, mip-window rebase, VP6/MPEG video formats. |

The renderer notes (doc 3) are the ones worth re-reading: the NHL12 dev
independently arrived at the **composited-equipment model** (diffuse + template
`_tm` + special `_sm` + logo + recolor-template + DB per-zone RGB), which echoes
our own jersey-number compositing diagnosis — but their fixes live in a
texture-cache architecture we do not share, so there is no drop-in solution.

See also memory: `nhl12-decomp-cross-reference`.
