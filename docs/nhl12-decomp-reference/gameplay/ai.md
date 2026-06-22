# Gameplay — AI

The AI brain in **`internal/nhlgameplay/ai`**. Module names CONFIRMED `[P4]`;
behaviour INFERRED from filenames + domain knowledge. See
[`core-systems.md`](core-systems.md) for the full system→module table.

## 1. Core AI data & math (CONFIRMED files)
- **`aistruct.cpp`** — the per-player (and likely per-team) AI **state block**: the
  struct that holds what each AI skater knows and intends. The anchor data structure
  for the whole brain.
- **`ai_math.cpp` / `ai_math.h`** — shared AI geometry: intercept solving (where to
  skate to meet the puck), lead/aim targeting, angle/lane computations, time-to-arrive
  estimates. The math `ai/*` modules call.
- **`randomd0.cpp`** — deterministic RNG feeding AI decisions (so sims are
  reproducible). See [`core-systems.md`](core-systems.md) §9.

## 2. Decision structure (INFERRED)
EA hockey AI of this era is organised as a hierarchy:
**team strategy → role assignment → per-player brain → low-level steering/animation.**
The recovered files map onto that:

| Level | Module(s) `[P4]` | Role |
|---|---|---|
| Team offence / strategy | `stratoff_rush.cpp`, `toffplay.cpp` | "Strategy-offense: the rush" and team offensive play execution — coordinated attacks, breakouts, zone entries. |
| Per-player brain | `aistruct.cpp` (+ situational modules) | Individual decisions: support the puck, cover a man, forecheck, backcheck. |
| Situational sub-brains | `postwhistle/postwhistlebrain.cpp`, goalie brain (see [`goalies.md`](goalies.md)) | Context-specific controllers swapped in for stoppages, goaltending, etc. |
| Steering / movement | `choreo/choreoglide.cpp` | Turns a decision into smooth on-ice motion (see [`skating.md`](skating.md)). |

> Only the **offensive** strategy file (`stratoff_*`) surfaced in fired asserts; a
> defensive counterpart almost certainly exists (`stratdef_*` / forecheck-backcheck)
> but did not leave a string. **UNKNOWN** until more paths are recovered or addresses
> pinned.

## 3. Tuning lives in data, not code (CONFIRMED storage)
AI behaviour is **data-driven**: `gamedata/aidata.big` is the **AI tuning archive**
`[STR]`, and the **Lynx parameter registry** (`external/lynx`) provides blendable,
named tunables `[P4]`. So "how aggressive is the forecheck" is a *parameter*, not a
branch — reading the actual values requires the `.big` unpacker + Lynx parameter
format (both open tasks). This is also how difficulty levels and sliders work
(INFERRED): they scale these parameters.

## 4. Difficulty & sliders (INFERRED)
Not yet evidenced as code, but the data-driven design implies difficulty (Rookie→
Superstar) and the gameplay sliders select/scale Lynx parameter sets fed into the
brain and `ai_math` thresholds. **UNKNOWN** — needs `aidata.big` contents.

## 5. Recomp fidelity notes
- **Determinism** (`randomd0`) must be bit-exact (see recomp notes).
- `ai_math` is VMX-heavy (vector geometry) → correct SSE/AVX translation and lane
  order matter, or AI mis-aims/mis-skates subtly.

## Open questions
- Defensive strategy module name(s); the full situational-brain set.
- `aistruct` field layout (the AI state struct) — pin `aistruct.cpp` and read it.
- Difficulty/slider → parameter mapping (in `aidata.big`).

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
