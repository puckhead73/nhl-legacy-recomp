# Task prompt — Codebase audit: red flags, tech debt, bugs, inaccurate judgments

> Hand-off prompt for an audit pass over the hand-written code and the project's own recorded
> conclusions. Read `docs/agent-coordination-handoff.md`, `docs/codex-onboarding.md`, and the memory
> index `memory/MEMORY.md` FIRST for context on what is intentional vs accidental.

## Mission

Audit the **hand-written** parts of this project for: (1) correctness bugs, (2) tech debt, (3) general
red flags, and (4) **inaccurate judgments** — documented or in-code claims that the evidence does not
actually support. Produce a prioritized findings report. **Do NOT apply fixes** in this task (it is an
audit); propose them. The one exception: you may note "quick wins" separately, but still don't edit
source.

This is a recompiled Xbox 360 game (`NHL Legacy`) with a custom D3D12 "beta" Tier-1 backend layered on
a closed SDK (`rexglue`). The backend has been built by two agents (Codex, Claude) under heavy
empirical iteration, so expect accumulated diagnostics and hypothesis-shaped code. The goal is to
separate load-bearing code from cruft, and proven facts from over-stated ones — without breaking the
active investigation.

## Scope

IN SCOPE (audit these):

- `renderer/core/` — especially `nhl_command_processor.cpp` (~3600+ lines) and `.h`. This is the
  beta backend and the bulk of the risk surface.
- `src/` — e.g. `injection_registry.{h,cpp}` and siblings.
- Any other hand-written `.cpp/.h` under the repo root that is part of the build (check
  `CMakeLists.txt` / `CMakePresets.json` for the real source list).
- The project's own **recorded judgments**: `docs/*.md` (handoff, onboarding, faithful-fold brief,
  build-order) and `memory/*.md`. Cross-check their "Proven"/"Status" claims against the code and the
  evidence.

OUT OF SCOPE (do not audit / do not edit):

