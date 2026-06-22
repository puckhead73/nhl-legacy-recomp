# Gameplay — Rules & Officiating

Hockey rules enforcement lives in **`internal/nhlgameplay/ai/rules/`** plus the
referee actor. Module names CONFIRMED `[P4]`; behaviour INFERRED.

## 1. Modules (CONFIRMED files)

| File | Role (inferred) |
|---|---|
| **`rules/rulebook.cpp`** | **The rulebook** — the authority on legality: offside, icing, too-many-men, goal validity (puck fully over the line, no kicking/high-stick/crease interference), penalty definitions and their durations. The logic that decides *what* happened. |
| **`rules/oniceofficial.cpp`** | **On-ice official decision-making** — turns a rulebook fact into a *call*: when to blow the whistle, wave off a goal, signal a delayed penalty, wash out icing. The judgement/timing layer over the rulebook. |
| **`ai/objects/referee.cpp`** | The referee/linesman **actor** — the on-ice body that skates, positions, and animates the signal. The presentation of the call. |

> The separation is meaningful: **`rulebook`** = the rules as data/logic;
> **`oniceofficial`** = the official applying them (including delayed-penalty and
> advantage timing); **`referee`** = the visible skater. A rules question is answered
> by `rulebook`; *when the whistle blows* is `oniceofficial`.

## 2. What the rulebook almost certainly covers (INFERRED)
Standard NHL ruleset for the era:
- **Stoppages:** offside (blue line), icing (likely with NHL 12's touch-icing),
  hand-pass, high-stick on the puck, puck out of play.
- **Goals:** validity checks (over the line, no distinct kicking motion, no goalie
  interference, before/after whistle).
- **Penalties:** minors/majors/misconducts; tripping, hooking, slashing, boarding,
  charging, interference, roughing, fighting (→ `ai/fight.cpp`), too-many-men
  (→ `ai/bench.cpp` line changes), delay of game.
- **Special-teams state:** power play / penalty kill, delayed penalty (whistle on
  possession change), 4-on-4 / penalty offsetting.

## 3. Flow to/from other systems (INFERRED)
```
gameplay event (hit, puck crosses line, stick infraction)
      │
   rulebook        ── is it illegal? is it a goal? ──▶ fact
      │
 oniceofficial     ── should I call it now? delayed? ──▶ decision + whistle timing
      │
   referee (actor) ── skate/position/animate the signal
      │
 postwhistlebrain  ── players react, set up next faceoff (ai/postwhistle)
      │
 frontend/UI       ── scorebug penalty clock, replay (ui/, presentation)
```

## 4. Post-whistle handoff (CONFIRMED modules)
A whistle from `oniceofficial` hands control to
`ai/postwhistle/postwhistlebrain.cpp` (scrums, return-to-position) and then to
`ai/faceoff.cpp` for the next draw. See [`core-systems.md`](core-systems.md) §5.

## 5. Recomp notes
Rules are mostly integer/boolean logic (low FP risk) — should translate cleanly. The
risk is **timing** (delayed-penalty windows, icing touch races) depending on a
faithful sim tick.

## Open questions
- Whether icing is touch or no-touch in NHL 12 (read `rulebook`).
- Penalty duration/severity tables (likely data in `aidata.big`, not code).
- How rule *toggles* (e.g. offside/icing on/off, period length) flow from frontend
  (`cmn/ClubSetup`) into the rulebook.

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
