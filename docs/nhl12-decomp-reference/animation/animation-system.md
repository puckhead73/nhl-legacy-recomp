# Animation System

Player and goalie animation. CONFIRMED anchors: **`anim/animplayer.cpp`**,
**`ai/twoplayeranim.cpp`**, **`ai/goalie/goaliepose.cpp`**,
**`ai/choreo/choreoglide.cpp`** `[P4]`, plus the **`faceposer.big`** facial-animation
archive `[STR]`. Mechanism INFERRED.

## 1. The animation player — `anim/animplayer.cpp` CONFIRMED
The central skeletal-animation driver for gameplay actors. Responsibilities
(INFERRED, standard for an EA animation player):
- Sample animation clips on a **skeleton** (the bind poses ship as
  `cache:\rendering\skeleton_bindpose\*.rx2` `[RT]`).
- **Blend** multiple clips (locomotion blend trees, additive layers, upper/lower-body
  splits — e.g. stickhandle while skating).
- Advance animation **state** each sim tick and produce the pose the renderer skins.

## 2. State + blending model (INFERRED)
EA hockey uses a **blend-tree / state-graph** animation model driven by gameplay
state. The recovered modules indicate at least these state sources:
- **Locomotion** — skating, driven by `choreo/choreoglide` (choreographed movement →
  matching animation; see [`../gameplay/skating.md`](../gameplay/skating.md)).
- **Contextual upper body** — stickhandling, shooting, passing, checking wind-up.
- **Goalie stance** — `goaliepose` selects butterfly/stand-up/post as an animation
  state (see [`../gameplay/goalies.md`](../gameplay/goalies.md)).

The exact graph/blend structure is **UNKNOWN** (needs `animplayer` pinned and read).

## 3. Two-player interaction animations — `ai/twoplayeranim.cpp` CONFIRMED
A dedicated system for **synchronised two-actor animations**: checks, tie-ups, board
battles, fight exchanges. Both skaters are locked into matching, aligned animations
for the interaction's duration, and the *result* feeds back to gameplay. This is the
clearest **animation-drives-gameplay** coupling in the game.

> **Recomp hazard (CONFIRMED relevance).** Both actors must advance in lockstep on the
> NHL Sim thread; the interaction's outcome depends on both poses being computed for
> the same tick. Any threading/ordering perturbation from the recomp could desync the
> pair. Flag when validating contact. See
> [`../architecture/threading.md`](../architecture/threading.md).

## 4. Facial animation — `faceposer.big` CONFIRMED archive
`gamedata/anim/faceposer.big` (8.2 MB) is a **facial-animation** archive `[STR]` —
"FacePoser"-style face/lip data for player faces in cutscenes/closeups (NIS sequences,
celebrations). Likely consumed by the presentation/cinematic layer (`rw::movie` +
NIS), not the on-ice sim. Internals UNKNOWN.

## 5. IK (INFERRED, UNKNOWN)
EA hockey of this era used inverse kinematics for skate-to-ice planting, stick-to-puck
contact, and goalie limb placement. No IK module surfaced by name — **UNKNOWN** whether
IK is inside `animplayer`, `choreoglide`, the goalie modules, or RenderWare. An open
RE target.

## 6. Animation ↔ rendering boundary
`animplayer` produces poses on the sim/job side; the **render** thread skins the
skeleton and draws (RenderWare + `nhlrender`). The skeletons are `.rx2` assets in
`cache:\`. See [`../graphics/rendering-pipeline.md`](../graphics/rendering-pipeline.md).

## Open questions
- The blend-tree/state-graph structure inside `animplayer`.
- Whether animation sampling runs on JobManager workers (parallel) — see
  [`../architecture/threading.md`](../architecture/threading.md).
- IK location and rig.
- `faceposer.big` format; how facial anim binds to NIS.

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
