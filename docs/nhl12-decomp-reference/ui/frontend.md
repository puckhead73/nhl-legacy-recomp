# UI / Frontend

Menus, modes, HUD, and input. The frontend is the **`internal/nhlfrontend`** package
(CONFIRMED `[P4]`) atop RenderWare input/UI. Module names CONFIRMED; behaviour
INFERRED.

## 1. Frontend package (CONFIRMED files `[P4]`)
| Module | Role |
|---|---|
| **`resourcekernel/resourcekernel.cpp`** | The **Resource Kernel** — frontend resource/asset lifetime + loading manager: screens, fonts, textures, layouts. The backbone the menu system runs on. |
| **`leaguelogic/gmmodedata.cpp`** | **GM Mode / franchise** ("Be A GM") data model — rosters, schedule, standings, trades, finances. |
| **`leaguelogic/injurygen.cpp`** | Season/franchise **injury generation** (shares the injury concept with gameplay `injury.cpp`). |

> **Key architectural fact:** the **between-games simulation** (season/franchise/GM)
> lives in `nhlfrontend/leaguelogic`, separate from the on-ice sim
> (`nhlgameplay`). The frontend configures a match and hands it to gameplay via
> `cmn/ClubSetup` (see [`../maps/codebase-map.md`](../maps/codebase-map.md) §3.1).

## 2. Scene/flow is data + script driven (CONFIRMED `[RT]`)
The boot/scene flow uses **`cache:\scrape\{boot.scrape,scenedef.lua}`** — a **Lua**
scene/boot definition. So screen flow and scene composition are partly **scripted**,
not hardcoded. `[RT]` Frontend art/resources stream from `cache:\fe\…` (e.g.
`fe\profile\default.png`).

## 3. Input — `rw::core::controller` (CONFIRMED `[RTTI]`)
Input is RenderWare's controller layer:
- `Manager` / `LLManager` (high/low-level managers).
- `DeviceState` — `mButtonValues`, `mDigitalButtonValues`, `mAxisValues` (analog
  sticks/triggers + buttons).
- `DeviceInfo`, `DeviceEffect` (rumble/force feedback).

On the host this is fed by RexGlue's **`XamInput*` → XInput/SDL** override (4 imports)
`[IMP]`. Menu navigation and gameplay both read through `rw::core::controller`.

## 4. HUD / scorebug (INFERRED)
The in-game HUD (scorebug: teams, score, period, clock, power-play indicator,
penalty clock) is INFERRED standard. It's drawn in the UI pass
([`../graphics/rendering-pipeline.md`](../graphics/rendering-pipeline.md) §3) and fed
by sim state (score/clock from the rules/sim) + special-teams state from
[`../gameplay/rules.md`](../gameplay/rules.md). No HUD module surfaced by name —
**UNKNOWN** location (likely `nhlfrontend` + `nhlrender` text/sprite).

## 5. Localization (CONFIRMED multi-language `[STR]`)
The disc title lists **En, Fr, De, Sv, Fi, Ru, Cs**; audio manifests are
locale-tagged (`NA_En_loadfile.xml`). So there is a string/locale system selecting
text + audio + region. Mechanism UNKNOWN (string-table format not yet found).

## 6. Game modes (INFERRED from packages)
Evidenced/INFERRED modes: **Play Now** (exhibition — the recomp MVP target),
**Be A GM** (franchise — `gmmodedata`), Be A Pro, Seasons, plus online (EASHL/Hockey
Ultimate Team) via the `online/*` stack (out of recomp scope). The mode set is
selected/configured in the frontend and realised by handing a session to gameplay.

## 7. Recomp relevance
- The frontend is the **MVP path** (boot → menu → Play Now). `ResourceKernel` and the
  Lua scene flow are early on the critical path once `cache:\` is populated.
- `XamShow*` system dialogs (15 imports) are stubbed to auto-dismiss so the frontend
  doesn't block on system UI. `[IMP]`

## Open questions
- The menu/screen state machine (in `resourcekernel` + Lua).
- HUD/scorebug location + data binding.
- String-table/localization format.
- Full mode list and their data models.

See [`../maps/codebase-map.md`](../maps/codebase-map.md) and
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).
