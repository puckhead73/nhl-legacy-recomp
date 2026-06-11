# Codex / Claude coordination handoff

> Shared coordination ledger for development performed by Codex and Claude Code.
> Read this file before starting work and update it before ending a work session.
> Last updated: 2026-06-10 by Claude.

---

## Purpose

This document is the durable intermediary between the two development agents. It records changes that
have actually been made, the evidence used to validate them, and the exact state in which work was
left. It complements task-specific briefs such as `faithful-edram-fold-handoff.md`; it does not replace
them.

The repository is a plain working tree with no Git history to recover from. Treat existing edits as
intentional unless this ledger or direct inspection proves otherwise. Never overwrite or revert the
other agent's work merely because it is unfamiliar.

## Coordination protocol

Before beginning a session:

1. Read this document, `codex-onboarding.md`, and the active task handoff.
2. Inspect the current source and generated artifacts; file/line references in docs are only hints.
3. Check the **Active ownership** section to avoid simultaneous edits to the same subsystem.
4. Add or update an ownership entry before making substantial changes.

Before ending a session:

1. Update **Current verified state** if the project baseline changed.
2. Record every source/doc file changed and summarize the behavioral effect.
3. Record the exact build and validation commands run, including failures.
4. Separate proven facts from hypotheses and list the next concrete action.
5. Add a dated entry to **Session log** and release or transfer ownership.

Use agent names `Codex` and `Claude`. Keep entries concise, factual, and append-only where practical.
When correcting an old entry, mark it superseded rather than silently deleting its history.

## Active task

**Task:** Faithful EDRAM fold for the beta owned GPU backend.

**Goal:** Make beta's own EDRAM render and resolve path reconstruct the scene_02 player at the oracle
position without `NHL_BETA_SKIP_RESOLVE`, while preserving the default path, 2D parity, depth,
texture injection, and the shipped offscreen fallback.

**Primary brief:** `docs/faithful-edram-fold-handoff.md`

## Active ownership

