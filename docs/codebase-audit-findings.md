# Codebase audit findings — beta backend, support code, and recorded judgments

> Produced per `docs/codebase-audit-prompt.md`. AUDIT ONLY — no source files were edited.
> Date: 2026-06-10. Author: Claude (audit pass).
> Method: five parallel readers (register/leak/lifetime bugs; EDRAM/resolve/addressing;
> env-diagnostic inventory; `src/` support files; documented-judgment cross-check), then
> independent re-verification of the highest-value provable items against current source.
> All `file:line` references are to the current tree (`nhl_command_processor.cpp` = 3761 lines).

> **STATUS (2026-06-10): Phases 1 & 2 of the fix plan are APPLIED + validated** (default path byte-identical;
> green-band rov result unchanged — see `docs/agent-coordination-handoff.md` ledger). Applied: B1, B3, B10,
> R1, M5, L5, L3, B7, B8, L7, L8, B13, B14, L9, L9-app, L10 (Phase 1); B2, B11, B5, B4, B12, M3, M6 (Phase 2,
> several comment-only where the math/behavior change needs a known-surface A/B). **DEFERRED to Phase 3**
> (until the green-band fix lands, to keep the bisection toolkit intact): L2 (BetaDiagConfig getenv caching),
> the ~20-flag RETIRE, the diag-family CONSOLIDATE, B9 (per-frame-flag refactor), L1 (god-function split),
> and the file/line references below now drift by the lines those edits added — re-grep before trusting them.
> `file:line` references below reflect the PRE-fix tree.

---

## 0. Top-line summary

The beta backend's **register-file save/restore discipline in the main owned-draw path is clean**
(RB_SURFACE_INFO, RB_BLENDCONTROL, PA_SU_SC_MODE_CNTL all restored before every early return; the
`NHL_BETA_FORCE_SAMPLE` 0x2318 override is restored unconditionally). The real risks cluster in:

1. **Diagnostic-only correctness bugs** that would mislead the *active investigation* (a blend-register
   index bug, untile loops that don't match the SDK's tiled-address sequence, a shared-memory buffer
   left in the wrong state).
2. **Fragile couplings to the closed SDK** — most acutely a raw `this + 0xCE8` private-member read.
3. **Loop-replay fragility** from overloading a lifetime counter as a per-frame "first draw" flag.
4. **A sidecar JSON parser** in `injection_registry.cpp` that can silently mis-pair address→asset
   entries on malformed input (this one writes into guest RAM, so mis-pairing is corruption).
5. **Recorded-judgment drift**: one "Complete/Proven" faithful-fold row is not reconciled with the
   still-open green-color defect; the `NHL_BETA_TEX` flag in memory is stale (renamed/inverted to
   `NHL_BETA_NOTEX`); a "RENDER-SIDE" green conclusion is asserted while the deciding instrument
   (`NHL_BETA_EDRAM_DUMP`) was never built.

Nothing found indicates the **default / non-takeover / 2D-parity path** is altered by a diagnostic —
**with one exception**: `NHL_BETA_DRAWPROBE` runs on the default path and, by its own comment, can
corrupt the base frame / cause device loss. It is off-by-default and one-shot, but it is the single
diagnostic that violates the "must not perturb non-takeover output" rule. See R1.

---

## 1. Prioritized findings table

Severity x category. `B`=bug, `TD`=tech-debt, `RF`=red-flag, `IJ`=inaccurate-judgment.
Independently re-verified items are marked ✓.

