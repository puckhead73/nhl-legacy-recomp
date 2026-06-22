# NHL 12 (Xbox 360) — Technical Documentation

Reverse-engineering documentation for the **NHL 12** (EA Canada, 2011) Xbox 360
executable, produced as part of the `nhl12-recomp` static-recompilation project.

> **Read this first.** It explains *what kind of codebase this is*, *where the
> evidence comes from*, and *how confident each claim is*. Everything else in
> this tree depends on those conventions.

---

## 1. What this documentation covers — and what the "codebase" actually is

This is **not** a clean, symbol-recovered C++ decompile. The artifact under study
is the retail Xbox 360 executable `nhlzf.exe` (`default.xex`, title `0x45410964`,
base `0x82000000`) and a **static recompilation** of it:

- **103,714 functions**, every one named `sub_<hexaddress>` (e.g. `sub_826FBD30`).
  There are **no recovered symbol names** in the translated code — EA's retail
  build is stripped.
- Each function is a **machine translation of PowerPC machine code into C++** that
  operates on a `PPCContext` register file plus a flat guest-memory `base` pointer.
  A representative line looks like:
  ```cpp
  // lhz r11,16(r4)
  ctx.r11.u64 = REX_LOAD_U16(ctx.r4.u32 + 16);
  ```
  The original PPC mnemonic is preserved as a comment above each statement. This is
  produced by **XenonRecomp** (Phase 2) and **RexGlue's codegen** (Phase 4); it is
  *behaviourally* the game, but it is not source you can read for intent.

So we cannot "open `Goalie.cpp` and read it." Instead, this documentation
reconstructs how NHL 12 works from the evidence that *did* survive in the binary
and from the runtime behaviour of the recompiled build. See §3.

For the *recompilation project itself* (how the EXE is built, runtime glue, GPU
strategy, current blockers) the canonical living documents are:
- [`PLAN.md`](../PLAN.md) — the phased port plan.
- [`docs/HANDOFF.md`](HANDOFF.md) — current build/run state (Phase 4).
- The `recompilation/` section of this tree distils the durable parts.

## 2. The game at a glance (confirmed)

| Property | Value | Evidence |
|---|---|---|
| Title | NHL 12 | disc / title ID `0x45410964` |
| Developer | EA Canada | P4 paths, `RENDERWARE` banner |
| Year | 2011 | XDK `xdk-main-feb11` |
| Executable | `nhlzf.exe` (27.3 MB image) | XEX header |
| **Engine** | **EA RenderWare 4** (`rw::core`, `rw::math`, `rw::audio::core`, `rw::movie`) | RTTI/assert strings |
| Shared tech | EA "basekit": MemoryFramework, OSDK, BlazeSDK, EAStore, AdManager, RWAudioCore | P4 paths |
| Middleware | Lynx 1.7.1 (particles + parameter/tuning), Speex, EASTL | P4 paths, RTTI |
| Game packages | `nhl` (cmn), `nhlfrontend`, `nhlgameplay`, `nhlrender` | P4 paths |
| Imports | 317 unique (200 `xboxkrnl` + 117 `xam`) | import table |

The reconstructed module tree is in **[`maps/codebase-map.md`](maps/codebase-map.md)** —
start there; it is the single most useful artifact in this set.

## 3. Evidence sources (and how to read citations)

Every non-obvious claim cites one of these sources. When you see a tag like
`[STR]` or `[P4]`, that is the provenance.

| Tag | Source | Reliability |
|---|---|---|
| `[P4]` | **Perforce source paths** left in retail assert strings (e.g. `…\nhlgameplay\dev\source\nhlgameplay\ai\goalie\goaliesave.cpp`). These name real original source files. | **High** — these are literal original filenames. |
| `[RTTI]` | C++ RTTI / namespace strings (`rw::audio::core::Voice::IsExpelled`, `eastl::hash_map`). | **High** — literal original symbol fragments. |
| `[STR]` | Other ASCII strings in the image (format strings, asset paths, enum-name tables, error text). | Medium–High — intent inferred from context. |
| `[IMP]` | Kernel/XAM import table (`docs/kernel_imports.csv`). | **High** — exact platform API surface. |
| `[RT]` | Observed runtime behaviour of the recompiled build (thread names, log lines, crash sites, VFS open calls). | **High** for *what happens*, lower for *why*. |
| `[ASM]` | Direct reading of recompiled C++ / PPC disassembly at a cited address. | **High** but expensive; used sparingly. |
| `[INV]` | Prior investigation docs in `docs/` (renderer bug hunts, etc.). | As noted in those docs. |
| `[INF]` | **Inference** from the above + domain knowledge of EA Sports / RenderWare titles. | **Low–Medium — explicitly a hypothesis.** |

### Confidence markers used throughout
- **CONFIRMED** — directly supported by `[P4]`/`[RTTI]`/`[IMP]`/`[ASM]`/`[RT]`.
- **INFERRED** — reasoned from evidence; plausible but unproven. Always paired with the evidence it rests on.
- **UNKNOWN** — we know the thing exists but lack evidence for its detail; the doc says what evidence would resolve it.

If a statement has no marker, it is either trivially CONFIRMED (e.g. a file path
that exists in the repo) or it is project-mechanical fact from `PLAN.md`/`HANDOFF.md`.