| Agent | Area | Status | Since | Notes |
|---|---|---|---|---|
| Codex | Faithful EDRAM fold investigation and beta cache-driving changes | Complete (POSITION only — see correction) | 2026-06-09 | ROV path centers the player and populates the pass-B color resolve; remaining green material output is the separate scene_02 texture-injection gap. **CORRECTION (2026-06-10, Claude audit B6):** "Complete" applies to player POSITION only. The Codex 2026-06-09 "Proven: faithful EDRAM fold/addressing objective is complete" row is **SUPERSEDED** — a regional green COLOR defect at the x=640 fold line remains open (oracle-confirmed; see the 2026-06-10 Claude entries and `rov-green-player-is-fold-color`), and the active brief `faithful-edram-fold-handoff.md` still treats the fold as the open hard task. Do not read this row as "fold fully solved." |
| Claude | ROV/EDRAM fold COLOR resolve (taking over beta resolve-driving for the green-band fix) | Active | 2026-06-10 | Proved the "green player" is a regional GREEN-ADDITIVE EDRAM-fold defect at the x=640 fold line, oracle-confirmed — NOT a missing-material gap. Now taking over the EDRAM resolve area (overlaps Codex's prior ROV work — preserving it) to locate and fix the green source. See 2026-06-10 Claude session entries. |

If an agent needs to touch an area owned by the other, first read that agent's latest session entry and
preserve its changes. Explicitly record overlapping edits.

## Current verified state

Baseline inherited from the onboarding and faithful-fold handoff; Codex has not independently rerun
these validations yet:

- The offscreen beta takeover path renders the complete textured scene_02 player, with the known
  far-right/wrap positional artifact.
- The faithful EDRAM path's pass-B resolves at `0x1A7ED000` and `0x1AF1D000` are currently black or
  comb-like rather than containing the unfolded player.
- Skipping resolves 15 and 16 allows trace bytes to reach the downstream composite and places the
  player correctly, localizing the remaining defect to beta's EDRAM render/resolve driving.
- The default/live path and beta-without-takeover are required to remain byte-identical.
- No source changes have been made by Codex as of this document's creation.

Codex verification/update on 2026-06-09:

- The inherited RTV failure was reproduced in `nhllegacy_387.log`: resolves 15/16 succeed as API
  calls, but `gpudump_1A7ED000.png` is effectively black and the final frame has no player.
- The offscreen fallback was independently reproduced after the new code was built. It still renders
  the fully textured player with the known far-right plus left-wrap artifact.
- An opt-in beta ROV path now writes directly to the SDK EDRAM UAV. Its first complete-state
  iteration produces a full player at the correct centered oracle position in the final composite.
  `gpudump_1A7ED000.png` contains the unfolded player rather than black.
- The faithful fold/addressing problem is therefore proven solved by the ROV architecture, but parity
  is not complete: the player is green, the UI/background colors are corrupted, and some textures
  are missing. The remaining work is ROV system-constant / texture-color contract completion.
- `NHL_BETA_RT_PATH=rov` is required for the experiment. With the variable absent, beta continues to
  construct the previous RTV cache.
- Resolve isolation now proves resolve 15 is depth and visually irrelevant, while resolve 16 is the
  pass-B color resolve that exposes beta's centered player. Skipping only 15 is byte-identical to the
  normal ROV result; skipping only 16 is byte-identical to skipping both and restores the trace's
  preloaded far-right textured player.
- The original faithful-fold brief defines the canonical replay oracle as a centered black
  silhouette because scene_02's player material textures were resident before capture and are absent
  from guest RAM. Beta's player draws confirm those material fetch pages are zero, and scene_02 has
  no `.inject.json` sidecar. The remaining green-vs-black material output is therefore outside the
  fold/addressing task.

## Change ledger

Add newest entries first. List only files actually changed.

| Date | Agent | Files | Summary | Validation |
|---|---|---|---|---|
| 2026-06-10 | Claude | `renderer/core/nhl_command_processor.cpp`, `src/loose_tree_device.cpp` | **Audit-fix Phase 2** (fragile-coupling & dump-oracle hardening). B2: `this+0xCE8` view-heap read now via `ReadCpViewHeapChecked()` — SEH-guarded + descriptor-validated, returns nullptr (caller skips sampler table) instead of binding a garbage heap if the SDK layout shifts. B11: hoisted register-index arrays into `namespace beta_reg` (one def, de-duped the 3 `kColorInfoRegisters` copies) with a `static_assert` tripwire vs the exported `RB_COLOR_INFO::register_index`. B5: marked `NHL_BETA_ZFAR` non-faithful in-code (contiguous-tile + 0xFF-fill are wrong for folded/MSAA D24FS8). B4/B12: corrected the false "matches xenos::TiledAddress" claim on both untile dumpers (row-major ≠ SDK GetTiledOffset2D interleave) and labeled the BGRA-assumed PNGs — comments only, untile math unchanged pending a known-surface A/B. M3: `RecordBetaGpuDumps` no longer clears an unconsumed buffer batch (skips instead). M6: `HostFile::ReadSync` serialized with a per-file mutex. | Build PASS (static_assert tripwire compiled; SEH guard OK). scene_00+scene_03 still **byte-identical** vs Phase-1 before-baseline. Green-band rov repro: **SHA256-identical to the Phase-1 rov result**, B2 heap validated (no WARN), no crash. Logs 444-446. |
| 2026-06-10 | Claude | `renderer/core/nhl_command_processor.{cpp,h}`, `src/injection_registry.cpp`, `src/loose_tree_device.cpp`, `src/union_device.{cpp,h}`, `src/nhllegacy_app.h`, `src/diag_hooks.cpp` | **Audit-fix Phase 1** (correctness + quick wins; plan `~/.claude/plans/build-out-a-plan-eventual-mountain.md`). B1: `NHL_BETA_NOBLEND` now uses correct blend regs (0x2201/0x2209/0x220A/0x220B) at save+restore. B3: `RecordBetaGpuDumps` restores shmem `UseForReading()`. B10: resolve depth-source (copy_src_select>=4) flagged not clamp-mislabeled (diag). R1: **retired `NHL_BETA_DRAWPROBE`** (default-path device-loss risk) incl. `RunBetaDrawProbe`/`RecordBetaClearTest` + members. M5: clear `resolve_dest_bases_` per-frame in IssueSwap. L5: removed dead `beta_srv_heap_/view_bump_/sampler_bump_`. L3: depth-stats honest re D24FS8. B7/B8: object-bounded sidecar parser (no addr↔rx2 mis-pair on malformed input). L7: collision warn + 64-slot comment. L8: case-insensitive `.rx2` matcher. B13: `registered_` only on success. B14: "mounted" log gated on `lower_active()`. L9: file_size ec + stoul comment. L9-app: replay exit code reflects capture success. L10: diag_hooks addr comment reconciled. | Build PASS (8/8, only pre-existing getenv/fopen warnings). scene_00 + scene_03 default-path **SHA256 byte-identical** before/after (8D91C6FF.. / A7627832..). Green-band rov repro unperturbed (clean PNG, no crash). B7/B8: well-formed→39, malformed(rx2 dropped)→38 (skipped not mis-paired, no 0x17E11000 leak). B1 NOBLEND run: no crash. Logs 435-443. |
| 2026-06-10 | Claude | `docs/codebase-audit-findings.md` (new), `docs/agent-coordination-handoff.md`, memory `tier1-backend-architecture.md` | Audit pass per `docs/codebase-audit-prompt.md`: 30+ prioritized findings (bugs/tech-debt/red-flags/inaccurate-judgments), `NHL_BETA_*` keep/retire/consolidate inventory, documented-judgment audit, quick-wins. AUDIT ONLY — no source edits. Recorded two corrections (B6 fold "Complete"→superseded note below; B15 `NHL_BETA_TEX`→`NHL_BETA_NOTEX` stale flag). | Read-only; independently re-verified B1 (`0x2201+i` blend reg bug, cpp:1171), B2 (`this+0xCE8` raw read, cpp:1994), B3 (shmem not restored, cpp:2575-2605) against current source. No build. |
| 2026-06-10 | Claude | `renderer/core/nhl_command_processor.cpp` | Added `NHL_BETA_SKIP_RANGE=lo-hi` (skip draw geometry in a range) and an EDRAM color/depth base+pitch probe (gated by `NHL_BETA_EDRAM_DIAG`). Diag-only. | Rebuild PASS. Range-skips byte-identical (green draw-invariant); `nhllegacy_433.log` gives composite bases color0=0 depth=736 pitch=8t surf=640 msaa=1. |
| 2026-06-10 | Claude | `renderer/core/nhl_command_processor.cpp` | Added `NHL_BETA_FORCE_SAMPLE=N` diag: overrides `copy_sample_select` (RB_COPY_CONTROL 0x2318 bits[4:6]) on 1280-wide resolves, restored immediately after. Diag-only, env-gated. | Rebuild PASS. Sweep `claude_rov_sample0..3.png`: green identical across all samples -> not resolve MSAA averaging. |
| 2026-06-10 | Claude | `renderer/core/nhl_command_processor.cpp` | Added a green-band probe to the `NHL_BETA_RESOLVE_DIAG` block: logs `RB_COLOR_CLEAR`/`_LO` (regs 0x231E/0x231F) and the rt0 EDRAM base/format per resolve. Diag-only, behind the existing env gate. | Incremental rebuild PASS (vcvars64+LLVM, 1 obj + link). `nhllegacy_422.log`: RB_COLOR_CLEAR=0 for ALL resolves -> guest clears to BLACK; green is NOT a clear color. |
| 2026-06-10 | Claude | `docs/agent-coordination-handoff.md` | Recorded the green-player re-diagnosis: it is a regional EDRAM-fold COLOR defect at x=640 (oracle-confirmed), not missing materials. Claimed ownership. | Pixel diffs + oracle ground-truth + reseed/blend rule-outs; reproductions via `run_edram.ps1`. |
| 2026-06-09 | Codex | `renderer/core/nhl_command_processor.cpp` | Added resolve source/command/destination-format diagnostics and removed the redundant post-resolve shared-memory invalidation. | Build passed; skip-15/skip-16 bisection localized the visible player replacement to color resolve 16. |
| 2026-06-09 | Codex | `renderer/core/nhl_command_processor.cpp` | Completed the accessible ROV constant block, corrected split EDRAM bases and primitive index state, matched the bindful EDRAM descriptor layout, and added ROV diagnostics. | Build passed; scene_02 UI/equipment textures recovered while the player remained centered. |
| 2026-06-09 | Codex | `renderer/core/nhl_command_processor.cpp` | Added opt-in beta ROV cache construction, ROV pipeline/bound-target handling, EDRAM UAV descriptors, and scene-relevant ROV system constants. | Build passed; scene_02 ROV renders the player centered and populates resolve 15; offscreen fallback still renders. |
| 2026-06-09 | Codex | `docs/agent-coordination-handoff.md` | Recorded faithful-fold investigation, implementation, evidence, and next work. | Readback review. |
| 2026-06-09 | Codex | `docs/agent-coordination-handoff.md` | Created the shared agent coordination ledger. | Document reviewed for required handoff fields; no build needed. |

## Validation ledger

Record commands and outcomes precisely. Do not convert an inherited claim into a locally verified
claim without rerunning it.

| Date | Agent | Command / artifact | Result |
|---|---|---|---|
| 2026-06-10 | Claude | Pixel diff `codex_rov_skip16.png` vs `codex_offscreen_baseline.png` (player region, 10753 samples) | 87.4% of player-content pixels differ by EXACTLY delta (R,G,B)=(0,+127,+15); red channel byte-identical everywhere. Materials ARE sampled (red preserved, full texture detail); green is a constant additive, not a flat fallback. |
| 2026-06-10 | Claude | Oracle ground-truth `oracle_editplayer_f30.png` bg/UI sample | Real console = blue bg (10,9,225) + blue UI, matching `codex_offscreen_baseline.png`, NOT the ROV result. Confirms (0,127,15) is corruption introduced by the ROV EDRAM round-trip; offscreen RTV path is correct. |
| 2026-06-10 | Claude | `run_edram.ps1 -Scene scene_02 -Frame 30 -Edram 1 -Extra @{NHL_BETA_RT_PATH='rov'}` (`claude_rov_baseline.png`, `nhllegacy_417.log`) | Live reproduction of centered-player ROV result with cyan UI / green player. |
| 2026-06-10 | Claude | Same + `NHL_BETA_NO_RESEED=1` (`claude_rov_noreseed.png`, `nhllegacy_418.log`) | Byte-identical to baseline; EDRAM snapshot reseed RULED OUT as the green source. |
| 2026-06-10 | Claude | `codex_rov_noblend.png` jersey-point check | Jersey points still read (0,127,15); ROV blend constant (regs 0x2105-0x2108) RULED OUT as the green source. |
| 2026-06-10 | Claude | Oracle→ROV frame-wide color map + spatial bbox (`claude_rov_corruption_map.png`) | Corruption is REGIONAL, split at exactly x=640 (the 640-pitch fold line): left band [0,639] corrupt, [640,843] clean, player band [844,1016] corrupt, [1017+] clean. Additive green (+30 over dark panel, +127 over black); red/white preserved. |
| 2026-06-09 | Codex | `NHL_BETA_SKIP_RESOLVE=15` (`codex_rov_skip15.png`, `nhllegacy_404.log`) | Byte-identical to normal ROV output; resolve 15 is depth and does not cause the green player. |
| 2026-06-09 | Codex | `NHL_BETA_SKIP_RESOLVE=16` (`codex_rov_skip16.png`, `nhllegacy_405.log`) | Byte-identical to skipping 15+16; resolve 16 alone replaces trace-preloaded material output with beta's centered green player. |
| 2026-06-09 | Codex | Resolve state diagnostics | Resolve 15: source 4, raw depth copy. Resolve 16: source color 0, convert, `k_8_8_8_8`, endian 2, red/blue swap, color clear. |
| 2026-06-09 | Codex | ROV texture-ordering and descriptor corrections (`nhllegacy_392.log`, `nhllegacy_393.log`) | Byte-identical to iteration 1; both hypotheses ruled out. |
| 2026-06-09 | Codex | ROV no-texture control (`codex_rov_notex.png`) | Completely blue frame; proves texture tables affect ROV output. |
| 2026-06-09 | Codex | ROV no-blend control (`codex_rov_noblend.png`) | Large image change; proves shader-implemented ROV blending is active. Diagnostic only, not a fix. |
| 2026-06-09 | Codex | Full accessible ROV constants (`codex_rov_iteration4.png`, `nhllegacy_396.log`) | Improved: equipment textures/footer restored and player stays centered. Player remains uniformly green. |
| 2026-06-09 | Codex | Zero texture-sign control (`codex_rov_zero_signs.png`, `nhllegacy_398.log`) | Player disappears; signed fetch slot 16 is required and must not be zeroed. |
| 2026-06-09 | Codex | scene_00/scene_03 frame-30 captures (`nhllegacy_400.log`, `nhllegacy_401.log`) | Inconclusive: these are not the validated canonical reference frames and must not be treated as parity results. |
| 2026-06-09 | Codex | Forced object rebuild plus `cmake --build ... --target nhllegacy` | PASS; command processor compiled and executable linked. |
| 2026-06-09 | Codex | `run_edram.ps1 -Scene scene_02 -Frame 30 -Depth 1 -Edram 0` | PASS; `codex_offscreen_baseline.png` shows the textured player and unchanged split/wrap artifact. |
| 2026-06-09 | Codex | RTV EDRAM replay with diagnostics and GPU dumps | Reproduced failure; `nhllegacy_387.log`, final player absent, pass-B dump black. |
| 2026-06-09 | Codex | ROV replay with `NHL_BETA_RT_PATH=rov` | Device healthy; first partial contract wrote non-black EDRAM but was badly corrupted (`nhllegacy_388.log`). |
| 2026-06-09 | Codex | ROV replay plus `NHL_BETA_DEPTH_FORCE_ALWAYS=1` | Identical corruption; depth rejection ruled out (`nhllegacy_389.log`). |
| 2026-06-09 | Codex | Updated ROV replay with texture signs/depth flags | Major PASS for fold: centered full player in `codex_rov_iteration1.png`; unfolded player visible in `gpudump_1A7ED000.png` (`nhllegacy_391.log`). Color/UI parity still FAIL. |
| 2026-06-09 | Codex | Documentation-only setup | No build or render run yet. |

## Open questions and hypotheses

- ROV is now the selected faithful-fold architecture and has proven correct player positioning.
- The previously corrupted UI was primarily caused by incomplete ROV system constants. Completing
  the accessible Xenia contract restored equipment textures and footer text.
- The remaining green player is narrower than a general ROV color failure. Player draws use many
  GPU-resident textures whose sampled guest pages are zero, while UI textures have populated guest
  RAM. The offscreen path proves the beta texture cache can still render those assets.
- Moving texture-sign calculation after `RequestTextures` and correcting the EDRAM descriptor table
  produced byte-identical frames, ruling those out.
- The only nonzero sign entry in player passes is `0x55` for fetch slot 16 (signed RGBA, used by the
  vertex stage). Zeroing signs removes the player entirely, so that sign is required rather than the
  source of its green color.
- Superseded: downstream resolve interpretation is not the green source. Resolve 16 faithfully
  exposes the already-green ROV color surface; the material inputs for the player pass are absent
  from the trace.
- Menu/intro byte-identity has not yet been rerun after this change. The code is opt-in behind
  `NHL_BETA_RT_PATH=rov`, and the normal scene_02 offscreen fallback was verified.

## Next concrete actions

> SUPERSEDED (2026-06-10, Claude): items 1-2 below (the scene_02 texture-injection hunt) are no
> longer the lead. The "green player" was proven to be a regional EDRAM-fold COLOR defect at the
> x=640 fold line (oracle-confirmed; materials are present and sampled — red channel byte-identical
> to the correct offscreen render). Fix the fold color resolve FIRST; only then re-evaluate whether
> any material gap remains. See the 2026-06-10 Claude session entry.

1. (PRIMARY) Run the direct EDRAM compute-readback instrument to settle render-side vs resolve-side:
   see `docs/edram-readback-instrument-prompt.md` (self-contained task prompt). It dumps the composite
   COLOR surface (tiles [0,360), base 0, pitch 8, 640x720) and DEPTH surface (tiles [736,1096)) in
   their native 640-pitch EDRAM tile layout — if the native color surface is clean, the green is the
   resolve's pitch<width UNFOLD; if it already contains green, it's render-side. Regression signal:
   red channel identical to `codex_offscreen_baseline.png`.
2. (DEFERRED) Only after the fold color is correct, re-check whether scene_02 still shows any
   missing-material areas; if so, resume the `NHL_INJECT_CORRELATE=1` sidecar work. Do not add a
   green-to-black render hack.
3. Validate scene_00 and scene_03 using their actual canonical frames and compare against freshly
   generated base-path oracles with `NHL_BETA_RT_PATH` absent.
4. Once the fold color and canonical regressions pass, decide whether ROV becomes the default EDRAM
   path or remains explicitly selected.

## Session log

### 2026-06-09 - Codex - Coordination setup

**Objective:** Establish a shared handoff mechanism before development begins.

**Work completed:**

- Reviewed `docs/codex-onboarding.md` and `docs/faithful-edram-fold-handoff.md`.
- Created this coordination ledger with ownership, change, validation, hypothesis, and next-action
  sections.

**Files changed:**

- `docs/agent-coordination-handoff.md` - new file.

**Validation:** Documentation-only change; no build or GPU replay was run.

**State left for next agent:** No implementation files have been changed. Codex is ready to accept the
first development prompt.

### 2026-06-09 - Codex - ROV faithful-fold proof

**Objective:** Make beta's own EDRAM path produce the scene_02 folded player surface at the oracle
position without injected resolve bytes.

**Work completed:**

- Reproduced the normal offscreen render and the broken RTV EDRAM resolve.
- Compared ReX headers with upstream Xenia's D3D12 `UpdateSystemConstantValues` and binding logic.
- Added `NHL_BETA_RT_PATH=rov` as an opt-in cache-construction selector. The default remains `rtv`.
- For ROV draws, configured zero host render-target bits, populated EDRAM pitch/base/color/depth
  constants, wrote shared-memory/EDRAM UAV descriptors, and supplied texture signs plus relevant
  flags.
- Worked around non-exported SDK `rt_register_indices` statics with local documented register arrays.

**Files changed:**

- `renderer/core/nhl_command_processor.cpp` - opt-in ROV construction and draw contract.
- `docs/agent-coordination-handoff.md` - this record.

**Validation:**

- Forced rebuild compiled `nhl_command_processor.cpp.obj` and linked `nhllegacy.exe`.
- `nhllegacy_387.log`: reproduced RTV resolve 15/16 black-player failure.
- `codex_offscreen_baseline.png` / `nhllegacy_390.log`: normal fallback remains fully textured with
  only the known split/wrap position artifact.
- `nhllegacy_389.log`: forcing ROV depth to ALWAYS did not change output, ruling out depth rejection.
- `codex_rov_iteration1.png` / `nhllegacy_391.log`: beta ROV renders a full centered player at the
  oracle position. Device remains healthy.
- Fresh `gpudump_1A7ED000.png`: contains the unfolded player instead of a black surface.

**Proven:**

- Direct ROV EDRAM addressing faithfully handles the non-affine 640-pitch fold.
- The downstream resolve/composite consumes beta's own ROV-written surface and places the player
  correctly.
- The shipped offscreen fallback remains functional because the new path is opt-in.

**Hypotheses / unresolved:**

- Green player, cyan background, striped regions, and missing equipment textures are caused by the
  still-partial ROV system-constant / texture binding contract.
- The fold itself should no longer be debugged via viewport placement hacks unless new evidence
  contradicts the centered ROV result.

**State left for next agent:**

- Continue with `NHL_BETA_RT_PATH=rov`.
- Preserve `codex_rov_iteration1.png`, `gpudump_1A7ED000.png`, and `nhllegacy_391.log` as the current
  proof artifacts.
- Port the remaining upstream ROV constant/binding behavior, then rerun scene_00/scene_03 regression
  checks before enabling ROV more broadly.

### 2026-06-09 - Codex - ROV contract completion and green-player isolation

**Objective:** Remove the broad ROV color/UI corruption while preserving the centered faithful fold.

**Work completed:**

- Moved texture-sign finalization after `RequestTextures`; result was byte-identical, ruling out the
  ordering hypothesis.
- Matched the bindful shared-memory/EDRAM descriptor layout used by Xenia; result was byte-identical.
- Ported the remaining accessible system constants: tessellation, line-loop/index endian, user clip
  planes, point parameters, full 12-bit EDRAM color/depth bases, polygon offsets, and stencil state.
- Added ROV-aware no-blend and texture-sign diagnostics.

**Files changed:**

- `renderer/core/nhl_command_processor.cpp` - ROV constants and diagnostics.
- `docs/agent-coordination-handoff.md` - this record.

**Validation:**

- All forced rebuilds compiled the command processor and linked successfully.
- `codex_rov_iteration4.png`: equipment thumbnails and footer text render again; the full player
  remains centered at the oracle position.
- `codex_rov_notex.png`: all textured content disappears, proving texture tables are active.
- `codex_rov_noblend.png`: substantial change, proving ROV blend constants are active.
- `codex_rov_zero_signs.png`: player disappears. Normal logs show only fetch slot 16 has signed
  metadata (`0x55`), and that metadata is required.

**Proven:**

- The broad UI corruption was caused by incomplete ROV constants and is substantially fixed.
- Texture table binding, ROV blending, and signed texture metadata all affect output.
- The remaining green player is not caused by the general texture-sign ordering or descriptor-table
  hypotheses tested here.

**Hypotheses / unresolved:**

- The green player likely originates after the folded surface is resolved, when the downstream
  composite recreates/samples destinations `0x1A7ED000` and `0x1AF1D000`.
- Resolve invalidation or destination format/swizzle may be stale or mismatched for the beta texture
  cache.
- The attempted scene_00/scene_03 frame-30 captures were not canonical and are inconclusive.

**State left for next agent:**

- Use `codex_rov_iteration4.png` as the current best ROV result.
- Keep normal texture signs; do not adopt `NHL_BETA_ROV_ZERO_SIGNS`.
- Instrument the composite fetch of resolve destinations and the texture-cache SRV key transition
  around resolves 15/16.

### 2026-06-09 - Codex - Resolve invalidation correction and pass-boundary isolation

**Objective:** Verify the resolve-to-texture-cache transition and localize the remaining green
player to a specific resolve boundary.

**Work completed:**

- Verified against upstream Xenia that `D3D12RenderTargetCache::Resolve` calls
  `TextureCache::MarkRangeAsResolved`, which in turn notifies shared memory.
- Removed the beta path's redundant post-resolve `RangeWrittenByGpu` call.
- Correlated resolve destinations with downstream texture fetches. Draws 381/382 sample resolve 14
  at `0x1AF09000`; resolves 15/16 are not sampled directly, and resolve 17 later overwrites the
  same `0x1AF09000` destination.
- Ran the existing resolve-skip probe for resolves 15 and 16 together.

**Files changed:**

- `renderer/core/nhl_command_processor.cpp` - removed duplicate shared-memory invalidation after
  the SDK resolve.
- `docs/agent-coordination-handoff.md` - this record.

**Validation:**

- Forced command-processor rebuild and link: PASS.
- `codex_rov_iteration5.png` / `nhllegacy_402.log`: byte-identical to iteration 4
  (`SHA256 9859D8557080EAA5037795...`), proving duplicate invalidation was not the green-player
  cause.
- `codex_rov_skip15_16.png` / `nhllegacy_403.log`: different output
  (`SHA256 B827968C39FDA30D0C2A26...`). The player regains detailed uniform/face/equipment textures,
  but returns to the far-right placement.

**Proven:**

- The beta resolve path was invalidating resolved ranges twice; removing the duplicate is correct
  but visually neutral for this frame.
- Resolves 15/16 are the exact transition where faithful centered placement is gained and the
  player's detailed color is replaced by uniform green.
- The remaining issue is not a generic stale downstream SRV for destinations 15/16, since those
  destinations are not directly sampled by the logged composite draws.

**Hypotheses / unresolved:**

- One of resolves 15/16 likely carries the positioned color result and the other a mask/depth-like
  result; testing them together cannot identify which output is being interpreted incorrectly.
- Resolve copy source selection, destination format, endian, or channel mask at pass 15/16 is now
  the highest-value inspection point.

**State left for next agent:**

- Preserve `codex_rov_iteration5.png`, `codex_rov_skip15_16.png`, `nhllegacy_402.log`, and
  `nhllegacy_403.log`.
- Add copy-control/source/destination-format logging for resolves 15/16.
- Run `NHL_BETA_SKIP_RESOLVE=15` and `NHL_BETA_SKIP_RESOLVE=16` separately, then correct only the
  resolve whose output destroys color while retaining centered placement.

### 2026-06-09 - Codex - Pass-B resolve split and faithful-fold completion boundary

**Objective:** Separate resolves 15 and 16, identify which pass changes the visible player, and
determine whether the remaining green output belongs to the fold task.

**Work completed:**

- Added resolve-state logging for source selection, sample selection, copy command, clear flags,
  source color format/base, and destination format/number/endian/swap/bias/pitch.
- Rebuilt and ran independent `NHL_BETA_SKIP_RESOLVE=15` and `=16` captures.
- Inspected player draws 280-356 and the original faithful-fold brief.

**Files changed:**

- `renderer/core/nhl_command_processor.cpp` - focused resolve-state diagnostics.
- `docs/agent-coordination-handoff.md` - corrected task status and this record.

**Validation:**

- Initial link failed because the closed SDK did not export
  `RB_COLOR_INFO::rt_register_indices`; replaced it with the documented hardware register indices
  `{0x2001,0x2003,0x2004,0x2005}`. Rebuild then passed.
- `codex_rov_skip15.png` / `nhllegacy_404.log`: byte-identical to normal ROV iteration 5.
- `codex_rov_skip16.png` / `nhllegacy_405.log`: byte-identical to
  `codex_rov_skip15_16.png`.
- Resolve 15 is source 4, raw depth. Resolve 16 is source color 0, converted to format 6 with endian
  2 and channel swap.

**Proven:**

- Resolve 15 has no visible role in the green player.
- Resolve 16 is the pass-B color resolve. Executing it exposes beta's correctly centered ROV player;
  skipping it leaves the trace-preloaded textured player at the old far-right folded position.
- Resolve 16 is not converting correct beta color into green. The preceding player material draws
  fetch zero-filled guest pages at addresses such as `0x19432000`, `0x17D01000`, `0x17E11000`, and
  `0x13D62000`.
- The canonical scene_02 replay oracle in `faithful-edram-fold-handoff.md` is intentionally a
  centered black silhouette because those materials predate the capture. Scene_02 has no injection
  sidecar.
- The faithful EDRAM fold/addressing objective is complete: beta's own ROV render and resolve produce
  a full player at the oracle position without skipping resolves.

**Hypotheses / unresolved:**

- Green instead of black is the shader's fallback result with absent material inputs. Real material
  parity requires texture injection, not resolve or viewport changes.
- A live capture with `NHL_INJECT_CORRELATE=1` should generate the address-to-RX2 mappings needed for
  scene_02, but that requires running the live game and capturing the same scene.

**State left for next agent:**

- Treat the faithful fold as solved behind `NHL_BETA_RT_PATH=rov`.
- Preserve `codex_rov_skip15.png`, `codex_rov_skip16.png`, `nhllegacy_404.log`, and
  `nhllegacy_405.log`.
- Continue as a texture-injection task: generate a scene_02 `.inject.json` from a correlated live
  capture, replay it through ROV, then run canonical 2D regressions.

### 2026-06-10 - Codex - Skater asset correlation and cross-capture address remap

**Objective:** Use the supplied skater asset categories to recover the missing scene_02 player
materials and determine whether populated material pages remove the flat-green player output.

**Work completed:**

- Added optional category-filtered injection-registry scanning through
  `NHL_INJECT_REGISTRY_CATEGORIES`, while retaining rendering-root-relative paths in generated
  sidecars.
- Correlated the alternate scene_02 trace against the skater categories and generated 44 mappings
  in `gpu_trace/scene_02/454109EC_stream.inject.json`.
- Replayed the active trace with those raw alternate guest addresses. All but one mapping were
  written, but capture-layout address collisions corrupted unrelated UI and left the player green.
- Aligned active draws 280-297 to alternate draws 290-308 by vertex count, texture-binding count,
  fetch slot, dimensions, and format.
- Created a focused 39-entry active-address sidecar from the matched draw pairs and excluded the
  suspicious `13ABA000 -> 13958000` mapping whose decoded and draw-time dimensions disagree.
- Replayed with `NHL_BETA_BIND_DIAG=1` and proved the remapped pages remain populated at draw time.

**Files changed:**

- `src/injection_registry.h` - accepts an optional shared relative root for filtered scans.
- `src/injection_registry.cpp` - emits paths relative to that shared root.
- `renderer/core/nhl_command_processor.cpp` - supports comma-separated
  `NHL_INJECT_REGISTRY_CATEGORIES`.
- `gpu_trace/scene_02/454109EC_stream.inject.json` - 44 alternate-capture correlations.
- `gpu_trace/scene_02/454109EC_stream.active-remap.inject.json` - 39 matched active-trace
  allocations.
- `docs/agent-coordination-handoff.md` - this record.

**Validation:**

- Rebuild after registry changes passed (four objects plus link; only the existing `getenv`
  warning remained).
- Category-filtered correlation completed against
  `H:\Emulators\games\XBOX\NHL Legacy - Vanilla\_compiled\rendering`; log
  `nhllegacy_413.log`.
- Raw-address replay: `codex_rov_skater_inject1.png`, `nhllegacy_414.log`, SHA-256
  `36CB6DE1ABA51EE6339981A46206D833C58DAD123A4B131ABD90D584907EC6E2`.
- Focused remap replay: `codex_rov_skater_remap1.png`, `nhllegacy_415.log`, SHA-256
  `20C8F40F59A964EA700F30D61291E84A03AC38F9F74582515CEA4363C8FFE924`.
- Bind-diagnostic repeat was byte-identical:
  `codex_rov_skater_remap_binddiag.png`, `nhllegacy_416.log`.
- Representative draw-time residency after remap:
  - draw 280: pant color pages `4084/4096`, `4079/4096`, and `3937/4096` nonzero.
  - draw 281: jersey color pages `4096/4096`, `4070/4096`, and `4013/4096` nonzero.
  - draws 282-288: sock, skate, glove, and stick color pages remain nonzero.
  - draws 289-297: helmet, player, player-common, and jersey color pages remain nonzero.

**Proven:**

- Alternate and active scene_02 captures use different guest allocation layouts; raw guest
  addresses cannot be transferred safely.
- Draw-signature alignment is sufficient to translate the primary skater material allocations
  between these captures.
- Missing primary color pages alone do not explain the flat green player. The matched color maps
  are populated and sampled by the same draws, but the player remains visually unchanged.
- The remaining zero inputs are systematic companion/shared resources:
  - format 49 fetches such as `0x17D01000`, `0x13D62000`, `0x18BA1000`, and `0x13BA0000`;
  - format 20/6 shared fetches such as `0x16C43000`, `0x196D1000`, `0x196E9000`, and
    `0x13988000`.
- The clean focused-remap UI versus the corrupted raw-address replay confirms the narrowed address
  translation is materially correct and avoids unrelated allocation collisions.

**Hypotheses / unresolved:**

- Format 49 fetches are likely companion material maps whose RX2 ownership was not resolved by the
  current decoded-slot correlation.
- The shared format 20/6 inputs may live outside the supplied player category folders or may be
  generated/common lookup textures. One of these shared resources may gate the shader path that
  currently produces green.
- The next useful discriminator is to map alternate addresses `0x13BA2000`, `0x1956C000`,
  `0x1824F000`, `0x189B9000`, `0x183E7000`, `0x16DC8000`, `0x19750000`, and `0x19768000`
  to their source assets or generated-resource writes, then translate them to the aligned active
  addresses.

**State left for next agent:**

- Preserve `codex_rov_skater_remap1.png`, `codex_rov_skater_remap_binddiag.png`,
  `nhllegacy_415.log`, and `nhllegacy_416.log`.
- Use `454109EC_stream.active-remap.inject.json` as the baseline; do not reuse the raw 44-address
  sidecar on the active capture.
- Trace ownership/writes for the alternate format 49 and shared format 20/6 addresses. Prefer
  extending correlation to unsupported companion formats or finding their upload packets before
  changing shader, sampler, or EDRAM behavior.

### 2026-06-10 - Claude - Green band narrowed to the 2D-composite left fold-band (in-frame, draw-independent)

**Objective:** After taking over the EDRAM resolve area, localize the green source within the fold.

**Work completed (read-only diagnostics; no source changes):**

- Located the fold code: `nhl_command_processor.cpp` ~1300-1392. Create-player content is drawn in two
  `PA_SC_WINDOW_OFFSET` passes (win=(0,0) left, win=(-640,0) right) into ONE 640-pitch surface;
  default path uses viewport-full-width + scissor-clamp-to-surface_pitch; the SDK
  `beta_render_target_cache_->Resolve` un-folds to 1280x720. `NHL_BETA_WINOFF` is an opt-in prototype,
  not active in these runs.
- `NHL_BETA_NO_RESEED=1` is byte-identical to baseline -> the green is generated in-frame, NOT from the
  captured EDRAM snapshot seed.
- `NHL_BETA_ZEN_FILTER=0` (2D/non-depth-tested draws only): the left band [0,639] is STILL green
  (bg blue->cyan at x=100, panel (10,16,50)->(9,46,53)); right band clean. So the 3D player/arena
  passes do NOT deposit the green — it is intrinsic to the 2D composite's left fold-band.
- The additive-over-darkness signature (blue+green->cyan, black+green->(0,127,15)) indicates the left
  fold-band composites over a GREEN EDRAM base that is present independent of the draw set.

**Files changed:** none (diagnostics only; `docs/agent-coordination-handoff.md` updated).

**Validation:** `run_edram.ps1 -Scene scene_02 -Frame 30 -Edram 1` with `NHL_BETA_RT_PATH=rov` plus, in
turn, `NHL_BETA_NO_RESEED=1`, `NHL_BETA_ZEN_FILTER=0/1`, and `NHL_BETA_GPUDUMP` (logs 418-421).
NOTE: `NHL_BETA_GPUDUMP` output is UNRELIABLE here — its untile assumes pitch=width=1280 but these
surfaces are 640-pitch folded, so the raw dumps are scrambled by the fold itself.

**Proven:**

- Green is the left 640-fold-band compositing over a green EDRAM base; it is in-frame, draw-set
  independent, and not from the snapshot seed or the 3D passes.

**Hypotheses / unresolved:**

- The left fold-band most likely reads UNINITIALIZED / wrongly-addressed EDRAM tiles during the
  unfold (the green base is whatever those tiles hold), rather than tiles the composite actually wrote.
- Next decisive experiment (needs a rebuild): clear the beta EDRAM host color RT to a SENTINEL (e.g.
  pure red) at frame start; if the left band base shows the sentinel, the unfold is reading unwritten
  tiles -> fix is fold-aware tile addressing/clear for the left band. Alternatively add a fold-aware
  EDRAM host-surface readback before resolve 14.

**State left for next agent:**

- Preserve `claude_rov_zen0_2donly.png`, `claude_rov_zen1_3donly.png`, `nhllegacy_420.log`,
  `nhllegacy_421.log`, `nhllegacy_422.log`.
- Build env per `rexglue-build-environment` memory (VS2022 vcvars64 + LLVM on PATH) confirmed working;
  incremental rebuild = 1 obj + link via the onboarding cmd-line.

**UPDATE 3 (draw-invariant + structural — it's the resolve UNFOLD ADDRESSING, not draws/MSAA):**

- Added `NHL_BETA_SKIP_RANGE=lo-hi` (skip draw GEOMETRY in a range; clears/binds/resolves still run)
  and an EDRAM base probe (color/depth tile bases + pitch, gated by `NHL_BETA_EDRAM_DIAG`). Rebuilt PASS.
- Range-skip bisection (`claude_rov_skiprange_*.png`): skipping 0-201, 44-278, 0-43, or 202-278 ALL
  leave the green left-band byte-identical. The green is fully DRAW-INDEPENDENT.
- Composite EDRAM params (draws ~270-360, `nhllegacy_433.log`): color0 base=tile 0, depth base=tile
  736, tile_pitch=8 tiles (10240 dw), surf_pitch=640, **msaa=1** (NOT 4X — the earlier MSAA framing
  was wrong; FORCE_SAMPLE invariance is just because it's a 1x surface). Color tiles [0,360), depth
  [736,1096) — no trivial base overlap.
- KEY REALIZATION: `SKIP_RANGE` skips geometry but the resolves still execute. Since the green
  survives skipping ALL geometry 0-278, it is produced by the RESOLVE's pitch<width UNFOLD reading the
  left DISPLAY band from the wrong EDRAM region (a depth-like value 0.06), independent of draw content
  and MSAA sample selection. This is the SDK resolve's pitch<width unfold addressing driven by beta's
  register setup (surf_pitch=640 source -> 1280-wide dest), i.e. the core
  `beta-scene04-projection` pitch<width problem, surfacing as color.
- Next: this is now gated behind either (a) a direct EDRAM compute-readback (WriteEdramRawSRVDescriptor
  -> compute copy -> readback) to SEE the color[0,360) vs depth[736,1096) tiles, or (b) a register-
  level comparison of beta's resolve source pitch/base setup vs upstream Xenia's pitch<width resolve.
  Log-only instrumentation has reached its limit.

**UPDATE 2 (MSAA-unfold RULED OUT — green is render-side EDRAM content):**

- Added `NHL_BETA_FORCE_SAMPLE=N` (diag, behind env; restores RB_COPY_CONTROL after the resolve) to
  override `copy_sample_select` on the 1280-wide fold composites. Rebuilt (PASS).
- Swept samples 0/1/2/3 (`claude_rov_sample0..3.png`). The override works (sample0==sample2,
  sample1==sample3 — two distinct sub-samples; all differ from the baseline 0&1 average), confirming
  the surface is a 2x-effective 4X-MSAA fold. BUT the left-band green is IDENTICAL in every sample.
- CONCLUSION: the green is NOT introduced by the resolve's MSAA sample selection/averaging. It is
  present in the EDRAM color content itself, in every sample -> RENDER-SIDE. The resolve faithfully
  reproduces it. Combined with `ZEN_FILTER=0` (2D-only) still green and clear-color=black, the green
  is written into the composite surface's left-fold-band EDRAM tiles during rendering — most likely
  EDRAM tile ownership/aliasing (the left band reuses tiles holding earlier green content and the
  composite blends over them, vs offscreen's single flat RTV which has no such aliasing).
- Next instrument: bisect which pass deposits green into the composite's tile-0 region (draw/pass
  isolation), then fix EDRAM tile clear/ownership for the left fold-band. NOT a resolve change.

**UPDATE (clear-color ruled out, MSAA-unfold was the prior lead — now also ruled out by UPDATE 2):**

- `nhllegacy_422.log` (new `RB_COLOR_CLEAR` probe): the guest clear color is `0x00000000` (BLACK) for
  ALL 19 resolves. The green base is NOT a guest clear color. EDRAM is cleared to black; content is
  rendered over black (offscreen proves correct), so the FOLD/RESOLVE itself produces the left-band
  green.
- The frontbuffer composite (resolve #14, dest 0x9000000, the image we see) is a 4X-MSAA resolve with
  `copy_sample_select=4` (average samples 0&1), surf_pitch=640 -> 1280x720. The 4X MSAA samples ARE
  the pitch<width fold encoding. The left-band green is therefore most likely an MSAA
  sample->pixel UNFOLD addressing/averaging defect for the left fold-band, NOT a clear/base/blend
  issue. (resolve #13, the other 1280x720, uses sample_select=0 single-sample.)
- Next experiment: force/vary `copy_sample_select` for resolve #14 (single sample 0 vs 1 vs avg) and
  observe the left band, or instrument the MSAA sample positions the SDK resolve uses for the fold.
  Regression signal stays: left band must match `codex_offscreen_baseline.png` (red byte-identical).

### 2026-06-10 - Claude - Green player is an EDRAM-fold COLOR defect, not missing materials

**Objective:** Resolve the user's observation that prior screenshots (`codex_offscreen_baseline.png`,
`codex_rov_skip16.png`) show a fully TEXTURED player, which seems to contradict the "zero
player-material pages" conclusion that motivated the scene_02 texture-injection hunt.

**Work completed (read-only investigation; no source changes):**

- Pixel-diffed the trace-preloaded ROV player (`codex_rov_skip16.png`) against the correct offscreen
  render (`codex_offscreen_baseline.png`). The player's RED channel is byte-identical at every point;
  87.4% of player-content pixels differ by EXACTLY (R,G,B)=(0,+127,+15). Full texture detail (crest,
  stripes, helmet) is present under the green — materials ARE sampled.
- Ground-truthed against the real console oracle `oracle_editplayer_f30.png`: it has the blue
  background (10,9,225) and blue UI, matching the offscreen render, NOT the ROV result. The
  (0,127,15) is therefore confirmed corruption from the ROV EDRAM round-trip; the offscreen RTV path
  (which bypasses EDRAM tiling) is correct.
- Confirmed the readback swizzle (DumpBetaEdramSwap) only reorders channels (no per-channel math), so
  the green is baked into the resolved EDRAM surface, not the PNG export.
- Ruled out three candidate sources empirically:
  - EDRAM snapshot reseed — `NHL_BETA_NO_RESEED=1` is byte-identical to baseline.
  - ROV constant-blend color (regs 0x2105-0x2108) — `codex_rov_noblend.png` still shows (0,127,15).
  - Missing materials — red-channel preservation + visible texture detail disprove a flat fallback.
- Mapped the corruption spatially (`claude_rov_corruption_map.png`). It is REGIONAL and splits at
  EXACTLY x=640 (the 640-pitch fold line): background band [0,639] corrupt, [640,843] clean, the
  centered player band [844,1016] corrupt, [1017+] clean. The defect is an additive green/blue leak
  (+30 over the dark panel, +127 over black; red and white preserved).

**Files changed:**

- `docs/agent-coordination-handoff.md` - ownership, validation rows, and this entry.

**Validation:** see the four 2026-06-10 Claude rows in the validation ledger. Reproductions used the
existing `run_edram.ps1` harness against `scene_02` frame 30; no rebuild was required.

**Proven:**

- The "green player" is the SAME pitch<width EDRAM fold problem as scene_04 projection
  (`beta-scene04-projection` memory), now manifesting as a per-band COLOR defect rather than a
  geometry one. ROV fixed the player POSITION but not the cross-fold color.
- scene_02 player materials are present in the trace and are sampled correctly; the texture-injection
  hunt (companion format-49 / format-20·6 addresses) is chasing a symptom that the green corruption
  masks, not the root cause.

**Hypotheses / unresolved:**

- The additive green most likely comes from one 640-band of the folded EDRAM color surface being
  unfolded/resolved from a wrong tile offset (aliasing a green-ish surface — candidate: the second RT
  or the depth tiles), since the additive amount tracks content darkness and varies spatially.
- Whether any secondary material gap exists CANNOT be judged until the green corruption is fixed
  (green masks the true per-texel color). Fix color first, then re-evaluate.

**State left for next agent:**

- Treat the scene_02 "green player" as a fold COLOR-resolve defect, not a texture-injection task.
- Preserve `claude_rov_baseline.png`, `claude_rov_noreseed.png`, `claude_rov_corruption_map.png`,
  `nhllegacy_417.log`, `nhllegacy_418.log`.
- Next: dump the per-band EDRAM tiles / resolve sources for the 1280x720 frontbuffer (resolve 14,
  dest 0x9000000) and identify which tile the left/player band reads its green from. Use the
  red-channel-identical-to-offscreen comparison as the objective regression signal.

### 2026-06-10 - Claude - Codebase audit pass (AUDIT ONLY)

**Objective:** Complete the audit prompted in `docs/codebase-audit-prompt.md` (via `codex-onboarding.md`):
find correctness bugs, tech debt, red flags, and inaccurate judgments across the hand-written backend
and the project's recorded conclusions. Produce a prioritized findings report; do not apply fixes.

**Work completed (read-only; fanned out 5 parallel readers, then re-verified the top items myself):**

- Wrote `docs/codebase-audit-findings.md`: prioritized findings table (B1-B16, R1, M/L items),
  `NHL_BETA_*` keep/retire/consolidate inventory, documented-judgment audit, quick-wins, and a
  verified-CORRECT list so the next agent doesn't re-chase clean code.
- Independently re-verified against current source: **B1** (`NHL_BETA_NOBLEND` writes `0x2201+i` =
  wrong blend regs for RT1-3; the file's own `kBlendControlRegisters` at cpp:1580 is correct),
  **B2** (`this + 0xCE8` raw private-member read at cpp:1994), **B3** (`RecordBetaGpuDumps` leaves
  shared memory in COPY_SOURCE — no `UseForReading()` before return at cpp:2605).
- Recorded corrections for two demonstrably-stale judgments (history preserved, not rewritten):
  the "faithful fold objective is complete" row (now annotated POSITION-only / SUPERSEDED for color),
  and memory `tier1-backend-architecture.md` `NHL_BETA_TEX` → `NHL_BETA_NOTEX`.

**Files changed:**

- `docs/codebase-audit-findings.md` - new findings report.
- `docs/agent-coordination-handoff.md` - change-ledger row, ownership-table correction (B6), this entry.
- `C:\Users\puckh\.claude\projects\...\memory\tier1-backend-architecture.md` - correction note (B15/L11).

**Validation:** Read-only audit; no build. All `file:line` cites are to the current tree. The three
top bugs were re-read directly from source (B1/B2/B3 confirmed).

**Proven:**

- B1, B2, B3 are real and grounded in current source (re-verified, not just inherited from a reader).
- The main owned-draw register save/restore discipline is CLEAN (no unrestored override on any early
  return) — only the `NHL_BETA_NOBLEND` index (B1) is wrong, and it self-restores within the draw.
- No diagnostic alters the default/non-takeover path **except** `NHL_BETA_DRAWPROBE` (R1), which by its
  own comment can cause base-frame device loss; off-by-default + one-shot. Recommend RETIRE.

**Hypotheses / unresolved:**

- B4 (untile loop vs SDK `GetTiledOffset2D`) and B12 (BGRA-assumed dump) are asserted from header
  cross-reference; they should be confirmed against a known-good surface before trusting/changing the
  gpudump oracle. Flagged, not applied.

**State left for next agent:**

- AUDIT ONLY — no source was edited. The findings doc is the deliverable; fixes are proposed, not made.
- Quick wins (B1, B3, L5, L10, B14, R1, B11) are safe isolated edits if/when someone picks them up.
- The active green-band investigation's load-bearing diagnostics (`NHL_BETA_RESOLVE_DIAG`,
  `FORCE_SAMPLE`, `SKIP_RANGE`, `EDRAM_DIAG`, gpudump knobs) were explicitly preserved in the inventory.

### 2026-06-10 - Claude - Audit-fix Phase 1 (correctness + quick wins) LANDED

**Objective:** Execute Phase 1 of the approved audit-fix plan
(`~/.claude/plans/build-out-a-plan-eventual-mountain.md`): all safe correctness fixes + quick wins,
none touching the active green-band flag set; keep the default/2D-parity path byte-identical.

**Work completed (16 findings):** B1, B3, B10, R1, M5, L5, L3 in `nhl_command_processor.{cpp,h}`;
B7, B8, L7, L8 in `injection_registry.cpp`; B13, L9 in `loose_tree_device.cpp`; B14 in
`union_device.{cpp,h}` + `nhllegacy_app.h`; L9-app in `nhllegacy_app.h`; L10 in `diag_hooks.cpp`.
Notable: **`NHL_BETA_DRAWPROBE` fully retired** (R1) — the only diagnostic that ran on the default path
and could cause device loss; removed both call sites, `RunBetaDrawProbe`, the now-orphaned
`RecordBetaClearTest`, and the `beta_*_probe_done_` members. B7/B8 rewrote `ParseSidecar` to be
object-bounded so a malformed sidecar can no longer mis-pair an addr with a neighbouring entry's rx2
(which would inject into the wrong guest address).

**Files changed:** `renderer/core/nhl_command_processor.cpp`, `renderer/core/nhl_command_processor.h`,
`src/injection_registry.cpp`, `src/loose_tree_device.cpp`, `src/union_device.cpp`, `src/union_device.h`,
`src/nhllegacy_app.h`, `src/diag_hooks.cpp`. (See the change-ledger row for per-finding detail.)

**Validation:**

- Build PASS (8/8 objects, link OK; only the pre-existing `getenv`/`fopen` deprecation warnings).
- Default-path no-regression: scene_00 + scene_03 frame-30 oracle PNGs **SHA256 byte-identical**
  before vs after (8D91C6FF…, A7627832…). Logs 435 (before) / 437-438 (after).
- Green-band repro unperturbed: `run_edram.ps1 -Scene scene_02 -Edram 1 NHL_BETA_RT_PATH=rov` produced
  a valid 1280×720 `beta_owned_draw.png`, no Application-Error crash. Log 439.
- B7/B8 targeted: well-formed sidecar → 39 entries (log 441); a copy with one entry's `rx2` removed →
  38 entries, the bad `0x17E11000` entry **skipped, not mis-paired** (no leak to the next rx2), no
  crash (log 442). The old parser would have mis-paired.
- B1 targeted: `NHL_BETA_NOBLEND=1` run completes, no crash (log 443).

**Proven:**

- All Phase-1 fixes are in non-default or env-gated paths; the default/2D-parity output is byte-for-byte
  unchanged. The object-bounded sidecar parser fixes the silent guest-RAM mis-injection without
  regressing well-formed parsing.

**Hypotheses / unresolved:**

- Phase 2 (B2 sanity-check guard for the `this+0xCE8` read, B11 register-namespace, and the dump-oracle
  correctness bundle B4/B12/B5/M3, plus M6) is NOT yet done — these need surface validation. B4/B12
  specifically must be confirmed against a known-good surface before changing the gpudump untile.
- Phase 3 (diag-config caching, ~20-flag retirement, B9 per-frame-flag refactor, L1 god-function split)
  remains deferred until the green-band fix lands, per plan.

**State left for next agent:**

- Phase 1 is landed + validated. Next: Phase 2 per the plan. The active green-band diagnostics are
  untouched. Note `RecordBetaClearTest` was removed (was DRAWPROBE-only scaffolding); if anything
  external referenced it, it's gone — nothing in-tree did.

### 2026-06-10 - Claude - Audit-fix Phase 2 (fragile-coupling & dump-oracle hardening) LANDED

**Objective:** Phase 2 of the audit-fix plan — harden the fragile SDK couplings and make the
green-band investigation's ground-truth dumpers honest, without disrupting the active flag set.

**Work completed:** B2 (`ReadCpViewHeapChecked` SEH+descriptor guard for the `this+0xCE8` read),
B11 (`namespace beta_reg` dedup + `static_assert` tripwire), B5 (ZFAR marked non-faithful in-code),
B4/B12 (untile/BGRA honesty comments on both dumpers; math unchanged pending a known-surface A/B),
M3 (gpudump batch not clobbered), M6 (`HostFile::ReadSync` per-file mutex).

**Files changed:** `renderer/core/nhl_command_processor.cpp`, `src/loose_tree_device.cpp`.

**Validation:**

- Build PASS — the B11 `static_assert(kColorInfo[0]==RB_COLOR_INFO::register_index)` compiled (RT0
  index matches SDK 0.8.0), the SEH `__try/__except` guard compiled clean.
- scene_00 + scene_03 default-path frames still **SHA256 byte-identical** to the pre-Phase-1 baseline.
- Green-band rov repro: `beta_owned_draw.png` **SHA256-identical to the Phase-1 rov result**; the B2
  validation passed live (no `+0xCE8 failed` WARN → the heap is the expected shader-visible view heap,
  sampler table bound normally); no crash. Logs 444-446.

**Proven:**

- B2 hardens the single most fragile construct (raw offset read) with zero behavior change on the happy
  path — it only diverges (safe nullptr → skip sampler table) if the SDK member ever moves.
- B11 turns a silent SDK-bump hazard into a compile-time failure for RT0.
- Dump-oracle comments (B4/B12/B5) are honest now; the untile MATH is deliberately unchanged (needs a
  known-surface A/B before trusting GetTiledOffset2D), so no green-band evidence shifted.

**Hypotheses / unresolved:**

- B4 (full GetTiledOffset2D untile switch) and a faithful B5 ZFAR are deferred until validated against a
  known surface — they're documented, not applied. L4 (gpudump readback OOB on large W/H override) was
  noted but not changed (out of the approved Phase-2 set).

**State left for next agent:**

- Phases 1 + 2 landed + validated; default/2D-parity byte-identical throughout; green-band rov result
  unchanged. **Phase 3 (deferred per plan): the disruptive cleanup** — `BetaDiagConfig` getenv-caching,
  retire the ~20 dead `NHL_BETA_*` flags, consolidate diag families, the B9 per-frame-flag refactor, and
  the L1 god-function split — should run AFTER the green-band fix lands, so the bisection toolkit stays
  intact during the active investigation. See `docs/codebase-audit-findings.md` §2 and the plan file.

---

## Session entry template

```markdown
### YYYY-MM-DD - Agent - Short title

**Objective:**

**Work completed:**

**Files changed:**

- `path/to/file` - behavioral summary.

**Validation:**

- `exact command` - PASS/FAIL and important artifact or metric.

**Proven:**

- Facts supported by source inspection, logs, builds, or images.

**Hypotheses / unresolved:**

- Clearly labeled uncertainty.

**State left for next agent:**

- Exact next action, relevant toggles, artifacts, and ownership status.
```
