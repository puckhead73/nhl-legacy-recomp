# Gameplay — Core Systems

The hub for on-ice simulation. Everything here is in the **`internal/nhlgameplay`**
package, whose module tree was recovered from `[P4]` source paths (see
[`../maps/codebase-map.md`](../maps/codebase-map.md) §3.2). Module *names* are
CONFIRMED; the system descriptions are INFERRED from the filename + hockey/EA-Sports
domain knowledge unless a stronger source is cited.

> **How to read this section.** Each gameplay system below names the `[P4]` source
> file(s) it is built from. To go from "named module" to "running code", use the
> pinning method in [`../maps/codebase-map.md`](../maps/codebase-map.md) §6. None of
> these are address-pinned yet — that is the open RE work.

---

## 1. The gameplay tick (where these systems run)
All of this executes on the **NHL Sim** thread (see
[`../architecture/main-loop.md`](../architecture/main-loop.md)) as a fixed-step
simulation, feeding a state snapshot to the render thread. The modules below are the
phases of that tick.

## 2. System → module map (CONFIRMED files)

| Hockey system | `nhlgameplay` module(s) `[P4]` | Doc |
|---|---|---|
| **Player AI brain** | `ai/aistruct`, `ai/ai_math`, `ai/randomd0` | [`ai.md`](ai.md) |
| **Skating / movement** | `ai/choreo/choreoglide`, `ai/bench` (line changes) | [`skating.md`](skating.md) |
| **Body checking** | `ai/checkingstatemachine`, `ai/ccheck`, `ai/twoplayeranim` | this doc §3 |
| **Stick / poke checking** | `ai/cstickcheck` | this doc §3 |
| **Faceoffs** | `ai/faceoff`, `ai/livefaceoff`, `ai/tface`, `ai/tutorialmode/faceoffdrill` | this doc §4 |
| **Offensive strategy** | `ai/stratoff_rush`, `ai/toffplay` | [`ai.md`](ai.md) |
| **Goaltending** | `ai/goalie/{goalie,goalie_analysis,goaliepose,goaliesave,savespace}` | [`goalies.md`](goalies.md) |
| **Rules / officiating** | `ai/rules/{rulebook,oniceofficial}`, `ai/objects/referee` | [`rules.md`](rules.md) |
| **Post-whistle behaviour** | `ai/postwhistle/postwhistlebrain` | this doc §5 |
| **Fighting** | `ai/fight` | this doc §6 |
| **Injuries** | `ai/injury` (+ frontend `leaguelogic/injurygen`) | this doc §7 |
| **Loose items** | `ai/loosestickmanager`, `ai/dynamic_items/dynamic_items_interface` | this doc §8 |
| **Puck & physics** | `physics/{physmodule,puck,waterbottle}` | [`puck-physics.md`](puck-physics.md), [`../physics/collision.md`](../physics/collision.md) |
| **Animation glue** | `anim/animplayer`, `ai/twoplayeranim` | [`../animation/animation-system.md`](../animation/animation-system.md) |

## 3. Checking (body + stick) — INFERRED from CONFIRMED modules
Body checking is modelled as a **state machine** (`checkingstatemachine.cpp`),
strongly implying discrete phases: *eligible → wind-up/approach → contact →
result* (clean hit / missed / boarding penalty). `ccheck.cpp` is the body-check
evaluation; `cstickcheck.cpp` is the separate stick/poke-check path. Contact that
involves two skaters is realised through **`twoplayeranim.cpp`** — paired animations
that both players are locked into for the duration (see §Animation coupling below).

> **Animation↔gameplay coupling (important).** The existence of a dedicated
> `twoplayeranim` module means certain gameplay outcomes (a check connecting, a tie-up,
> a board battle) are driven by **synchronised two-actor animations**: gameplay
> selects the interaction, animation plays it on both bodies, and the *result* feeds
> back into gameplay. This is a classic place where "animation drives gameplay" — and
> a fragile one for the recomp, because both actors must advance in lockstep on the
> sim thread. CONFIRMED module; mechanism INFERRED.

## 4. Faceoffs — INFERRED from CONFIRMED modules
Four files cover faceoffs, suggesting a layered system:
- `faceoff.cpp` — the faceoff setup/rules (who takes it, where, alignment).
- `livefaceoff.cpp` — the **interactive** resolution (timing/stick input → win/loss,
  clean draw vs. scrum).
- `tface.cpp` — a helper (team/timed faceoff support).
- `tutorialmode/faceoffdrill.cpp` — the training-mode faceoff drill.

`choreoglide` (see [`skating.md`](skating.md)) likely positions skaters into the
faceoff formation before the drop.

## 5. Post-whistle behaviour — `postwhistle/postwhistlebrain.cpp` CONFIRMED
A dedicated "brain" governs what 12 skaters + officials do **after a whistle**:
returning to position, scrums/shoving, escorting, lining up for the next faceoff.
This is presentation-critical (it's what makes stoppages feel alive) and is a
separate AI controller from the in-play brain.

## 6. Fighting — `ai/fight.cpp` CONFIRMED
NHL 12 has a fighting engine (the series' "Enforcer" fighting). Self-contained
minigame logic: instigation → drop gloves → punch/block/grab exchange → result
(takedown / linesmen break it up → penalties via the rulebook).

## 7. Injuries — `ai/injury.cpp` + `nhlfrontend/leaguelogic/injurygen.cpp` CONFIRMED
Two-sided: **in-game** injury occurrence/evaluation lives in gameplay (`injury.cpp` —
a hit or block causes an injury), while **season/franchise** injury *generation* lives
in the frontend (`injurygen.cpp`). They share the injury concept across the
sim/frontend boundary — a notable cross-package dependency.

## 8. Loose items & dynamic objects — CONFIRMED modules
- `loosestickmanager.cpp` — tracks **dropped sticks** on the ice (after a slash, a
  fight, a broken stick) as collidable items a player can be without/pick up.
- `dynamic_items/dynamic_items_interface.cpp` — the general interface to dynamic
  on-ice objects (loose pucks, gloves, lost equipment, the water bottle).

## 9. Determinism — `ai/randomd0.cpp` CONFIRMED
A dedicated deterministic RNG ("random d0") underpins the sim. This matters for
**replays, instant-replay, and any netcode** (both clients must compute the same
outcome from the same inputs+seed) — and it is a **recomp fidelity requirement**: the
RNG must be bit-exact or sims diverge. See
[`../recompilation/xenondecomp-notes.md`](../recompilation/xenondecomp-notes.md) §6.

---

### What we can't yet say (UNKNOWN)
- Exact state lists/transition tables inside `checkingstatemachine` and the goalie
  state machines — needs the functions pinned and read.
- Tuning values (check thresholds, faceoff win curves) — these live in **`aidata.big`**
  (AI tuning archive) and the **Lynx parameter registry**, not in code. Reading them
  needs the `.big` unpacker + Lynx parameter format.
- The exact sim tick rate and integration order.

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