| # | sev | cat | file:line | finding | impact | proposed action |
|---|-----|-----|-----------|---------|--------|-----------------|
| B1 ✓ | high | B | `nhl_command_processor.cpp:1171-1172` | `NHL_BETA_NOBLEND` writes `(*register_file_)[0x2201 + i]` for i=0..3 → regs 0x2201/0x2202/0x2203/0x2204. The four blend-control regs are **0x2201, 0x2209, 0x220A, 0x220B** — the file's own `kBlendControlRegisters` (line 1580) and the ROV path (line 1631) have it right. | Diag only disables blend on RT0; for RT1-3 it instead clobbers RB_COLORCONTROL/RB_COLOR_MASK while the PSO + system-constants are built from corrupted state. Within-draw restore means live state survives, but the diagnostic's verdict is invalid. | Replace `0x2201 + i` with `kBlendControlRegisters[i]` at 1171-1172 and the restore at 1283-1285. |
| B2 ✓ | high | B/RF | `nhl_command_processor.cpp:1990-1994` | Reads the CP's private `view_bindful_heap_current_` by hardcoded byte offset `static_cast<...>(this) + 0xCE8` ("verified from rexruntimerd.dll disasm @0x178448"). | Silently breaks on any SDK rebuild that changes member layout (reorder / new member / debug-vs-release padding) → arbitrary pointer → crash or, worse, a bogus heap that desyncs the untile (the exact failure this code exists to prevent). Most fragile construct in the tree. | Add a runtime sanity check (verify the read pointer matches a heap the CP is known to own) before use; pin the comment to SDK 0.8.0 explicitly; `static_assert` if any layout anchor is exported. |
| B3 ✓ | high | B | `nhl_command_processor.cpp:2575-2605` (`RecordBetaGpuDumps`) | Flips `beta_shared_memory_->UseAsCopySource()` (2575) and returns at 2605 **without** the matching `UseForReading()`. The correct sibling in `RenderBetaOwnedDraw` restores it (1836→1844). | After an `NHL_BETA_GPUDUMP` dump, the shared-memory buffer is parked in COPY_SOURCE; a later read assumes the read state → wrong/absent transition barrier. Masked today only because capture is one-shot. | Add `beta_shared_memory_->UseForReading();` after the dump loop, mirroring 1844. |
| B4 | high | B | `nhl_command_processor.cpp:2635-2640` (`WriteBetaGpuDumps`), dup at `3697-3702` | Hand-rolled 32bpp untile uses row-major macro-tile order `((y/32)*(aligned_w/32)+(x/32))`; the SDK's `GetTiledOffset2D` (header `pipeline/texture/util.h:197-265`) documents an interleaved X term `((X>>6)<<12)\|(((X>>5)&1)<<10)`. Comment at 3696 claims "matches xenos::TiledAddress 32bpp" — it does not. | The gpudump/`NHL_DUMP_ADDRS` images are used as ground-truth oracles for the green-band hunt; a wrong untile produces horizontally swapped 32px bands and can send the investigation off-course. | Call/mirror `texture_util::GetTiledOffset2D(x,y,pitch,bpb_log2=2)`; drop the "matches" claim or label the dump approximate. |
| B5 | high | B/RF | `nhl_command_processor.cpp:3025-3046` (`NHL_BETA_ZFAR`) | Depth-tile far-seed treats tiles `[base..base+360)` as one contiguous byte run and fills `0xFF`. EDRAM depth tiles are not contiguous for a folded/MSAA surface (stride = surface tile-pitch), and `0xFFFFFFFF` is not the D24FS8 encoding of far-depth. | Can miss real depth tiles and clobber interleaved color tiles; even in-range tiles get a "far-ish by luck" value. ZFAR is a non-faithful diagnostic, not a fix. | Compute the depth tile set from actual pitch/base/MSAA like the RT cache; seed the format-correct far value (or clear via the RT cache). Mark ZFAR non-faithful in docs. |
| B6 ✓ | high | IJ | `docs/agent-coordination-handoff.md` Codex 2026-06-09 "Proven" row vs `faithful-edram-fold-handoff.md` + `rov-green-player-is-fold-color.md` | "The faithful EDRAM fold/addressing objective is complete" / ownership "Complete" is not reconciled with the later oracle-confirmed open green COLOR defect, and the active brief still calls the fold "the hard task." The `NHL_BETA_RT_PATH=rov` flag and `BetaResolveEdram` do exist (3354). | The project's headline task reads as both "solved" and "active hard task." Same class of stale judgment as the calibration example (green=="missing materials"). | Mark the "objective is complete" row **superseded** (position solved; color open). Done in this pass — see §3 / handoff correction note. |
| B7 ✓ | high | B/RF | `injection_registry.cpp:196-237` (`ParseSidecar`) | Per-entry `find_str("rx2", a, ...)` searches forward with no `{...}` object boundary. A truncated/edited sidecar missing one `rx2` grabs the *next* object's value, mis-pairing entry N's `addr` with N+1's `rx2`/`slot` and skipping N+1. | Stage-1 injection writes into guest RAM; a mis-paired entry injects an asset at the **wrong base** — silent corruption, not a no-op. Trigger: any sidecar not byte-matching `WriteSidecar`'s exact output (kill mid-write, hand-edit). | Bound each entry to `[`{`, matching `}`)` and run all field scans inside that substring; reject entries missing addr or rx2. |
| B8 | high | B | `injection_registry.cpp:206-216` (`find_str`) | `s.find('"', s.find(':', k+pat.size()) + 1)` — the colon `find` can return `npos`; `npos+1==0` restarts the quote scan from file start, returning a stale earlier quote. Closing-quote scan is also unbounded. | On any non-exact input, returns plausible-but-wrong strings instead of failing → compounds B7. | Capture the colon offset, `return npos` if not found; bound `q1`/`q2` to before the next `,`/`}`. |
| B9 | med | B | `nhl_command_processor.cpp:1014`, `2376` (`first_draw` / `beta_takeover_rendered_`) | `first_draw = (beta_takeover_rendered_ == 0)` gates per-frame-required work (cache `BeginSubmission`/`BeginFrame`, 512MB `RequestRange`+`UseForReading`, RT clear), but the counter only ever `++`s and is never reset. | On a 2nd looped capture frame, `first_draw` is false → caches never re-begun, shmem never re-resident, RT never re-cleared. Masked today only by the one-shot `beta_capture_done_` gate; design supports exactly one captured frame. | Track a real per-frame flag reset in `IssueSwap`; don't overload the lifetime counter. (Also fixes L-items below.) |
| B10 | med | B | `nhl_command_processor.cpp:3377-3379` (`BetaResolveEdram`) | Resolve source select clamps `copy_src_select` to `[0,3]` via `std::min(...,3u)`. Xenos `copy_src_select >= 4` selects **depth** as the resolve source, not color RT3. | A depth resolve is read as if it were color RT3 — the exact depth-as-color aliasing the green-band probe is hunting; the clamp hides it. | Branch to RB_DEPTH_INFO when `copy_src_select >= 4`; at minimum log when the clamp fires. |
| B11 | med | TD/RF | `nhl_command_processor.cpp:1579`, `1580`, `3376` (+ inline reg indices throughout) | Hardcoded register-index arrays stand in for non-exported SDK statics (`RB_COLOR_INFO::rt_register_indices`). `kColorInfoRegisters` is duplicated 3× (1579, 3376, and the per-i form). All verified correct vs 0.8.0 `register_table.inc`, but fragile across SDK bumps and mostly un-provenanced. | Silent wrong-register reads on an SDK update. | Hoist all into one `namespace nhl_reg_080` with a "re-verify on SDK bump" note; de-dup the 3 copies; `static_assert` where the symbol's `register_index` is exported. |
| B12 | med | B | `nhl_command_processor.cpp:2644-2648`, `3708` (`WriteBetaGpuDumps`, `NHL_DUMP_ADDRS`) | Readback-to-PNG hardcodes `d[0]=p[2];d[1]=p[1];d[2]=p[0]` ("assume BGRA->RGBA") with no format/endian awareness (verified: pure channel reorder, no per-channel math). `DumpBetaEdramSwap` (2459-2463) does decode swap properly. | If the dumped surface is RGBA or 2_10_10_10, the PNG is silently wrong — another mislead-the-investigation risk. | Thread the guest format/endian (already read in RESOLVE_DIAG) into these dumpers, or label PNGs "BGRA-assumed." |
| B13 | med | B | `loose_tree_device.cpp:165-175` | `registered_` is set `true` *before* checking that bytes exist; if `ReadFile(host_)` returns empty (lock/transient I/O/0-byte) nothing is registered but the flag stays set, so the entry is never re-attempted. | Permanent per-session capture gap → a downstream "missing" texture with no diagnostic. | Set `registered_ = true` only inside the successful `RegisterRx2` branches. |
| B14 | med | RF | `union_device.cpp:29-33` + `nhllegacy_app.h:202-212` | `lower_.Initialize()` return value is discarded (best-effort by design), but `OnPostSetup` unconditionally logs "[loose-overlay] mounted". | User is told loose files are overlaid when the lower layer failed; loose edits then silently do nothing. | Expose `lower_active()`; gate the "mounted" log on it (or WARN when the lower layer is unavailable). |
| B15 ✓ | med | IJ | memory `tier1-backend-architecture.md` (older entries) vs `nhl_command_processor.cpp:1875` | Memory repeatedly names `NHL_BETA_TEX` (opt-in, default OFF). Code has no such flag; textures are **default-ON**, disabled by `NHL_BETA_NOTEX` (`beta_tex = getenv("NHL_BETA_NOTEX")==nullptr`). The file's own newer line and onboarding say "tex default-on" — stale + corrected coexist unmarked. | A reader acts on a flag that does nothing. | Add a superseded note in the memory file (done in this pass — see §3). |
| B16 | med | IJ | memory `rov-green-player-is-fold-color.md` "CONCLUSION … RENDER-SIDE" vs handoff UPDATE 3 + `edram-readback-instrument-prompt.md` | Memory states render-side EDRAM tile-ownership as settled; the coordination ledger's UPDATE 3 reaches the opposite (resolve-side pitch<width unfold) lead and says "log-only instrumentation has reached its limit," requiring `NHL_BETA_EDRAM_DUMP` — which grep confirms was **never built**. | Render-vs-resolve attribution is over-confident; the well-supported half (fold COLOR defect at x=640, red byte-identical, not missing materials) holds. | Soften the memory to "fold COLOR defect; render-vs-resolve unresolved pending the EDRAM-dump instrument." |
| R1 | med | RF | `nhl_command_processor.cpp:3177`, `3210` (`NHL_BETA_DRAWPROBE`) | Read on the **default / non-takeover** path; calls `RunBetaDrawProbe()` which (per the 3203-3209 comment) records into the base's SHARED command list and "corrupts the base's ROV frame -> device loss." | The only diagnostic that can perturb/crash the non-takeover path. Off-by-default + one-shot, but violates the sacred-path rule. Purpose documented as already served. | RETIRE. |
| M3 | med | B/TD | `nhl_command_processor.cpp:2577`, `2595` (`RecordBetaGpuDumps`) | `beta_gpudump_bufs_.clear()` (drops ComPtrs possibly still queued for `WriteBetaGpuDumps`) then a fresh ~3.75MB READBACK committed resource per address per call, no pooling. | Re-entry leaks/reallocs and can drop a buffer whose GPU copy is in flight. Diag-gated, low blast radius. | Pool by address or assert empty before refill; don't `clear()` while a copy may be unconsumed. |
| M5 | low | B/TD | `nhl_command_processor.cpp:3334` (`resolve_dest_bases_`) | `push_back` per resolve in `IssueCopy`, never cleared. | Slow unbounded growth on a long live session (one entry/resolve/frame forever). | Clear in `IssueSwap` alongside the other per-frame counters, or guard behind replay/takeover. |
| M6 | low | B | `loose_tree_device.cpp:79-94` (`HostFile::ReadSync`) | `stream_.clear(); seekg; read` on a shared `std::ifstream` with no lock. | Data race if the SDK ever dispatches concurrent `ReadSync` on one handle (unverified coupling). | Add a per-file mutex, or assert+comment that the SDK serializes per-handle reads. |
| L1 | low | TD | `nhl_command_processor.cpp:984-2377` (`RenderBetaOwnedDraw`) | ~1,390-line god-function: cache ticking, depth/PSO overrides, shader translation + async spin-waits, viewport/fold/WINOFF, the ROV sysconst block (1531-1700), constant upload, shmem residency, texture/sampler binding, the draw — plus ~12 inline `getenv` diag blocks. | Hard to modify safely; the bug surface for B1/B9/B10 lives here. | Extract seams: `BuildSystemConstants` / `BuildRovSystemConstants` / `ComputeOwnedViewport` / `BindOwnedTextures` / `BetaDrawDiag`. |
| L2 | low | TD | `nhl_command_processor.cpp` (per-draw `getenv` throughout) | ~30 distinct `std::getenv` fire **every owned draw** (`NHL_BETA_BIND_DIAG` 7×, `NHL_BETA_DEPTH_DIAG` 6×, `NHL_BETA_SHMEM_MB` 2×, etc.); `getenv` walks the env block each call on the CP worker thread. | Needless hot-path cost; minor. | Cache all reads once into a `BetaDiagConfig` at SetupContext (the codebase already does this for `beta_edram_enabled_`/`inject_correlate_enabled_`). |
| L3 | low | B | `nhl_command_processor.cpp:2794`, `2798` (`DumpBetaDepthStats`) | Decodes depth as 24-bit UNORM; default surface is D24FS8 (float24). | `cleared%` / normalized prints are wrong-scaled for FS8; FLAT/VARYING verdict still valid. | Branch on `beta_depth_xenos_fmt_`, decode float24 when kD24FS8. |
| L4 | low | TD | `nhl_command_processor.h:334` + `cpp:2617-2642` | `kBetaGpuDumpBytes = 1280*768*4` fixed, but `NHL_BETA_GPUDUMP_W/H` override only the interpretation loop, not the copy/readback size; a large W/H reads past the mapped buffer. | OOB read in a diagnostic if W/H overridden large. | Clamp the untile loop to `srcb+4 <= kBetaGpuDumpBytes`, or size readback from actual W/H. |
| L5 | low | TD | `nhl_command_processor.h:246,248,249` | Dead members `beta_view_bump_`, `beta_sampler_bump_`, `beta_srv_heap_` — zero uses in the .cpp; `beta_srv_heap_` is a never-created descriptor heap (code uses the CP's global view heap instead). | Confusing/stale. | Remove. |
| L7 | low | B | `injection_registry.cpp:57`, `77`, `122` | `for (slot < 64)` ceiling is an unexplained magic number; `try_emplace` is first-writer-wins, so a true 64-bit FNV collision silently maps two assets to one with no warning. | Silent mis-map on collision (low probability); undocumented 64 ceiling. | Document the 64-slot provenance; `REXLOG_WARN` on a collision where relpath/slot differ. |
| L8 | low | TD | `injection_registry.cpp:100-104` vs `loose_tree_device.cpp` `EndsWithCi` | Two different `.rx2` matchers with different case semantics (the registry one case-folds only `ext[1]`). | Latent inconsistency. | Share one `EndsWithCi(path, ".rx2")` helper. |
| L9 | low | RF | `nhllegacy_app.h:222-300` (replay path) | Replay hard-`TerminateProcess`es to dodge a known teardown double-free (0xC0000374); on `WritePng` failure it logs an error but still terminates with **exit code 0**. | `OnShutdown` join is dead code on this path; CI/automation sees success on a failed capture. | Plumb a success bool → `TerminateProcess(..., success?0:1)`. |
| L10 | low | B | `diag_hooks.cpp:165` vs `:166` | Comment says singleton ptr `0x83B7AA10`; code reads `0x83B3AA10` (B7 vs B3). Probe is `g_vp6_probe=false` (dead). | Harmless now; wastes time if the probe is revived. | Reconcile the two addresses. |
| L11 | low | IJ | `tier1-backend-architecture.md` "PARITY VERIFIED 45 dB" | The mechanism claims hold (SetExternalPipeline 2271-2274, kQuadList→LINELIST_ADJ 978, color_exp_bias 1542-1549 all present), but "45 dB VERIFIED" is a single un-reproduced artifact diff against a reference the same note calls "bogus." | Over-confident label on a one-shot metric. | Reword to "single-artifact 45 dB, not a reproducible harness." |
| L12 | low | IJ | `beta-depth-buffer-status.md` "Depth works." | Implementation is real (`EnsureBetaDepthTarget` 448, DSV/clear/bind), but the same note admits sorting "isn't visually proven," depth-on≈off (0.2%), depth+MSAA unreconciled. | Headline overreaches its own caveats. | Soften headline to "depth write+compare reach HW; visual sorting unproven." |

---

## 2. `NHL_BETA_*` env-diagnostic inventory — load-bearing vs cruft

All env reads live in `nhl_command_processor.cpp` (`diag_hooks.cpp` and the header have none).
There is no cvar/GetEnv abstraction — everything is raw `std::getenv`. **KEEP-mandated** items are
required by the active green-band instrument (`docs/edram-readback-instrument-prompt.md`) and must not
be removed.

### KEEP (load-bearing — active investigation + core selectors)
`NHL_BETA_RESOLVE_DIAG`, `NHL_BETA_FORCE_SAMPLE`, `NHL_BETA_SKIP_RANGE`, `NHL_BETA_EDRAM_DIAG`,
all `NHL_BETA_GPUDUMP*` (LINEAR/ALPHA/W/H) — **mandated**.
Closely-coupled active aids: `NHL_BETA_RT_PATH` (selects the `rov` green-band repro),
`NHL_BETA_SKIP_RESOLVE`, `NHL_BETA_ZEN_FILTER` (2D/3D isolation), `NHL_BETA_CAPTURE_FRAME` (lands the
scene_02 f30 repro), `NHL_BETA_WATCH_ADDR` (trace-gap diag), `NHL_BETA_DEPTH` (functional feature).
Core selectors (not diagnostics): `NHL_BACKEND`, `NHL_BETA_TAKEOVER`, `NHL_BETA_EDRAM`,
`NHL_BETA_D3D12_DEBUG`. Capture/replay tooling: `NHL_REPLAY_XTR`, `NHL_CAPTURE_*`,
`NHL_HOTKEY_CAPTURE`, `NHL_DRAW_INVENTORY`, `NHL_SHOT_*`, `NHL_DUMP_*`.

### RETIRE (dead / superseded / proven-not-a-fix — none in the active chain)
`NHL_BETA_ROV_ZERO_SIGNS` (proven NOT a fix — zeroing removes the player), `NHL_BETA_WINOFF`
(stale fold-viewport prototype, "not active in these runs"), `NHL_BETA_VP_FULLRT` (stale viewport
hack), **`NHL_BETA_DRAWPROBE` (also R1 — default-path device-loss risk)**, `NHL_BETA_DEPTH_FORCE_NEVER`
/ `NHL_BETA_DEPTH_FORCE_ALWAYS` (depth-rejection ruled out), `NHL_BETA_NOBLEND` (ROV blend ruled out;
also carries bug B1), `NHL_BETA_NOCULL`, `NHL_BETA_NOALPHA`, `NHL_BETA_NOTEX`, `NHL_BETA_NOSAMP`,
`NHL_BETA_NOSHMEM` (tex/samp/shmem default-on & hardened — the off paths are dead), `NHL_BETA_FAKETEX`
(bring-up), `NHL_BETA_SSAA`, `NHL_BETA_MSAA` (no-op for the menu per the architecture memory),
`NHL_BETA_NO_RESEED` (reseed ruled out as green source), `NHL_BETA_ZFAR`/`_BASE`/`_COUNT` (non-faithful,
carries bug B5), `NHL_BETA_NODRAW` / `NHL_BETA_TEXONLY` (superseded by SKIP_RANGE).

### CONSOLIDATE (overlapping families — collapse, don't delete blindly)
- **Diag logging** → one `NHL_BETA_DIAG` (or bitmask): `NHL_BETA_BIND_DIAG`, `NHL_BETA_INJECT_DIAG`,
  `NHL_BETA_PROBE_ADDR`, `NHL_BETA_DEPTH_DIAG`, `NHL_BETA_CLEAR_DIAG`.
- **RenderDoc** → one flag w/ `=oracle`: `NHL_BETA_RENDERDOC`, `NHL_BETA_RDOC_ORACLE`.
- **Draw filtering/bisection** → keep `NHL_BETA_SKIP_RANGE`, fold in `NHL_BETA_ONLY_DRAW`,
  `NHL_BETA_MAX_DRAW`.
- **Injection family** (out of scope for the fold-color lead): `NHL_BETA_INJECT*`,
  `NHL_INJECT_REGISTRY_*`, `NHL_BETA_INJECT_ROOT/SIDECAR/ALLOW_LINEAR/SWAP16` — one documented toggle.
- **Depth config**: `NHL_BETA_DEPTH_CLEAR` into a depth-config struct with `NHL_BETA_DEPTH`.

### Cross-cutting recommendation
Cache **all** `NHL_BETA_*` reads once into a `BetaDiagConfig` populated in `SetupContext` (right after
`beta_takeover_` is set, ~line 2929) and replace every per-draw `getenv` with a struct field. This
removes the L2 hot-path concern entirely **without behavior change** and is the highest-value cleanup.

---

## 3. Documented-judgment audit

Verdicts: HOLDS / OVER-STATED / WRONG / STALE-REFERENCE. Cross-checked against current source.

| claim (source) | verdict | evidence |
|---|---|---|
| "faithful EDRAM fold/addressing objective is **complete**" + ownership "Complete" (`agent-coordination-handoff.md`, Codex 2026-06-09) | **OVER-STATED / unreconciled** | Position fix plausibly real (`NHL_BETA_RT_PATH=rov`, `BetaResolveEdram` exist), but the later oracle-confirmed green COLOR defect is open and the active brief still calls the fold "the hard task." Not marked superseded. → **B6**; corrected below. |
| `NHL_BETA_TEX` opt-in, default OFF (memory `tier1-backend-architecture.md`, older entries) | **STALE-REFERENCE** | No such flag in code. `cpp:1875` `beta_tex = getenv("NHL_BETA_NOTEX")==nullptr` — default ON, opt-OUT. → **B15**; corrected below. |
| green band is **RENDER-SIDE** EDRAM tile ownership (memory `rov-green-player-is-fold-color.md`) | **OVER-STATED** | Handoff UPDATE 3 holds the opposite resolve-side lead and says log-only instrumentation is exhausted; the deciding `NHL_BETA_EDRAM_DUMP` instrument was **never built** (grep nil). Well-supported half (fold COLOR defect at x=640, red byte-identical, not missing materials) HOLDS. → **B16**. |
| "PARITY VERIFIED 45 dB" (memory `tier1-backend-architecture.md`) | **OVER-STATED** | Mechanism code all present (SetExternalPipeline 2271, LINELIST_ADJ 978, color_exp_bias 1542); the *number* is one unreproduced artifact vs a self-described "bogus" reference. → **L11**. |
| "Depth works / IMPLEMENTED + functional" (memory `beta-depth-buffer-status.md`) | **HOLDS (impl) / headline OVER-STATED** | `EnsureBetaDepthTarget` (448), DSV/clear/bind real; same note admits sorting unproven, depth-on≈off, depth+MSAA unreconciled. → **L12**. |
| injection_registry Stage-1: FNV-1a 64 over 4096-byte prefix, all-zero rejected, sidecar JSON (memory `beta-injection-stage1-sidecar.md`) | **HOLDS** | `injection_registry.h:55` `kHashPrefix=4096`; `.cpp:26-35` FNV-1a 64 (offset 1469598103934665603, prime 1099511628211); all-zero rejected on register (70) and lookup (122); `WriteSidecar`/`ParseSidecar` present. (Parser has bug B7/B8, but the *described facts* are accurate.) |
| `NHL_BETA_MSAA` one-shot ResolveSubresource; minimal recipe "just works" (memory) | **HOLDS** | `ResolveSubresource` at 2693; `NHL_BETA_TAKEOVER` 2928; tex/samp default-on consistent with B15 correction. |
| scene04-projection / faithful-fold cited flags & line numbers | **HOLDS (flags) / STALE line numbers** | Flags exist (`NHL_BETA_WINOFF` 1367, `VP_FULLRT` 1393, `SKIP_RESOLVE` 3411). Doc line cites drift (`RenderBetaOwnedDraw` "960"→984, `beta_edram_enabled_` "2666"→2932, `BetaResolveEdram` "3088"→3354) — docs self-label these "hints, not gospel." `NHL_BETA_VP_VPORT`/"WIDE-RT" have no code presence (reverted, consistent). |

**Corrections recorded this pass** (history preserved, not rewritten):
- A "superseded/correction" note appended to `docs/agent-coordination-handoff.md` for the "objective is
  complete" row (B6).
- A correction note appended to memory `tier1-backend-architecture.md` for `NHL_BETA_TEX` → `NHL_BETA_NOTEX` (B15).

---

## 4. Quick wins (safe, isolated — described, NOT applied)

1. **B1** — `0x2201 + i` → `kBlendControlRegisters[i]` at `cpp:1171-1172` (+ restore 1283-1285). One-line, the correct array is already in scope. *(diag-only, but a provable bug.)*
2. **B3** — add `beta_shared_memory_->UseForReading();` after the `RecordBetaGpuDumps` loop (`cpp:2604`), mirroring 1844.
3. **L5** — delete dead members `beta_view_bump_` / `beta_sampler_bump_` / `beta_srv_heap_` (`h:246-249`).
4. **L10** — reconcile the `0x83B7AA10` vs `0x83B3AA10` comment/code mismatch in `diag_hooks.cpp:165-166`.
5. **B14** — gate the "[loose-overlay] mounted" log on the lower-layer init result (`union_device.cpp`/`nhllegacy_app.h:202`).
6. **R1** — retire `NHL_BETA_DRAWPROBE` (default-path device-loss risk; purpose served).
7. **B11** — de-duplicate the three `kColorInfoRegisters` definitions into one `nhl_reg_080` block.

**Deliberately NOT a quick win** (needs design/validation, not a snip): B2 (the `0xCE8` raw-offset read
has no safe one-liner — needs a sanity check or an exported anchor), B4/B12 (untile correctness — must
be validated against a known surface), B7/B8 (sidecar parser — needs a real object-bounded rewrite),
B9 (per-frame flag refactor touches the whole `first_draw` contract).

---

## 5. Verified-CORRECT (checked, no finding) — so the next agent doesn't re-chase these

- Register save/restore in `RenderBetaOwnedDraw`: RB_SURFACE_INFO (1146→1282), RB_BLENDCONTROL0..3
  (1168-1173→1283-1285, modulo B1's wrong index), PA_SU_SC_MODE_CNTL (1181-1183→1286) all restored
  before the `if (!configured) return` at 1287 — no early return between save and restore.
- `NHL_BETA_FORCE_SAMPLE` 0x2318 override (3437→3450) restored unconditionally; the SKIP_RESOLVE
  early-return (3422) is before the save.
- The duplicate post-resolve `RangeWrittenByGpu` invalidation is already removed (3451-3452 comment);
  no sibling found.
- Map/Unmap pairing correct in `DumpBetaOffscreenTarget`, `DumpBetaSharedMemoryProbe`,
  `EnsureBetaFakeTexture`, `DumpBetaEdramSwap`, `WriteBetaGpuDumps`, `DumpBetaDepthStats`.
- `color_exp_bias` (1542-1549) = exp2(guest bias), correct; `kQuadList`→`LINELIST_ADJ` (978) correct;
  `SetExternalPipeline`/`SetExternalGraphicsRootSignature` ordering (2271-2274) correct.
- ROV EDRAM tile-pitch math (1559-1573) matches `xenos.h` `pitch_tiles`.
- injection_registry: all-zero rejection (42-47, applied at 70 & 122) sound; `rx2ffi_free` on every
  decode/replace return path (no leak); mutex discipline correct (`CorrelateRecord` drops the lock
  before re-acquiring); `MemFile`/`HostFile` EOF semantics correct.

---

*End of audit. No source files were modified. Editing was limited to this findings doc plus the
correction notes in `docs/agent-coordination-handoff.md` and memory `tier1-backend-architecture.md`.*