## 4. Documentation map & status

> Status legend: ✅ complete given current evidence · 🟡 written but inherently
> limited by RE not yet done (the doc says exactly what's missing) · ⬜ planned.
> **Note:** "complete given evidence" still means *evidence-bounded* — a ✅ doc
> documents everything the binary/runtime currently supports, and flags inferences.

| Area | File | Status |
|---|---|---|
| **Index / framing** | [`README.md`](README.md) | ✅ |
| **Codebase map** | [`maps/codebase-map.md`](maps/codebase-map.md) | ✅ |
| Function index (how to navigate 103k funcs) | [`maps/function-index.md`](maps/function-index.md) | ✅ |
| Global state | [`maps/global-state.md`](maps/global-state.md) | ✅ |
| Glossary | [`maps/glossary.md`](maps/glossary.md) | ✅ |
| **Architecture** overview | [`architecture/overview.md`](architecture/overview.md) | ✅ |
| Main loop | [`architecture/main-loop.md`](architecture/main-loop.md) | 🟡 |
| Memory | [`architecture/memory.md`](architecture/memory.md) | 🟡 |
| Threading | [`architecture/threading.md`](architecture/threading.md) | 🟡 |
| **Recompilation** notes | [`recompilation/xenondecomp-notes.md`](recompilation/xenondecomp-notes.md) | ✅ |
| RexGlue runtime | [`recompilation/rexglue-runtime.md`](recompilation/rexglue-runtime.md) | ✅ |
| **Assets** pipeline | [`assets/asset-pipeline.md`](assets/asset-pipeline.md) | ✅ |
| Save / load | [`save-load/serialization.md`](save-load/serialization.md) | 🟡 |
| **Gameplay** core | [`gameplay/core-systems.md`](gameplay/core-systems.md) | ✅ |
| Skating / choreo | [`gameplay/skating.md`](gameplay/skating.md) | ✅ |
| Puck physics | [`gameplay/puck-physics.md`](gameplay/puck-physics.md) | ✅ |
| Goalies | [`gameplay/goalies.md`](gameplay/goalies.md) | ✅ |
| AI | [`gameplay/ai.md`](gameplay/ai.md) | ✅ |
| Rules / officials | [`gameplay/rules.md`](gameplay/rules.md) | ✅ |
| **Animation** | [`animation/animation-system.md`](animation/animation-system.md) | ✅ |
| **Physics** collision | [`physics/collision.md`](physics/collision.md) | ✅ |
| **Graphics** pipeline | [`graphics/rendering-pipeline.md`](graphics/rendering-pipeline.md) | ✅ |
| Shaders / materials | [`graphics/shaders-materials.md`](graphics/shaders-materials.md) | ✅ |
| **Textures/shaders feeding + equipment-corruption root cause** | [`graphics/textures-and-shaders-feeding.md`](graphics/textures-and-shaders-feeding.md) | ✅ |
| **RE: equipment color-zone recolor material (pinned functions)** | [`re/equipment-recolor/README.md`](re/equipment-recolor/README.md) | ✅ |
| Cameras | [`graphics/cameras.md`](graphics/cameras.md) | 🟡 |
| **UI** / frontend | [`ui/frontend.md`](ui/frontend.md) | ✅ |
| **Audio** | [`audio/audio-system.md`](audio/audio-system.md) | ✅ |
| **Quirks** gotchas | [`quirks/gotchas.md`](quirks/gotchas.md) | ✅ |
| Magic values | [`quirks/magic-values.md`](quirks/magic-values.md) | ✅ |
| **Open questions** | [`unknowns/open-questions.md`](unknowns/open-questions.md) | ✅ |

### Existing investigation docs (folded in by reference, not duplicated)
The renderer was investigated in depth during Phase 4 bring-up. Those notes remain
authoritative for graphics; the `graphics/` docs summarise and cross-reference them:
- [`nhl12_renderer_system_notes.md`](nhl12_renderer_system_notes.md)
- [`renderer_investigation_blue_texture_bug.md`](renderer_investigation_blue_texture_bug.md)
- [`nhl12_renderer_regression_guardrails.md`](nhl12_renderer_regression_guardrails.md)
- [`nhl12_vp6_green_video_fix_plan.md`](nhl12_vp6_green_video_fix_plan.md)
- [`nhl12_equipment_db_color_notes.md`](nhl12_equipment_db_color_notes.md)
- [`nhl12_crash_debugging.md`](nhl12_crash_debugging.md)

## 5. How to use these docs

- **Reverse-engineering a subsystem?** Start at [`maps/codebase-map.md`](maps/codebase-map.md),
  find the package/module, then the per-subsystem doc. Each subsystem doc lists the
  `[P4]` source files that compose it and any addresses pinned so far.
- **Working on the recomp/runtime?** `recompilation/` + `HANDOFF.md`.
- **Modding / asset work?** `assets/asset-pipeline.md`.
- **Adding a claim?** Cite a source tag from §3 and mark confidence. Prefer
  incremental edits to the right subsystem doc over a new file.

---
*Generated by reverse-engineering the retail binary. No EA code or assets are
reproduced here — only names, offsets, and structure recovered for interoperability
and documentation purposes.*