- `generated/` and `generated.bak_*/` — machine-generated recompiled code. Not hand-written; ignore.
- The closed SDK headers under `E:\Tools\rexglue-sdk\...` (read for reference only).
- The sibling `nhl-database-studio` tree unless it is part of this repo's build.
- Game data on `H:\`.

## What to look for (calibrated to THIS codebase)

### 1. Correctness bugs
- **Register-file mutations that aren't restored.** The beta path temporarily writes guest registers
  (e.g. blend disable saves/restores `0x2201..` around a draw; the `NHL_BETA_FORCE_SAMPLE` diag
  saves/restores `RB_COPY_CONTROL` 0x2318). Audit EVERY `(*register_file_)[...] =` / `WriteRegister`
  write in the beta path: is it unconditionally restored on ALL exit paths (early `return`,
  exceptions, `skip_this`)? An un-restored override silently corrupts later draws/frames.
- **Per-frame / per-draw resource creation & leaks.** e.g. `RecordBetaGpuDumps` creates a committed
  READBACK resource per dumped address per call; the EDRAM reseed allocates buffers. Check for
  resources created in hot paths without pooling/reuse, ComPtr cycles, and missing `Unmap`.
- **Submission / barrier correctness.** The beta path opens submissions, transitions resources, and
  records into deferred command lists. Look for transitions left in the wrong state across
  submission reopen, missing barriers, or double-invalidation (Codex already found+removed one
  duplicate `RangeWrittenByGpu` — check for siblings).
- **Off-by-one / addressing.** EDRAM tile math, pitch<width fold addressing, untile loops
  (`WriteBetaGpuDumps`, `DumpBetaEdramSwap`) — bounds, endian/swap assumptions, `kEdram*` constants.
- **Lifetime/threading.** `static` locals in methods that run per-frame across a looped replay;
  members reset assumptions (`beta_*_done_` one-shots) that break when the replay loops frames.

### 2. Tech debt
- **Diagnostic sprawl.** `nhl_command_processor.cpp` has dozens of `std::getenv("NHL_BETA_*")` calls,
  many in the per-draw hot path (getenv is not free and is queried every draw). Inventory them:
  which are still useful, which are dead/superseded (e.g. `NHL_BETA_ROV_ZERO_SIGNS` was proven NOT a
  fix; `NHL_BETA_WINOFF` is a prototype), which duplicate each other. Recommend a consolidation
  (e.g. cache env lookups once, or a single diag-config struct) — but flag which are load-bearing for
  the active green-band investigation (`docs/edram-readback-instrument-prompt.md`) and must stay.
- **Workarounds for non-exported SDK symbols.** Hardcoded register-index arrays replacing
  non-exported SDK statics (e.g. `{0x2001,0x2003,0x2004,0x2005}` standing in for
  `RB_COLOR_INFO::rt_register_indices`). These are fragile across SDK updates — flag each, note the
  SDK version they were derived against (0.8.0), and whether a 0.8.1 path exists.
- **Long functions / mixed concerns.** The owned-draw render function interleaves pipeline setup,
  viewport/fold logic, diagnostics, and binding. Note the worst offenders and suggest seams.
- **Commented-out / prototype paths** left inline (WINOFF, VP_FULLRT, etc.) — keep or excise?

### 3. Red flags
- Comments asserting conclusions as fact that may be stale or were context-specific (e.g. "the
  long-hunted all-zero textured output", "the gamma/color gaps DON'T EXIST"). Flag confident claims
  that lack an in-repo evidence pointer.
- Magic numbers without provenance; silent fallbacks that mask failure; `getenv`-gated behavior that
  changes the DEFAULT path (diagnostics must not alter non-takeover/parity output).
- Error handling that logs and continues where it should hard-fail, and vice-versa.

### 4. Inaccurate judgments (HIGH VALUE — do this carefully)
The docs and memory record "Proven" facts and statuses. At least one was wrong: the handoff long
claimed the scene_02 green player was "missing materials," which was disproven (oracle-confirmed it is
a pitch<width fold COLOR defect — see the 2026-06-10 Claude entries and memory
`rov-green-player-is-fold-color`). Treat that as evidence that other recorded conclusions deserve
scrutiny. For each significant "Proven"/"Status: SOLVED/Complete" claim in `docs/*.md` and
`memory/*.md`:
- Is it backed by a reproducible artifact/log/build, or is it inference stated as fact?
- Does the cited file/function/flag/address STILL exist in the code? (Memory explicitly warns recalled
  facts may be stale — verify before trusting.)
- Are "Superseded" notes consistent, or do later entries contradict earlier "Proven" rows without
  marking them?
- Flag claims that are: contradicted by code, unverifiable, or over-confident relative to their
  evidence. Be specific and cite both sides.

## Method

- Ground EVERY finding in `file:line` evidence. No vibes-only findings.
- Classify each: severity (high/med/low) x category (bug / tech-debt / red-flag / inaccurate-judgment).
- For bugs, state the concrete failure mode and a minimal repro or the trigger condition.
- For inaccurate judgments, quote the claim and the contradicting evidence.
- Distinguish INTENTIONAL (load-bearing, documented) from ACCIDENTAL. When unsure whether something is
  deliberate, say so and ask rather than asserting it's a bug — this codebase has many deliberate
  hacks behind env gates.
- Prefer breadth first (sweep all in-scope files for the patterns above), then depth on the top risks.
  Consider fanning out parallel readers per area (command processor / resolve+EDRAM / injection /
  docs+memory) and synthesizing.

## Deliverable

Write `docs/codebase-audit-findings.md` containing:
1. A prioritized findings table: `# | severity | category | file:line | finding | impact | proposed action`.
2. A short "load-bearing vs cruft" inventory of the `NHL_BETA_*` env diagnostics (keep / retire /
   consolidate), explicitly preserving those the active green-band instrument needs.
3. A "documented-judgment audit" subsection: each scrutinized claim, verdict (holds / over-stated /
   wrong / stale-reference), and evidence.
4. A short "quick wins" list (safe, isolated fixes) — described, NOT applied.

Then: add a Session-log entry + change-ledger row in `docs/agent-coordination-handoff.md` pointing to
the findings doc, and (only where a recorded judgment is demonstrably wrong/stale) flag it in the
handoff/memory with a "superseded/correction" note — do not silently rewrite history.

## Guardrails

- AUDIT ONLY — do not edit source files or apply fixes. Editing is `docs/` (findings + ledger) and, if
  a recorded judgment is demonstrably wrong, a correction note in the relevant doc/memory.
- Do not break or delete env-gated diagnostics; the active investigation
  (`docs/edram-readback-instrument-prompt.md`) depends on `NHL_BETA_RESOLVE_DIAG`,
  `NHL_BETA_FORCE_SAMPLE`, `NHL_BETA_SKIP_RANGE`, `NHL_BETA_EDRAM_DIAG`, and the gpudump knobs.
- The default/non-takeover and 2D parity paths must be treated as sacred — any finding that a
  diagnostic alters them is HIGH severity.
- Don't audit or touch `generated/`. If a finding references generated code, note it as out-of-scope.
- Build to verify a suspected compile-level issue only if needed (incremental build per
  `rexglue-build-environment` memory); otherwise reason from source.
