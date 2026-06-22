# Edit-Player equipment enumeration (sticks first)

**Status:** Phase 1 — understand + document. Static map complete; runtime confirmation of the
visibility gate is **pending a live capture** (see [§5](#5-runtime-capture-procedure)).
**Scope:** player hockey sticks, with a method that generalizes to gloves/skates/helmets/visors.

## 0. TL;DR

- The Create/Edit Player ("pro shop") equipment system is **brand → model, index-based**.
  `nhlng.db` stores only the player's *chosen index* per slot; it has **no catalog table** that
  lists which models exist or are selectable.
- The selectable catalog is keyed by the numbered **UI thumbnail** asset set
  `fe\ion\artassets\createplayer\sticks\stickN.big` (N = 1..112, 6 gaps → **106 present**).
- Which of those the picker actually shows is decided by the **frontend** (compiled ion screen
  `cpstickpickerpanel.big` and/or opaque recomp `sub_*` code) — **not** by the DB or
  `equipatrib.bin`. That gate is confirmed empirically with the `NHL_LOG_ASSET_OPENS`
  diagnostic added for this investigation.
- Two concrete "exists but may not be selectable" candidates fall straight out of the static
  inventory: **stick 96 and 97 have a render graphic but no UI thumbnail** (nothing for the
  picker to display).

## 1. Data model — where each piece lives

### Per-player selection (the only DB involvement)
`nhlng.db` (schema `db\nhlng-meta.xml`), table **`exhibitionskaterequipment`** (shortname
`ajmx`, maxinsert 1500). Each row is one skater; equipment fields store an *index*, and the
field bit-width is the hard ceiling on that index:

| field | bits | range | meaning |
|---|---|---|---|
| `stick` | 7 | 0..127 | stick model index |
| `stickcurve` | 2 | 0..3 | curve |
| `stickflex` | 2 | 0..3 | flex |
| `sticktapecolor` | 4 | 0..15 | tape color |
| `gloves` | 7 | 0..127 | glove model |
| `skatebrand` | 7 | 0..127 | skate model |
| `helmetbrand` | 8 | 0..255 | helmet model |
| `pantsbrand` | 3 | 0..7 | pants |
| `visortype` | 2 | 0..3 | visor |

League/stock/custom variants of the same data: `leagueskaterequipment`,
`stockteamequipmenttable`, `customteamequipmenttable`. The `*zoneN_rgb` columns there and in
`exhibitiongoalieequipment` are **recolor zones (color), not a model catalog** — separate
concern from "which models appear".

**Takeaway:** the DB caps the *index* at 0..127 but does not enumerate or gate the catalog. The
schema would only become a limiter if we needed > 128 sticks (it does not — there are 106).

### Stick assets on disk (two parallel, number-keyed sets)
- **3D render** — `rendering\playerstick\`: a few base meshes (`playerstick_0_2/0_4/1_2/1_4`)
  + recolorable brand graphics `texlib_18..112` (74 of them) + `texlib_999` fallback.
- **UI thumbnails** — `fe\ion\artassets\createplayer\sticks\stickN.big`: **N = 1..112 with gaps
  at 10, 11, 14, 80, 96, 97 → 106 present**. This numbered set is the de-facto catalog key.
- **Brands** — `fe\ion\artassets\createplayer\brands\brand0..19` (~19 brand logos/headers).

### UI screens (compiled ion, no readable strings)
`fe\ion\game\screens\createplayer\`: `cpstickpickerpanel.big` (73 KB),
`cpskatepickerpanel.big`, `cpequipmentimagepickerpanel.big`, `cpgenericimagepickerpanel.big`.

### `fe\equipatrib.bin` — NOT the gate
598-byte equipment **attribute-boost** table: 14 attribute codes `BDEGHJKMNOPQRT` followed by
repeating 7-tier boost magnitudes (`ZZZZZZZ`, `AAAAAAA`, `2222222`, `UUUUUUU`, …, values on a
0..100 scale). It is keyed by quality tier × attribute, i.e. *how much stat boost a piece of
gear gives* — **not** a model count and **not** a visibility filter. Ruled out as the gate.

## 2. Static cross-tabulation: UI thumbnail vs render graphic

(106 UI thumbnails vs 74 `texlib_N` render graphics)

- **34 IDs have a UI thumbnail but no `texlib_N`** (1–9, 12, 13, 15–17, 19–23, 27–29, 38,
  42–44, 57–59, 61, 62, 66, 69, 76). These low/early IDs are most likely the licensed "pro"
  sticks whose render graphic comes from a dedicated base texture (`playerstick_0_*`) or the
  `texlib_999` fallback rather than the generic `texlib` pool.
- **2 IDs have a render graphic but no UI thumbnail: 96 and 97.** With no `stickN.big` to draw,
  the image-picker has nothing to show — prime "in the files but not selectable" candidates.
- UI numbering gaps (10, 11, 14, 80, 96, 97) imply the catalog was curated; a frontend
  brand→model list almost certainly drives which numbers are live.

## 3. The gate is in the frontend (why a runtime probe is needed)

The recomp (`generated\`) is decompiled to opaque `sub_XXXX` functions and loads resources by
hashed name, so the list-population logic is **not statically searchable by symbol**. The ion
screen is compiled binary with no readable strings. Therefore the authoritative answer to "how
many sticks show and which" is observed at runtime by logging which `stickN.big` the picker
opens.

**Leading hypotheses** (to disambiguate from the capture):
1. **Brand→model map omission** — the frontend lists fewer (brand, model) pairs than there are
   `stickN.big`, so unmapped assets (e.g. 96/97, or any with no brand owner) never appear.
2. **Hardcoded count** — a fixed cap in the create-player component truncates the list.
3. **Render-only gap, not a menu gap** — the picker enumerates all 106 and the "missing gear"
   the user noticed is actually the render-side `texlib` fallback on the 34 thumbnail-only IDs.

## 4. Diagnostic added for this investigation

`NHL_LOG_ASSET_OPENS` (env-gated) in
[`src/loose_tree_device.cpp`](../../src/loose_tree_device.cpp) → `LooseTreeEntry::Open()`.
When set, every loose asset the guest opens is logged as
`[asset-open] <logical cache: path> (<bytes>)`. Walking the stick picker emits one line per
requested `fe\ion\artassets\createplayer\sticks\stickN.big`, so **the set of N logged is the
selectable catalog**. Default path logs nothing. Compiles clean against `win-amd64-vk` (only
the pre-existing `getenv`-deprecation warning, matching `NHL_INJECT_CAPTURE`).

**Serving requirement:** the seam fires only for files served from the loose `_compiled` tree
(mounted under `cache:` by the `UnionDevice` in `nhllegacy_app.h` `OnPostSetup`). The picker
assets must therefore exist in `game_data_root\_compiled\fe\ion\artassets\createplayer\sticks\`.
Verified present (106 files) in the `NHL Legacy - Vanilla\_compiled` tree — run with a
`game_data_root` whose `_compiled` contains them. (If a future run serves them only from the
`.big`, stage them loose or instrument the `.big` read path instead.)

## 5. Runtime capture procedure

1. Build the VK target (under a VS2022 `vcvars64` + LLVM-on-PATH shell):
   `ninja -C out\build\win-amd64-vk nhllegacy`
2. Launch with the env var set and a `game_data_root` whose `_compiled` has the createplayer
   assets, e.g. `tools\drive.ps1` with `NHL_LOG_ASSET_OPENS=1` (or set the env var, then run
   the produced exe).
3. In game: go to Create/Edit Player → equipment → **stick picker**, and scroll through
   **every brand and every model** (loads may be lazy — scroll the whole list).
4. Save the log. Grep it: `Select-String "createplayer.sticks" log.txt` for the stick IDs, and
   `createplayer.brands` for the brand walk.

## 6. Analysis — capture (`nhllegacy_055.log`, 2026-06-17, user-confirmed complete)

**Confirmed: the picker shows a fixed, curated subset; the gate is a hardcoded/data item
list, not the assets.** Render-only-gap is ruled out (the missing sticks render fine when
assigned to a player via the DB — user-confirmed).

**Mechanism (corrected):** the stick picker is a **single grid showing a fixed 23-stick set —
there is no brand-selection step** (user-confirmed: all shown in one grid; 23 sticks, 10 brand
logos). Thumbnails load lazily as the grid scrolls into view, and `brands_small\brandN.big`
loads are just the **brand logos labeling each stick**, not navigation. (An earlier reading of
this as brand-grouped navigation was wrong.)

**The complete shown set (23):** `35, 36, 82, 84, 85, 86, 87, 93, 94, 95, 98, 99, 102, 103,
104, 105, 106, 107, 108, 109, 110, 111, 112` — i.e. only `{35, 36}` plus the high range
**82–112**. Every stick `< 82` (except 35/36) is excluded. This looks like a "current-
generation / licensed" cut: the high IDs are the modern sticks, the low IDs are legacy/retro.

**Orphaned — on disk and fully functional, but NOT in the picker (83 of 106):**
`1–9, 12, 13, 15–34, 37–79, 81, 83, 88–92, 100, 101`. These are exactly the assets the user
wants surfaced.

So **making all sticks selectable = extend the 23-entry list to include the 83 orphans.**

### Key corollary (user-confirmed): assets are NOT the limit, enumeration is
The sticks that don't appear in the picker **still render fine when their ID is assigned to a
player** (via roster/DB). So every stick asset, the render path, and the DB `stick` index
(0..127) are fully functional — the *only* limiter is the picker's enumeration. "Make all gear
show" is therefore a pure front-end change: extend the picker's fixed item list. Nothing in
the assets, renderer, or DB schema needs to change.

### Where the 23-entry list lives (the edit surface — still open)
- **Disproved:** the `cpstickpickerpanel.big` BIGF table-of-contents. Its numeric entry names
  (0,1,11,14,…,49) are internal layout/frame element IDs, not model IDs — they include invalid
  stick IDs (0/11/14 are disk gaps) and omit the 82–112 sticks the runtime loaded under brands
  3/4/19. Same for `cpskatepickerpanel.big`.
- **Equipment data files actually consulted during create-player** (from the capture):
  `fe\equipatrib.bin` (598 B — attribute boosts), `attribdb\renddb.vlt`/`renddb.bin` (the
  render attribute DB, 161 KB / 104 KB), and `db\nhlng.db`. `renddb.vlt` is an opaque EA VLT
  (versioned hashed structs: `Vers`/`StrN`/`DepN` chunks, no plaintext field names) — not
  grep-able; decoding needs its schema.
- **Conclusion:** the fixed 23-entry stick list is held either in `renddb` (most likely — it's
  the render catalog) or as static data compiled into the recomp create-player component.
  Pinpointing it is the **edit surface** for surfacing the 83 orphans, and is a deeper RE task
  (VLT schema decode, or recomp `sub_*` tracing) than the file-open probe used so far.

## 7. Hunt for the 23-entry list (where to edit) — static search EXHAUSTED

Added a tool to enable offline search: **`NHL_DUMP_IMAGE`** (env-gated, in
[nhllegacy_app.h](../../src/nhllegacy_app.h) next to `NHL_DUMP_OVERALL_WEIGHTS`) writes the
decompressed guest image (`0x82000000`, `0x1EA0000` bytes) to `guest_image_dump.bin` and
fast-exits — same technique [overall_weights_dump.h](../../src/overall_weights_dump.h) uses to
reach static `.rodata` that the LZX-compressed `default.xex` hides. Verified: 32,112,640-byte
dump produced.

Searched the dump and every create-player data file for the 23-ID set, all **negative**:
- **Guest image (.rodata/.data/.text):** no clean u8/u16/u32 array of the 23 IDs; no
  stick→brand table indexed by stick id; the only all-in-set runs are constant-value noise
  (e.g. `102,102,…`) or ascending counters, and the densest windows are the ASCII glyph table
  (chars 33–126).
- **`cpstickpickerpanel.big`, `cpequipmentimagepickerpanel.big`, `cp4_editplayer.big`,
  `cp_imagepicker.big`:** the apparent ID runs are sequential ion element/property tables
  (e.g. `97,98,99,…` with `(21,16)` field tags) and ASCII names (`"compon…"`), not the catalog.
- **`equipatrib.bin`:** boosts only; shown IDs 86/87/94/102–112 appear nowhere in it.
- **`renddb`:** render-effects DB (string table = `high_plume`, `cloudSpray`, `board`…).
- **`nhlng.db`:** no equipment-model catalog table (only per-player/team index fields).

**Conclusion:** the fixed 23-stick selection is **not a static byte array** anywhere editable on
disk. It is almost certainly **built at runtime** by the create-player code — most likely each
stick is included based on a per-item attribute it evaluates live (the shown set is the high
range 82–112 plus 35/36, but *irregular* within it: 83/88–92/100/101 have thumbnails yet are
excluded — so not a simple range, an explicit per-item decision).

### Runtime scanner (BUILT): overlay → Developer Tools → "Scan stick list"
Implemented [src/stick_list_scan.h](../../src/stick_list_scan.h) (`ScanStickList`), mirroring
`DumpOverallWeightsRuntime`'s committed-page walk: it scans live guest memory for a compact
32-element window dense in **distinct** shown IDs (>=16/23) in u8/u16/u32 big-endian, reporting
each candidate's VA + values + contaminant count. The distinct-count filter is the key — it
rejects the constant-run / ascending-counter noise that defeated the static scan.

Wired into the in-game overlay under a new **"Developer Tools"** collapsing section
([nhl_overlay.cpp](../../renderer/core/nhl_overlay.cpp) `DrawDevTools`); the app
([nhllegacy_app.h](../../src/nhllegacy_app.h) `OnCreateDialogs`) resolves the guest membase at
click time and runs the scan on a detached thread, writing `stick_list_scan.txt` next to the
exe. **Usage: open Create/Edit Player → the stick picker FIRST, then open the overlay (Guide/F1)
→ Developer Tools → "Scan stick list".** Builds clean on `win-amd64-vk`.

If the scan pins the list, the candidate VA + count is the runtime edit surface (patch via a
hook). Fallback if it comes up empty (list not materialised as an array): trace the recomp
function that formats the `createplayer\sticks\stickN.big` path strings to find its loop bound +
source. The same scanner generalises to other slots by swapping the fingerprint ID set.

### Runtime scan RESULT (2026-06-17, stick_list_scan.txt) — NEGATIVE
Ran with the stick picker on-screen. Every dense window is a **sequential range** (76–107,
97–122, …) — counters / glyph tables that merely overlap the high end of the shown set
(`contaminants=16`, half each window is *non*-shown IDs). No window holds the irregular 23-set
(would be distinct=23, contaminants≈0). A follow-up search for an **availability table indexed
by stick ID** (shown→valid, orphan→sentinel) produced only trivial matches in empty/uniform
regions (zeros/0xFF that mean nothing).

**Conclusion (static + runtime, exhaustive):** the 23-stick set is **not stored as any indexed
array, ID list, or per-ID availability table** — anywhere on disk or in live memory. The picker
**computes** the set.

### Call-site trace (BUILT + RUN) → the producing code path
`NHL_TRACE_STICK_OPEN` ([src/stick_caller_trace.h](../../src/stick_caller_trace.h), wired in
`LooseTreeEntry::Open`) walks the host stack at each `stickN.big` open (the recomp compiles
guest funcs as host funcs, so host stack == guest call chain) and logs RVAs into `nhllegacy.exe`.
Symbolized with `llvm-symbolizer` against the build PDB (RVA + PE ImageBase `0x140000000`).
**Identical chain on all opens** (innermost → outermost):

```
sub_83068AE8 (EA file-open) ← sub_8276AF70 ← sub_827681E0 ← sub_82764080 ← sub_82767510 ← sub_826716E8
```

- `sub_8276AF70` (recomp.26.cpp:71293) is a generic **path-resolve/open helper**: it walks the
  path string in `r4` scanning for `/`(47) and `\`(92). The full `…\sticks\stickN.big` string —
  number included — is **already built by the caller**, so the per-stick ID and the enumeration
  live in `sub_827681E0` (recomp.26.cpp:64680, ~6.6k lines) and/or its caller `sub_82764080`
  (recomp.26.cpp:55070, ~7.7k lines). Those are the functions to instrument/read for the loop
  bound + the ID source.

### Control flow is framework-driven, not a simple guest loop
Inspecting the chain in source: `sub_82764080` calls `sub_827681E0` **once** (recomp.26.cpp:55121,
near its start — not a loop), and `sub_827681E0` contains **no direct call** to `sub_8276AF70` —
so that call is **indirect** (function pointer / vtable / registrar). The identical 23× stack
threads up through the `0x83……` region (`sub_8307D638/6B8`, `sub_83074128` — recomp
dispatch/runtime) and into the SDK thread. Read together: the **ion image-picker widget iterates
its cells and invokes a guest callback per cell** (indirect), rather than a guest `for` loop
walking a 23-entry list. That's consistent with every prior negative (no stored array): the set
is produced by a framework-driven per-cell callback whose source we'd have to trace through
indirect dispatch.

### Status / honest assessment
Understanding is complete and the exact code path is pinned. Going further (extract the per-cell
data source through indirect dispatch, then patch it) is **deep, multi-cycle recomp RE** —
instrumenting indirect-call sites, tracing the ion data binding — and the eventual fix would mean
patching regenerated `generated/` code or installing a runtime call hook (fragile). This is a
real effort step-change from the probes so far; worth an explicit decision before continuing.
Cheaper parallel lever for "more sticks" exists in the DB-cap thread ([[stick-count-expansion-past-128]]).

## 8. Generalizing to other equipment

The same diagnostic and reasoning apply to every slot: grep the capture for
`createplayer\gloves`, `\skates`, `\helmet`, `\visor`, `\pads`, etc. Each has its own numbered
UI thumbnail set under `fe\ion\artassets\createplayer\<slot>\` and a matching DB index field in
`exhibitionskaterequipment` (table in §1). Identify each slot's gate the same way before
attempting to surface its full asset set.
