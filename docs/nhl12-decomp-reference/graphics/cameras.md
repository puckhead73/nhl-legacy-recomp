# Graphics — Camera System

**Status: ⬜ mostly UNKNOWN.** No camera module surfaced as a named `[P4]` file, and
the render path isn't exercised at runtime yet (build stalls pre-first-frame). This
doc records what's inferable and how to resolve it.

## 1. What NHL 12 needs (INFERRED from the genre)
- **Broadcast/gameplay cameras** — the standard NHL camera presets (Overhead,
  Broadcast, Ice-level, Zoom, etc.) that track the puck/play, with selectable presets
  and zoom/height sliders.
- **Replay / instant-replay camera** — free-er cameras for replays, driven by the
  storytelling/presentation layer (`storytelling/randomorder` sequences
  presentation beats `[P4]`).
- **Cinematic / NIS cameras** — scripted cameras for non-interactive sequences
  (`nis.big`) and intros (`rw::movie`).
- **Frontend cameras** — menu/scene cameras (the `scenedef.lua` boot scene `[RT]`).

## 2. Where it likely lives (INFERRED)
- The on-ice gameplay camera is probably in `nhlrender` or a `cmn` presentation module
  (it consumes sim state — puck/players — to frame the play). The puck-tracking logic
  overlaps with `ai_math`-style geometry.
- RenderWare provides the low-level camera primitive (`rw::` view/frustum); the game
  drives it.

## 3. Recomp relevance
- Camera math is FP/VMX → faithful translation needed (a wrong camera is obvious but
  not crashy).
- Camera is **render-thread** state derived from the sim snapshot — part of the
  sim/render boundary (see [`../architecture/threading.md`](../architecture/threading.md)).

## 4. How to resolve (open RE task)
1. Reach the first frame (unblock `cache:\`).
2. Find the view-matrix setup feeding RexGlue's command processor each frame; trace
   back to the function that computes it from puck/player positions.
3. Look for camera-preset tables and the slider parameters (likely Lynx params).

## Open questions
- Camera preset list + which is default.
- Puck-tracking/framing algorithm.
- Replay camera control.
- Where camera state is computed and stored.

See [`rendering-pipeline.md`](rendering-pipeline.md) and
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).
