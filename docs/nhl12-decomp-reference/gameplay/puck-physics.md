# Gameplay — Puck Physics

The puck is the most timing-sensitive object in the game and has its **own source
file**: **`internal/nhlgameplay/physics/puck.cpp`** `[P4]`. CONFIRMED module;
behaviour INFERRED. See [`../physics/collision.md`](../physics/collision.md) for the
broader collision/physics treatment.

## 1. Modules (CONFIRMED files)
| File | Role |
|---|---|
| **`physics/puck.cpp`** | The puck body: position/velocity/spin integration, bounces off boards/glass/posts, friction on ice, knuckling. |
| **`physics/physmodule.cpp`** | The gameplay physics module/integrator that steps `puck` (and player bodies) each tick. |
| **`physics/waterbottle.cpp`** | The net-top water bottle — a small independent physics body knocked off by high shots (presentation). |

## 2. What puck physics must handle (INFERRED)
- **Free flight & ice glide:** velocity + spin, ice friction, the puck settling flat.
- **Board/glass/post bounces:** restitution off the rink boundary and the posts/
  crossbar (a post ping is a distinct, beloved outcome).
- **Stick/puck interaction:** the handoff from the stick — shots, passes, deflections,
  one-timers, saucer passes. This is the boundary between puck physics and the
  shooting/passing systems (which select the launch vector; puck physics carries it).
- **Goalie interaction:** saves and **rebounds** — a pad save imparts a deflection
  vector (see [`goalies.md`](goalies.md) §3).
- **Goal detection handoff:** crossing the line is judged by
  [`rules.md`](rules.md) (`rulebook`), not the puck integrator.

## 3. The stick↔puck boundary (INFERRED, important)
Shooting and passing **select** a launch velocity/spin (influenced by player rating,
aim, stick position, and `randomd0` for spread); **`puck.cpp`** then **carries** it.
Deflections and tips re-enter puck physics mid-flight. The shot/pass *selection* code
did not surface as a named `[P4]` file yet (likely in `ai/` or a `shot`/`pass`
module) — **UNKNOWN** location, an open RE target.

## 4. Determinism (CONFIRMED relevance)
Puck outcomes must be deterministic for replays/netcode; `randomd0` seeds any
randomness (knuckling, bounce variation). The recomp must keep puck integration
bit-faithful — FP rounding and VMX lane order matter (see
[`../recompilation/xenondecomp-notes.md`](../recompilation/xenondecomp-notes.md) §6).

## 5. Recomp fidelity notes
This is the **highest-value object to validate** against the Xenia reference: small FP
divergences accumulate into visibly wrong bounces/rebounds. Pin `puck.cpp` and
`physmodule.cpp` early when validating physics.

## Open questions
- Puck integrator step (substeps per tick? continuous collision for fast shots?).
- Restitution/friction constants (data vs. code).
- Where shot/pass launch vectors are computed (the missing stick-handling module).

See [`../physics/collision.md`](../physics/collision.md) and
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).
