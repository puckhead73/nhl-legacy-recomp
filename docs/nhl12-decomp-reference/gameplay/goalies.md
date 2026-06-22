# Gameplay — Goalies

The goaltender is a **first-class subsystem** with its own directory:
**`internal/nhlgameplay/ai/goalie/`** (5 CONFIRMED files `[P4]`). This is unusual
granularity and tells us goaltending is one of the most developed AI systems in the
game. Behaviour INFERRED from filenames + hockey domain knowledge.

## 1. The five goalie modules (CONFIRMED files)

| File | Role (inferred) |
|---|---|
| **`goalie.cpp`** | Top-level goalie controller / state owner — the goalie "brain" that ties the others together and runs each tick. |
| **`goalie_analysis.cpp`** | **Reads the play**: shooter position & angle, puck speed, whether the goalie is screened, the dangerous lanes, pass-across threat. Produces the threat picture the controller acts on. |
| **`savespace.cpp`** | The **"save space"** model — the net-coverage geometry the goalie tries to occupy. Effectively a map of "what part of the net is currently defended vs. exposed" that `goalie_analysis` and `goaliesave` consume. |
| **`goaliepose.cpp`** | **Stance/pose selection** — butterfly, stand-up, post-integration (RVH/VH), hugging the post, depth in/out of the crease. Drives the goalie's animation state. |
| **`goaliesave.cpp`** | **Save execution** — selecting and committing a save: glove, blocker, pad/leg, poke-check, desperation. The action chosen when a shot is actually on net. |

## 2. How they fit together (INFERRED pipeline)
```
each sim tick (goalie):
  goalie_analysis  ── read shooter/puck/screen ──▶ threat picture
        │
  savespace        ── compute net coverage vs. threat ──▶ where to be / what's exposed
        │
  goaliepose       ── choose stance & depth to fill save space ──▶ animation state
        │
  (on shot)
  goaliesave       ── pick & commit save action ──▶ paired w/ animation + puck physics
```
The split between **`savespace`** (geometry/intent) and **`goaliepose`/`goaliesave`**
(animated execution) is the key design insight: the goalie reasons about *coverage*
abstractly, then realises it through stance and save animations. CONFIRMED modules;
pipeline ordering INFERRED.

## 3. Animation & physics coupling (INFERRED)
- `goaliepose` is tightly bound to the animation system
  ([`../animation/animation-system.md`](../animation/animation-system.md)) — a chosen
  stance is an animation state.
- `goaliesave` outcomes interact with **puck physics**
  ([`puck-physics.md`](puck-physics.md)) — a glove save catches, a pad save deflects
  (rebound), a missed save is a goal. The save→rebound vector is a physics handoff.
- A high shot that beats the goalie can knock off the **water bottle**
  (`physics/waterbottle.cpp`) — a deliberate presentation detail.

## 4. Tuning (CONFIRMED storage)
Goalie skill (reaction time, save selection, rebound control) is almost certainly
parameterised via `aidata.big` + Lynx parameters (see [`ai.md`](ai.md) §3), scaled by
difficulty and the goalie's rating. **UNKNOWN** until `aidata.big` is unpacked.

## 5. Recomp fidelity notes
- `goalie_analysis`/`savespace` are geometry-heavy (VMX) → correct vector translation
  matters or the goalie misjudges angles.
- Save timing depends on the sim tick rate being faithful.

## Open questions
- The goalie state list inside `goalie.cpp` (pin & read).
- `savespace`'s representation (sampled net grid? angular sectors?).
- How rebounds are computed (in `goaliesave` vs. `physics/puck`).

See [`core-systems.md`](core-systems.md) and
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).
