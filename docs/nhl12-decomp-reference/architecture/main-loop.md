# Main Loop & Frame Lifecycle

**Status: 🟡 partial.** The thread topology is CONFIRMED `[RT]`; the per-frame call
graph and tick rate are not yet pinned to addresses. This doc records what is known
and the exact method to resolve the rest.

## What's confirmed

NHL 12 uses the EA Sports **decoupled sim/render** model (see
[`overview.md`](overview.md) §3). At runtime the recompiled build spawns named
threads including **NHL Sim**, **NHL Render**, **JobManager** workers,
**AssetStream**, and an **XMA audio worker**. `[RT]`

This implies the canonical structure:

```
main()  (EA CRT → engine init, behind a setjmp barrier)
  └── spawns:
        NHL Sim thread     → fixed-step gameplay tick
        NHL Render thread  → variable-rate present, interpolates sim state
        JobManager pool    → parallel work fan-out/join per frame
        AssetStream        → background load/unpack/translate
        XMA audio          → mixer feed
```

### Sim thread (gameplay tick) — INFERRED structure
A hockey sim of this era runs a **fixed timestep**. Per tick, in order
(INFERRED from the `nhlgameplay` module set):
1. Sample input (`rw::core::controller::DeviceState`).
2. AI decision pass (`ai/*`: brains, strategy, goalie analysis).
3. Animation state advance (`anim/animplayer`, `twoplayeranim`).
4. Physics integrate (`physics/physmodule` → `puck`, player movement, `waterbottle`).
5. Collision/contact resolution (`checkingstatemachine`, board/net contact).
6. Rules evaluation (`rules/rulebook`, `rules/oniceofficial` — offside, icing,
   penalties, goals).
7. Publish sim state snapshot for the render thread.

> **UNKNOWN — tick rate.** Likely 30 Hz or 60 Hz gameplay. Resolve by finding the
> sim thread's timer/sleep and the fixed `dt` constant it integrates with.

### Render thread (frame present) — INFERRED structure
1. Acquire latest sim snapshot; interpolate transforms.
2. Update cameras (see [`../graphics/cameras.md`](../graphics/cameras.md)).
3. Cull + build draw lists (RenderWare world/atomics).
4. Submit passes to the Xenos ring (scene → players → ice → crowd → particles →
   UI/HUD → post). See [`../graphics/rendering-pipeline.md`](../graphics/rendering-pipeline.md).
5. `VdSwap` → present. **Not yet reached** in the current build (stalls at asset
   load). `[RT]`

## How to pin this (standing method)
1. In the recompiled output, find the Sim/Render **thread entry functions** — they
   are the `ExCreateThread` start routines. RexGlue logs thread creation; correlate
   the start address with `SetThreadName("NHL Sim")`-style calls. `[RT]`
2. From the thread entry, read the loop: a `while`/`bctr` back-edge wrapping a body
   of `bl` calls. Each `bl` target is a phase; name it by the `[P4]` assert string
   its body references.
3. The fixed `dt` appears as an immediate loaded near the timer query
   (`rw::core::timer::Stopwatch`).

## Open questions
- Exact sim tick rate and whether render is capped/vsynced independently.
- Whether JobManager parallelises within the sim tick (e.g. parallel AI/animation)
  or only render-side. Determinism (`randomd0`) suggests sim is at least logically
  serial.
- Frame pacing relationship to `VdSwap` cadence.

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
