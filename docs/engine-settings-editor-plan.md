# Engine settings / tunables — inventory & editor plan

**Goal:** identify every internal setting/tweak the recompiled NHL Legacy engine
exposes (database size, equipment counts, physics, animation, overtime/rules,
AI, etc.) and build editors for them. Decided 2026-06-17.

**Editor end-state (user decision):** *both* surfaces —
- an **in-game overlay** (extend the existing ImGui overlay on the Vulkan-fsi
  path) for live tuning via `REX_STORE`, and
- the **nhl-database-studio** desktop app for authoring/persisting values.

**First build step (done):** the *tunable enumerator* — turn the speculative
physics/rules/animation list into a real, confirmed catalog before any editor
work. See "Tunable enumerator" below.

---

## The two worlds of settings

Settings live in two fundamentally different places that need different editors.

### World A — data-driven tables (`nhlng.db`) — schema fully known

Parsed today by the sibling `nhl-database-studio` `tdb-*` crates against
`nhl-database-studio/database/nhlng-meta.xml`. Covers **database size** and
**equipment counts** concretely. Caps come from the schema bit-widths:

| Group | Table(s) | Caps |
|---|---|---|
| Skater equipment selection | `exhibition/leagueskaterequipment` | stick/gloves/skates 0–127 (7-bit), helmet 0–255 (8-bit), tape 0–15, visor type 0–3, visor colour 0–15, stick curve/flex 0–3 |
| Goalie equipment | `exhibition/leaguegoalieequipment` | stick/blocker/trapper/pads 0–127, mask 0–511 (9-bit) |
| Team equipment + 9 colour zones | `stock/custom/leaguecustom teamequipmenttable` | equipment type 0–14, brand 0–254, variant 0–30 |
| Jerseys | `stockteamjerseys` etc. | 32 variants/team (5-bit) |
| Helmet placement | `stockplayerhelmetplacement` | 600 rows |
| Row caps | per table | league skaters 6000, league goalies 1200, exhibition skaters 1500, exhibition goalies 300; global u16 65535 ceiling |
| Stick assets on disk | `createplayer/sticks/stickN.big` | 106 present of the 128 catalog slots |

**Status (corrected 2026-06-17 by reading the crates):** write-back is **already
implemented and end-to-end wired**, not missing — the earlier review agent was
wrong. `tdb-core::write_tdb` (serialize.rs) reseals all four CRC scopes and is
byte-exact for unedited files; `tdb-edit::apply_to_file_bytes` (session.rs:1184)
handles header/cap edits (`current_records` up to a table's `max_records`), row
append/delete (swap-pop), per-cell numeric/inline-string/bytes writes, and
type-66 varchar pool rebuild; `tdb-gui-api::save_project` (commands.rs:655) +
`save_all` write to disk with backup, dry-run, validation preflight, and an
SHA external-edit guard. Round-trip + save tests live in `tdb-edit/tests/edit.rs`
and `tdb-gui-api/tests/commands.rs`.

So editing equipment **field values / colors / attributes** in existing rows and
**appending rows up to a table's built-in cap** works today. The genuine
remaining gaps are narrow and documented as deferred phases:
- Huffman tree rebuild for *new* glyphs in type-66 string edits (fails loud).
- **Raising a table's capacity** beyond its allocated `max_records` (table
  relocation / `db_size` growth) — that's `tdb-expand`'s separate path
  ([[db-expansion-ceilings]], [[sibling-db-editor]]), not `write_tdb`'s.

The equipment **brand→model visibility gate** is *not*
in the DB — it's in frontend code / a compiled ion screen
(`cpstickpickerpanel.big`); see [[editplayer-equipment-enumeration]].

### World B — engine constants baked into the recompiled image

