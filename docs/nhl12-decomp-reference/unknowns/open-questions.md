# Open Questions — RE Backlog

Consolidated unknowns from every subsystem doc, with the evidence that would resolve
each. This is the reverse-engineering to-do list. Priorities reflect both blocking
value (does it unblock the recomp?) and documentation value.

## P0 — blocks reaching the first frame / playable build
1. **BigEB v3 entry-table layout + compression** — needed to write
   `tools/unpack_big.py` and populate `cache:\`. The current hard blocker.
   *Resolve by:* RE the TOC after the known header (`45 42 00 03` · count · `0x400` …);
   check for refpack. Start with `boot.big`/`audioboot.big`. See
   [`../assets/asset-pipeline.md`](../assets/asset-pipeline.md).
2. **Does mounting `cache:\cache.big` directly suffice** (game reads members) vs. needing
   the loose tree unpacked? *Resolve by:* try direct mount; watch `NtCreateFile` for
   loose-path opens after.
3. **The game's own cache-build trigger** (`FileCopier::Thread`/`AssetStream::Unpack`)
   — making it run would self-populate `cache:`. *Resolve by:* find + force the unpack
   path in the recompiled code.

## P1 — core simulation fidelity (validate vs. Xenia)
4. **Sim tick rate + integration order** — 30 or 60 Hz? *Resolve by:* pin the NHL Sim
   thread entry and read its timer/`dt`. See
   [`../architecture/main-loop.md`](../architecture/main-loop.md).
5. **`physics/physmodule` + `puck` integrator** (substeps, restitution, continuous
   collision). Highest-value to validate (small FP drift → wrong bounces). See
   [`../gameplay/puck-physics.md`](../gameplay/puck-physics.md).
6. **The sim↔render snapshot buffer** (the threading handoff + barriers) — correctness
   here prevents desync. See [`../architecture/threading.md`](../architecture/threading.md).
7. **Thread→hardware-thread affinity map** (the 6 HW threads). *Resolve by:* read each
   `ExCreateThread` arg set.
8. **Determinism check** — confirm `randomd0` is bit-exact post-translation.

## P2 — gameplay system internals (pin + read modules)
9. **`ai/aistruct` field layout** — the AI state struct (anchor for global game state).
10. **State lists** inside `checkingstatemachine`, the goalie state machine (`goalie.cpp`),
    and `savespace`'s net-coverage representation.
11. **Missing modules not yet surfaced by name:** defensive strategy (`stratdef`?),
    shot/pass launch-vector selection, HUD/scorebug, camera. *Resolve by:* more string
    mining + address pinning.
12. **Rules specifics** — touch vs. no-touch icing; penalty duration/severity tables
    (likely data). See [`../gameplay/rules.md`](../gameplay/rules.md).
13. **Animation graph/blend-tree** in `animplayer`; IK location/rig; `faceposer.big`
    format. See [`../animation/animation-system.md`](../animation/animation-system.md).

## P3 — data formats (need the unpacker first)
14. **`aidata.big` AI/difficulty tuning** — the values behind AI behaviour, difficulty,
    sliders.
15. **Lynx parameter registry format** — the data-driven tunables (`Attributed`,
    `HardBlend`).
16. **`.vlt` "vault" hashing** — to read `AttribDB/renddb` (materials) and possibly saves.
17. **`.sbr`/`.csi` audio bank/cue formats**; commentary location (`nocache.big`?) +
    event→line mapping; mixer/reverb state lists.
18. **`.rx2` RenderWare chunk format** (textures/skeletons) internals.
19. **NIS cinematic format** (`nis.big`) + how facial anim binds.

## P4 — frontend / modes / persistence
20. **Menu/screen state machine** (`resourcekernel` + `scenedef.lua` Lua flow).
21. **String-table / localization format** (7 locales).
22. **Full mode list** + data models (GM/franchise `gmmodedata`, Be A Pro?).
23. **Save formats** — profile/settings, rosters (`data0.big`?), franchise; EA content
    signing (`XeCrypt*`) expectations. See
    [`../save-load/serialization.md`](../save-load/serialization.md).

## P5 — graphics depth (after first frame)
24. **Actual render pass list/order** (pin `nhlrender` submission).
25. **Lighting model + post chain**; crowd rendering technique.
26. **Equipment texture model end-to-end** (cube vs 2D binding, intended composition) —
    the blue-bug root. See [`../graphics/shaders-materials.md`](../graphics/shaders-materials.md).
27. **Green intro-video bug** (DXT1, GPU-side) — paused. See
    [`../nhl12_vp6_green_video_fix_plan.md`](../nhl12_vp6_green_video_fix_plan.md).

## P6 — lifecycle
28. **Shutdown/quit sequence** — never exercised (only failure-to-start seen). See
    [`../architecture/overview.md`](../architecture/overview.md) §6.

---
### Method reminders
- Pin a module → addresses: [`../maps/function-index.md`](../maps/function-index.md) §3.
- Find globals: [`../maps/global-state.md`](../maps/global-state.md) §5.
- Mine more strings: `docs-build_strings.txt` (100,091 unique) + `tools/mine_srcpaths.py`.
- Validate behaviour against the **Xenia** reference build (the oracle).
