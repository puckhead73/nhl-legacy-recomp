# Physics & Collision

Gameplay physics in **`internal/nhlgameplay/physics/`** (CONFIRMED files `[P4]`) plus
the rink/contact model (INFERRED). The puck has its own doc:
[`../gameplay/puck-physics.md`](../gameplay/puck-physics.md).

## 1. The physics modules (CONFIRMED files)
| File | Role |
|---|---|
| **`physmodule.cpp`** | The physics module entry/integrator — steps all gameplay bodies each sim tick (players, puck, dynamic items). |
| **`puck.cpp`** | The puck body (its own doc). |
| **`waterbottle.cpp`** | The net-top water bottle — a tiny independent rigid body. |

> Three named physics files, but `physmodule` is the hub. Player-body physics and the
> rink-boundary model don't have their own fired-assert strings, so they're inferred
> to live inside `physmodule` (or unrecovered siblings).

## 2. Bodies & collision layers (INFERRED)
NHL 12 must resolve collisions among:
- **Skaters ↔ skaters** — checks, bumps, tie-ups. Realised partly through
  `ai/twoplayeranim` (paired animations) rather than pure rigid-body response — i.e.
  contact is **animation-mediated** for the dramatic cases, physics for incidental
  bumps. CONFIRMED module (`twoplayeranim`); split INFERRED.
- **Skaters ↔ puck** — possession, deflections, blocked shots.
- **Skaters ↔ boards/glass/net** — the rink boundary; players crunch into the boards.
- **Puck ↔ boards/glass/posts/net** — bounces (see puck doc).
- **Dynamic items** — loose sticks (`ai/loosestickmanager`), gloves, the water bottle.

A layered collision filter (who collides with what) is INFERRED standard practice;
the concrete layer set is **UNKNOWN** pending `physmodule` being pinned.

## 3. Rink boundaries & net (INFERRED)
- The rink is a closed boundary with rounded corners; boards + glass have different
  restitution (glass is livelier). The net is a frame (posts/crossbar = hard bounce)
  plus mesh (catches/slows the puck) plus the crease (a gameplay region for goalie
  interference, judged by [`../gameplay/rules.md`](../gameplay/rules.md)).
- The **water bottle** sits on the net and is knocked off by shots that hit high —
  modelled separately (`waterbottle.cpp`) purely for presentation.

## 4. Coupling to animation & gameplay (CONFIRMED modules)
- **Animation-driven contact:** `ai/twoplayeranim` locks two skaters into a
  synchronised interaction; physics yields to the animation for its duration, then
  resumes. This is a fragile recomp area (both bodies must advance in lockstep on the
  sim thread). See [`../animation/animation-system.md`](../animation/animation-system.md).
- **Choreographed skating:** `ai/choreo/choreoglide` blends scripted motion with the
  integrator (see [`../gameplay/skating.md`](../gameplay/skating.md)).

## 5. Recomp fidelity notes
- Heavy **VMX/FP** — translation must preserve rounding and vector lane order, or
  collisions resolve subtly wrong (drift, bad bounces) without crashing.
- **Determinism** (`randomd0`) — physics randomness must be bit-exact for replays.
- **Fixed tick** — substep count and `dt` must match the original or fast objects
  (shots) tunnel or bounce differently.
- Validate against the **Xenia reference oracle** — physics divergence is the classic
  silent recomp failure.

## Open questions
- Player-body model (capsules? where in `physmodule`).
- Collision layer/filter set; continuous-collision for fast shots.
- Board/glass/net restitution + ice friction constants (data vs. code).
- Exact physics↔animation arbitration for contact.

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