Live in the mapped guest image at `0x82000000` (size `0x1EA0000`), accessed via
`REX_LOAD_U32` / `REX_STORE_U32` ([nhllegacy_init.h](../generated/default/nhllegacy_init.h#L103-L115)).
PPC is big-endian; every read byte-swaps.

How the engine exposes named tunables (confirmed on *our* binary, not just the
NHL12 sibling): EA's in-engine "tweak editor" registers its schema from a
packed `.rodata` **registration pool** — `category-path → gXxx name →
(optional default|label) → terminator`. Categories use `/`
(`AI/Shooting/Desperation`, `Physics/Ragdoll/Settings`, `Animation/...`); names
match `g[A-Z]\w*`. Documented in
`nhl-database-studio/docs/eboot/tuning-registration-pool.md`; the PS3 EBOOT pool
has **8,861 records** spanning `AI/`, `Physics/`, `Animation/`, `AI/Goalie/`,
`AI/Fighting/`, etc.

Two caveats that shape the work:
1. **Address won't transfer.** The pool anchor (`0x01c16780`) is from the PS3
   NHL14 EBOOT. Our 360 `default.xex` differs and is LZX-compressed (a raw
   `strings` pass finds nothing). We must *locate* the pool by record density in
   the booted, decompressed image — which the enumerator does.
2. **Defaults are sparse in the pool.** Most tunables resolve their default at
   runtime via per-tunable registration call sites, not an inline table. The
   recomp's advantage over a static EBOOT: it has the **live booted image
   in-process**, so we can read real current values from the runtime tweak
   registry.

`gCardOverallWeights_*` (HUT card-overall weights) is a *separate* pool and is
already located + dumpable — see [overall_weights_dump.h](../src/overall_weights_dump.h)
and [[overall-formula-harness]].

#### Honest confidence on the named categories

The specific physics/skating/collision/animation/rules tunable *names* the
review surfaced (`gPuck*`, `gSkate*`, restitution, etc.) are **extrapolated from
the 4-year-older NHL12 sibling reference**, and none of their addresses are
confirmed on our binary. The confirmed facts are (a) the *mechanism* above and
(b) exactly one located group so far (`gCardOverallWeights_*`). The enumerator
exists to replace that extrapolation with the binary's actual catalog.

---

## Tunable enumerator (built — World B discovery)

`src/tunable_registry_dump.h` — a C++ port of
`nhl-database-studio/crates/tdb-eboot/src/tuning_pool.rs`, adapted to walk our
**live mapped image** and locate the pool(s) by density instead of a hard-coded
offset. Wired into [nhllegacy_app.h](../src/nhllegacy_app.h) via two env vars
(mirrors the `NHL_DUMP_OVERALL_WEIGHTS` fast-exit discipline):

- **`NHL_DUMP_TUNABLES=1`** — static pass. Enumerates the full `gXxx` catalog
  (category, name, label, any inline default) from the mapped image and writes
  `tunable_registry.txt` + `tunable_registry.json` next to the exe, then exits
  without launching the game. **This is "the inventory."**
- **`NHL_DUMP_TUNABLES_RUNTIME=1`** — runtime pass. Boots the guest, waits
  (`NHL_DUMP_DELAY_MS`, default 12000), then scans committed guest memory for
  the materialised name→value records and records each tunable's live value to
  `tunable_values_runtime.txt` + `.json`. Value-field offset is registry-layout
  dependent and reported as a candidate (to be confirmed against a known value).

How to run (after a build):
```pwsh
# static catalog
$env:NHL_DUMP_TUNABLES=1; & <path-to>\nhllegacy.exe; Remove-Item Env:NHL_DUMP_TUNABLES
# live values
$env:NHL_DUMP_TUNABLES_RUNTIME=1; & <path-to>\nhllegacy.exe
```

Outputs feed both editors: the JSON catalog is the schema the studio/overlay
build their UI from; `name_va`/`default_va`/`record_va` are the patch sites.

---

## Runtime-dump findings (2026-06-17) — these reshape the editor plan

The static + runtime passes have been run (12,098 tunables; outputs:
`docs/tunable_values_runtime.{txt,json}`, `out/build/win-amd64-vk/tunable_registry.{txt,json}`).
Reviewing them establishes:

1. **Value-field offset is pinned at `+8`.** 11,121 of 11,158 resolved values
   (99.7%) landed at `field+8`; the few +4/+12/+20 are heuristic drift where the
   `+8` slot was zero/pointer-looking. The runtime tweak registry is a flat
   fixed-stride record array: `record_va+0` = BE pointer to the `gXxx` name,
   `record_va+8` = the live 32-bit value (**the patch site**). This answers the
   former step 3. The 940 "value field not identified" are genuine zeros (the
   heuristic skips 0) or non-scalar slots, not unresolvable.
2. **`name`/`name_va` is the only reliable join key.** Category (non-empty on
   11,826) propagates statefully — `AI` "owns" 10,362 because one real `AI/...`
   string sticks to every following record until the next slash-string; and
   path-like strings (`rendering/watermark/...fsh`, `.big`) get misclassified as
   categories. Labels (5,479) drift by one field (e.g. `gSpeedForMaxAccel` →
   `"LateralFriction"`). **Category/label are best-effort decoration; never the
   data model.** Real grouping = `gXxx` name-prefix.
3. **No type info exists** in the dump — everything is raw 32-bit. Type must be
   inferred (name + bits).

### Decisions (2026-06-17)
- **Typing:** auto-infer float/int/bool in Phase 0 (name + value-bits), editors
  show typed widgets with a per-row **raw-hex override** escape hatch.
- **Scope:** expose **all** 12,098 behind a search/filter + category tree.
- **Persistence:** a **dedicated `nhl_tunables.json`** (name-keyed
  `name → value`), applied at boot; `nhl_enhancements.ini` stays for the small
  typed cvars. Both surfaces read/write this one file.

### Shared architecture
Everything keys on `name`. The studio can't know runtime addresses, so the
override file is `name → value`. The recomp, at boot **after tweak registration**
(same timing as the runtime dump), resolves `name → record_va` and writes
`record_va+8` via `REX_STORE_U32` (BE swap handled by the macro,
[nhllegacy_init.h](../generated/default/nhllegacy_init.h#L114)). The overlay does
the identical name→record_va resolution once, then writes live on each edit.

## Plan

**Track B (engine constants) — in progress**
1. ✅ Build the enumerator (`tunable_registry_dump.h`).
2. ✅ User ran the static + runtime passes (4 dump files produced).
3. ✅ Value-field offset pinned at `+8` from the runtime dump distribution.
4. **Phase 0 — harden dump into a schema:** refine `DumpTunableValuesRuntime`
   ([src/tunable_registry_dump.h](../src/tunable_registry_dump.h#L383)) to read
   `record_va+8` unconditionally (emit value even when 0, drop the multi-offset
   walk); add an inferred `type_hint` column; emit a merged
   `tunables_catalog.json` (name, name_va, category, label, type_hint,
   value_bits/f32) checked into the repo; add a category-normalization pass
   (reject path-like categories, fall back to name-prefix grouping).
5. **Phase 1 — overlay: ✅ BUILT (syntax-checked clean, live test pending).**
   - `ITunableStore` interface ([renderer/core/nhl_tunable_store.h](../renderer/core/nhl_tunable_store.h)),
     backend-free (mirrors `PadState`/`PerfSnapshot`).
   - `TunableRuntimeStore` ([src/tunable_runtime.h](../src/tunable_runtime.h)):
     reuses `EnumerateTunables` + the memory scan to build a live `name →
     {record_va, value_va, captured_bits}` index on a **worker thread** (the scan
     is only valid post-registration); BE read/write at `value_va`
     (= record_va+8); loads/applies/saves `nhl_tunables.json`.
   - Overlay "Engine Tunables" section ([renderer/core/nhl_overlay.cpp](../renderer/core/nhl_overlay.cpp)):
     lazy "Build index" button → background scan → filterable flat list
     (`ImGuiListClipper`, name filter + group combo + "only overridden"),
     type-aware widgets (float/int/bool) + global **Raw-hex** toggle,
     per-row reset-to-stock, Save / Revert-all, stock-value tooltip.
   - App wiring ([src/nhllegacy_app.h](../src/nhllegacy_app.h)): store created in
     `OnCreateDialogs`, passed to the dialog; if `nhl_tunables.json` exists,
     a delayed (`NHL_DUMP_DELAY_MS`, default 12 s) worker re-applies overrides at
     boot even if the overlay is never opened.
   - **Caveat surfaced in-UI:** some `gXxx` are runtime *state* the engine
     rewrites each frame, so a live write may not stick — power-user tool.
   - **NEXT (user):** live controller/keyboard test — open overlay (Guide/F1) →
     Engine Tunables → Build index → confirm a known value (e.g.
     `gDekeRatEffect` ≈ 0.2) and that an edit takes effect + persists across a
     relaunch.
6. **Studio:** new `tdb-tunables` crate / panel reading `tunables_catalog.json`,
   type-aware grid + filter + baseline diff, output the same name-keyed
   `nhl_tunables.json`. Mirrors World A (studio authors a file, game applies it).
   Optional later: tuning profiles (sim/arcade presets).

**Track A (data tables) — largely done**
1. ✅ `tdb-edit` write-back exists + is wired through `save_project`/`save_all`.
2. ⏳ Verify the round-trip/save tests pass (`cargo test -p tdb-core -p tdb-edit`)
   — blocked this session by a tooling outage.
3. Optional next: a focused round-trip test that edits a real `nhlng.db`
   equipment field, saves, reloads, asserts persistence.
4. Optional: wire `tdb-expand` (capacity growth) into the editor UI so equipment
   *counts* can exceed the built-in caps, not just be reassigned.

---

## Key files

- Enumerator: [src/tunable_registry_dump.h](../src/tunable_registry_dump.h), wired in [src/nhllegacy_app.h](../src/nhllegacy_app.h)
- Pool spec: `nhl-database-studio/docs/eboot/tuning-registration-pool.md`
- Reference parser: `nhl-database-studio/crates/tdb-eboot/src/tuning_pool.rs`
- DB schema: `nhl-database-studio/database/nhlng-meta.xml`
- Worked-example dumper: [src/overall_weights_dump.h](../src/overall_weights_dump.h)
- Memory macros: [generated/default/nhllegacy_init.h](../generated/default/nhllegacy_init.h#L103-L115)
- Overlay/settings: [renderer/core/nhl_settings.h](../renderer/core/nhl_settings.h)
