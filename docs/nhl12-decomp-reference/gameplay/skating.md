# Gameplay — Skating & Movement

How skaters move. The strongest CONFIRMED evidence is **`ai/choreo/choreoglide.cpp`**
`[P4]`; the rest is INFERRED from it and the surrounding modules.

## 1. Choreography-driven movement — `choreo/choreoglide.cpp` CONFIRMED
The module name is the key insight: skating is **choreographed**, not purely
physical. "Choreo glide" implies **blended, partly-scripted positional movement** —
the engine steers a skater along a chosen path/animation rather than integrating raw
forces. This is how EA hockey achieves smooth edges, crossovers, and tight turns that
pure physics would make jittery.

The `choreo/` *directory* (not just a file) implies a family of choreographed moves
(only `choreoglide` left a fired-assert string). INFERRED siblings: choreographed
turns, stops, pivots, faceoff approaches, celebration skating. **UNKNOWN** until more
paths/addresses surface.

## 2. Where skating sits in the tick (INFERRED)
```
AI decision (ai.md)  ──▶  desired position/target
        │
   choreoglide       ──▶  blended path + skating animation (anim/animplayer)
        │
   physics (physmodule) ──▶  body integration, collisions, momentum
```
So movement is a **negotiation between choreography and physics**: choreo provides the
intended motion, physics enforces momentum/collision. The blend point is where
"animation drives gameplay" for skaters (cf. `twoplayeranim` for contact).

## 3. Related movement modules (CONFIRMED)
- **`ai/bench.cpp`** — line changes / on-the-fly substitutions: skaters routing to/from
  the bench (a movement + roster-management system; too-many-men ties to
  [`rules.md`](rules.md)).
- **`anim/animplayer.cpp`** — plays the skating animation set the choreography selects
  (see [`../animation/animation-system.md`](../animation/animation-system.md)).
- **`physics/physmodule.cpp`** — the integrator that applies momentum/friction (see
  [`puck-physics.md`](puck-physics.md), [`../physics/collision.md`](../physics/collision.md)).

## 4. Ice friction / momentum (INFERRED, UNKNOWN detail)
Ice physics (acceleration, gliding deceleration, edge grip, turn radius vs. speed) is
modelled in `physmodule` and tuned via parameters (Lynx / `aidata.big`). Concrete
values UNKNOWN — needs the physics functions pinned and the tuning data unpacked.

## 5. Recomp fidelity notes
Skating blends are FP/VMX-heavy and timing-sensitive (the choreo blend advances per
tick). Faithful FP rounding, vector math, and tick rate are required or skating feels
"off" (drifty turns, wrong momentum) without crashing — a subtle, hard-to-test class
of regression.

## Open questions
- The full `choreo/` move set.
- The choreo↔physics blend formula (how much each contributes).
- Turn-radius / acceleration curves (data).

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
