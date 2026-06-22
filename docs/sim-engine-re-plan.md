# Sim Engine + Franchise Generation/Progression — RE plan

Scope: the season/franchise simulation engine (#5) plus the two potential-system
questions that gate roster realism over a multi-season franchise:

- **Q1 — generated draftee scaling vs the league.** If we make the league's rating
  spectrum harsher (lower the average player), do procedurally-generated draft
  prospects stay calibrated to the new spectrum, or do they drift to comparatively
  elite because their generation is absolute, not league-relative? We want to edit
  the spectrum generated players are drawn from.
- **Q2 — the growth spectrum per potential rating.** Where is it defined what range
  of attribute outcomes a player of a given potential tier reaches over a career?

## What is DATA (editable now in nhlng.db) vs CODE (needs RE)

Verified directly against `nhl-database-studio/database/nhlng-meta.xml`. The
per-player/per-pick *inputs* are all data; the *functions that consume them* are
code in `nhlfrontend/leaguelogic/gmmodedata.cpp` (translated but address-named).

### Data surface (edit via [[sibling-db-editor]] tdb-edit + loose-file overlay)

| Concern | Table(s) | Key fields |
|---|---|---|
| Potential ceilings | `leagueskaterai` / `leaguegoalieai` (+exhibition) | `c_potential`/`e_potential`/`s_potential` (0–63), `m_potential` (±15) |
| Growth curve inputs | same | `growthspeed` (−1..14), `growthletter` (−1..6 = A–F), `growthtier` (−1..14), `growthage` (peak age), `growthmod` (±15) |
| Draftee per-prospect tuning | `leaguerookiedata` (1500) | `scalepct` (0–127), `initialerrormargin` (±64 scouting noise), `isfranchise`, `allposplayingstyle`, `juniorleague`, `position` |
| Draft board | `leaguedraftpick` (1260) | `pickoverall`, `round`, `year`, `owner`, `origteamid`, `playerindex`→prospect |
| Name pool (generated players) | `playernames` (8191) | `name`, `bank` (0–15 nationality group), `isfirstname`, `audioid` |
| GM/sim AI constants | `leaguegmlogic` (300) | opaque `index`(0–511)/`value`(0–1023) pairs — meaning lives in code |
| Game-mode vars | `leaguegamemodesvars` (100) | typed `key`(0–1023)/`value`(±32767)/`type`/`team` store |
| Schedules | `nhlschedule` (60), `leaguescheduletable` (4000), `leaguetempscheduletable` (2700) | index/status/home/away/day/month/round |

### The c/e/s_potential triple is the key insight for Q2

`c_overall`/`e_overall`/`s_overall` and `c_potential`/`e_potential`/`s_potential`
are **current / elite / superstar** outcomes. These look like the *endpoints* of
the growth spectrum per player: the floor (current), the likely-good outcome
(elite), and the ceiling (superstar). The `growthletter`/`growthmod`/`growthspeed`
roll then determines where in [c .. s] a player lands and how fast. So **part of
the spectrum is per-player data** (you can already widen/narrow any individual's
c→s band), but **the function that maps the roll + age onto a per-season
trajectory is code** in gmmodedata. Confirm the c/e/s semantics by RE (below) or a
runtime experiment.

### What is NOT in the tunable registry (so it's code, not a runtime knob)

The 12,098-tunable dump (`docs/tunable_values_runtime.txt`) contains **no**
generation/progression tunables. Adjacent systems that DO surface:

- `AI/Ratings`: `gAttribCurveBase`, `gAttribCurveInflectionInput/Output`,
  `gAttribCurveInputMax/OutputMax`, `gPlayerAttribCurve`, `gGoalieAttribCurve`,
  `gUsePerAttribCurve` — the **stored-rating → effective-rating** curve. Relevant
  to the "harsher spectrum" goal (Q1) because it reshapes how a lowered rating
  spectrum plays out on the ice, but it is NOT the franchise progression curve.
- `AI/Beapro/Sim` + `AI/Beapro/SimOut`: `gBeAPro_Sim_*`, `gBeAPro_SimBlock_*` —
  these gate **Be-A-Pro live-game sim-out** (when to bail the human out of a game
  they're playing). They are NOT the "simulate a game I'm not playing → box score"
  engine. The franchise box-score generator is in gmmodedata.

## RE pipeline for #5 (and to answer Q1/Q2 definitively)

Functions are `sub_XXXXXXXX` only; the handle is EA's `__FILE__`/assert path
strings in .rodata. Pipeline:

1. **Dump the decompressed image** (one game run):
   `NHL_DUMP_IMAGE=1 <game exe>` → `guest_image_dump.bin` (base 0x82000000, ~32MB).
   (The LZX-compressed `assets/default.xex` is NOT searchable — confirmed: 0 hits.)
2. **Locate the module** (offline):
   `python tools/franchise_re/find_franchise_code.py guest_image_dump.bin \
       --generated generated/default --out docs/franchise_re_hits.txt`
   Stage 1 lists every franchise/sim string + its VA; stage 2 cross-refs each VA
   into the translated C++ to name the `sub_XXXX` assert sites = gmmodedata funcs.
3. **Walk the call graph** from those seed functions (callers/callees in the
   generated tree) to find: the rookie-generation routine (reads `playernames`,
   writes `leagueskaterai` ratings), the progression routine (reads growth* fields
   each sim-year), and the box-score generator (reads team/player ratings → score
   + stat lines). Use the DB table shortnames as anchors — code that touches
   `leaguerookiedata` (shortname `cpwZ`), `leaguegmlogic` (`xQwc`), etc. loads
   those 4-char tags.
4. **Answer Q1**: read the rookie-gen routine — does it sample ratings from an
   absolute distribution (constants/`leaguegmlogic`) or scale to current league
   stats? That single read resolves the relative-vs-absolute question.
5. **Answer Q2**: read the progression routine — recover the exact map from
   (growthletter, growthmod, growthspeed, growthage, c/e/s_potential) → per-season
   attribute delta. That IS the growth spectrum definition.

## Fast parallel runtime experiment (no RE needed, validates Q1)

Lower the league spectrum in nhlng.db (or via the `AI/Ratings` attribute curve),
sim ~10 seasons, then inspect the generated draft classes' rating distribution in
the save. If generated prospects track the lowered league → generation is
relative (good); if they cluster high → it's absolute and we must patch the
generation distribution (constants found in step 4). This can run in parallel with
the static RE and either confirms or refutes the concern empirically.

## Immediate code-free levers (partial, available today)

- Edit any individual's `c/e/s_potential` band + `growthletter`/`growthmod` to
  reshape that player's outcome spectrum.
- Edit the **pre-authored** draft classes directly: the first several franchise
  years draw from authored prospects in `leaguerookiedata` + their rating rows —
  fully editable. Only the *post-authored* (truly generated) classes need the code
  work above.
- `leaguegmlogic` (300) + `leaguegamemodesvars` (100) are editable now, but their
  index→meaning mapping requires step 4 to use safely.

## FRANCHISE ENGINE PINNED IN THE RECOMP (2026-06-18)

Image source: `H:/Emulators/games/XBOX/default.decompressed.exe` — a flat
decompressed image, exactly 0x1EA0000 bytes, VA = 0x82000000 + file_offset. This
replaces the need for a runtime `NHL_DUMP_IMAGE` dump; search it directly.

The LZX-compressed `assets/default.xex` yields 0 string hits — must use the
decompressed image above.

Franchise `leaguelogic` TUs, pinned by their `__FILE__` strings (full path is
`C:\p4\NHL\RL\nhl\fe\interface\nhlfrontend\dev\source\nhlfrontend\leaguelogic\…`):

| Module | `__FILE__` VA | Referenced by (seed func) |
|---|---|---|
| GmModeData.cpp (franchise core: schedule/standings/draft/trades/finance/gen/progression) | 0x82087BA0 | `sub_82B7A8D0` |
| NameGenerator.cpp (generated-player names) | 0x82089A70 | `sub_82B8BEE0` |
| InjuryGen.cpp | 0x82087CC0 | `sub_82B7BE58` |
| InjuryBrain.cpp | 0x82089930 | `sub_82B8AE70`, `sub_82B8EA38` |

Only 1–2 funcs per TU carry a surviving `__FILE__` ref (retail strips most
asserts), but they fix each TU's address neighbourhood. **All franchise leaguelogic
code lives in `generated/default/nhllegacy_recomp.60.cpp` (505 funcs,
sub_82B60848..sub_82B7B120) + `.61.cpp` (499 funcs, sub_82B82090..sub_82B9F540) —
i.e. the ~0x82B6xxxx–0x82B9Fxxx band.** A `leaguelogic::StatsBrain` class string is
also present (~VA 0x82082xxx).

Cross-ref method: addresses are emitted as DECIMAL `lis`/`addi`/`ori` pairs, not
inline hex — the locator tool reconstructs per-register address arithmetic to
attribute string VAs to functions. (Hex grep finds nothing; confirmed.)

NameGenerator seed `sub_82B8BEE0`: reads a category byte at `field+116` and
switch-branches on 77/74/67/69/72 (= 'M'/'J'/'C'/'E'/'H') — nationality/position
name-bank dispatch, consistent with selecting a `playernames.bank`.

## Decode findings (2026-06-18, option 1)

- **GM/franchise mode codename = "MUD"** (strings: `MUD_LEGACY_DRAFT_ORDER`,
  `MUD_DRAFT_PRIORITY`, `MUD_SELECT_TEAM_RANDOM`, `Mud min org {goalies,defense,
  forwards} keep from retire`, `TXT_TRADE_FAILED_MUD`). Search the image/code for
  "Mud"/"MUD" to find franchise routines. Online-seasons codename appears to be
  "Unicorn" (`Unicorn/Tune` settings category).
- **The code is OO with heavy vtable/indirect dispatch** — classes `HockeyTeam`,
  `StatsBrain`, `TradeBrain`, `TradeLogic`, `LegendTask`, `AccomplishmentDataManager`
  (from `Class::Method::Owner` debug strings). Static call-graph tracing hits
  `REX_CALL_INDIRECT_FUNC` walls (e.g. NameGenerator seed has NO direct callers).
  → runtime instrumentation (we now have addresses to hook) is the efficient way
  to nail input/output behaviour.
- **GmModeData seed `sub_82B7A8D0`** is a small accessor on a large franchise-state
  struct (member byte reads at +4386/+4387/+4481/+4482). The TU's algorithmic cores
  are the big functions in the band; biggest in files 60/61:
  `sub_82B6BA58` (3447 ln), `sub_82B9C078` (2918), `sub_82B906F8` (2646),
  `sub_82B74EE8` (2094), `sub_82B66EA8` (1707), `sub_82B741E8` (1686). These are the
  decode targets for generation/progression/sim.
- **Generation/progression is partly DATA-DRIVEN via assets, not just code.**
  Strong lead for Q1: a rating-randomization asset schema —
  `ContextDatabaseRandomChooserAsset`, `RandomValueAsset`, `UseDevelopmentCvars`,
  with fields `AgilityRatingMod / AccelRatingMod / SpeedRatingMod / ShotReleaseRating
  / BalanceRating / DekeRating` (VAs ~0x8201C3B3, 0x8201F81C, 0x8201FED4..0x8201FFC4).
  If these are loose/.big assets, generated-player attribute distribution is
  editable without touching code. **NEXT: locate these assets in game data.**
- **Franchise config keys exist** (low FE region, likely cvars/config-asset keys):
  `Rookie_Difficulty` (0x82004B04), `Max draft round for random draft` (0x82003118),
  `PROSPECT_TEAMS` (0x82004A2C), `NHLEntryDraft`, `GENERATE_OFFENSE`/`PK_GENERATE_OFFENSE`,
  `defaultdraft`, `Potential` (0x820042E8), `BAP_PLAYER_GROWTH` (0x82035818).
- **A 2nd EA tweak pool exists at ~0x839A0000–0x83B20000** (distinct from the
  gameplay pool 0x83Cxxxxx that NHL_DUMP_TUNABLES scanned). The franchise/FE
  settings descriptors live here. NOTE: `Lowest Normallized Goalie Rating`
  (0x8200DCD4) turned out to be a GOALIE-AI gameplay setting, not franchise gen —
  corrected. Extending the tunable dump to this pool would expose franchise knobs.

## Phase 1 RESULT (RandomValueAsset hunt) — NEGATIVE, ruled out

There is NO standalone editable "generation distribution" asset. The leads were
false positives:
- `ContextDatabaseRandomChooserAsset` / `RandomValueAsset` / `UseDevelopmentCvars`
  = EA's generic skating/animation randomization (`bgs_psk_useDevelopmentCvars`,
  fields AgilityRatingMod/AccelRatingMod = skating attribute→anim modulation), not
  roster generation.
- `GENERATE_OFFENSE` / `BAPGenerateOffenseNewTeamTask` = a Be-A-Pro play-generation
  task, not player generation.
- `Rookie_Difficulty` = the lowest difficulty-ladder name (with Pro_/AllStar_/
  Superstar_Difficulty), NOT rookie-gen tuning. `Potential` = just the attribute's
  name in the canonical attribute list. `PROSPECT_TEAMS`/`defaultdraft`/
  `NHLEntryDraft` = team/draft TYPE enum labels.
CONCLUSION: generated-player rating distribution is CODE (GmModeData) + DB per-record
inputs (leaguerookiedata.scalepct/initialerrormargin, c/e/s_potential). Q1/Q2 need
Phase 4 (runtime hook) or Phase 5 (decode). Editable levers that DO exist: DB
per-record fields (pre-authored prospects only) + AI/Ratings attribute curve.

Built-in dumps: only `gDumpGmInjuryTrackingData` (-> d:\injurynhl.csv / injuryahl.csv,
schema team,position,injurytype,severity,injurystring,occurrence,gamesplayedhurt,
mangameslost). No draft/gen/progression dumper exists.

## Status

- [x] Data surface mapped + verified ([[franchise-sim-data-surface]])
- [x] Injuries fully characterized -> docs/franchise-injuries-reference.md
- [x] Phase 1 (data-asset generation lever) — ruled out (negative result above)
- [x] Tunable registry checked — generation/progression NOT present (it's code)
- [x] Locator tool written + corrected (decimal lis/addi reconstruction):
      `tools/franchise_re/find_franchise_code.py`
- [x] Decompressed image obtained (`default.decompressed.exe`)
- [x] **Franchise leaguelogic TUs pinned** (GmModeData/NameGenerator/InjuryGen/
      InjuryBrain seeds + the .60/.61 address band)
- [ ] Enumerate GmModeData TU function range; find rookie-gen, progression, and
      box-score routines by contiguity + DB-tag/call-graph anchors
- [ ] Decode rookie-gen → answer Q1 (rating distribution absolute vs league-relative)
- [ ] Decode progression → answer Q2 (growthletter/mod/speed/age + c/e/s_potential → per-season delta)
- [ ] (parallel) Runtime experiment: lower league spectrum, sim ~10yr, inspect generated class
