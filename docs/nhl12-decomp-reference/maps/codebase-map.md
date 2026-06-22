# Codebase Map — Original Module / Package Structure

**The single most useful artifact in this documentation set.** NHL 12's retail
build shipped with **full Perforce source paths embedded in assert strings**
(`[P4]`). Mining the binary for these recovers the *original* EA source tree:
package names, version numbers, directory layout, and individual `.cpp/.h`
filenames — the structure that the 103,714 anonymous `sub_<addr>` functions were
compiled from.

This file is the spine of the whole documentation set. Every subsystem doc maps
back to a node here.

> **All paths in this file are CONFIRMED** — they are literal strings in
> `extracted/nhlzf_image.bin`. The full extracted list is in
> [`../../docs-build_srcpaths.txt`](../../docs-build_srcpaths.txt) (86 distinct
> source-file paths that survived as `__FILE__` in fired asserts) plus path
> fragments and `rw::` RTTI. **Role/intent annotations are INFERRED** from the
> filename and EA-Sports/RenderWare domain knowledge, and are marked where the
> inference is non-trivial.

---

## 1. Build provenance: two Perforce roots

Two distinct depot roots appear in the strings — the binary was assembled from a
merge of an EA-wide tech branch and the NHL 12 release branch:

| Root | Meaning |
|---|---|
| `c:\p4\nhl\main\packages\…` | The **EA-shared / "main" tech** branch (basekit, external middleware). Older path style; shared across EA titles. |
| `c:\p4\sync1\nhl12\release\packages\…` | The **NHL 12 release** branch — the game-specific `internal\*` packages and pinned versions of basekit. |

Also present: `e:\xenon\xdk-main-feb11\…` — the **Microsoft Xbox 360 XDK
(February 2011)**, statically linked (notably the D3D shader-microcode compiler,
`xtl\graphics\xgraphics\ucode\…`). `[P4]`

EA's build convention is `packages\<category>\<name>\<version>\(source|include)\…`
and for the game code `packages\internal\<pkg>\dev\source\<pkg>\<module>\…`.

---

## 2. Top-level taxonomy

```
NHL 12 (nhlzf.exe)
├── internal/        ← THE GAME (EA Canada NHL team)
│   ├── nhl          ← cmn: common/shared game runtime
│   ├── nhlgameplay  ← on-ice simulation, AI, physics, animation glue
│   ├── nhlfrontend  ← menus, modes, league/franchise logic, UI kernel
│   └── nhlrender    ← game-specific rendering (particles, scene glue)
│
├── basekit/         ← EA shared platform technology ("BaseKit")
│   ├── sys/MemoryFramework 1.10      ← allocators, memory categories
│   ├── audio/rwaudiocore 6.02        ← RenderWare Audio Core (+ Speex)
│   └── online/                       ← networking / live services
│       ├── osdk 6.01      ← Online SDK (settings, presence, streaming)
│       ├── blazesdk 3.09  ← Blaze (matchmaking, rooms, leaderboards, VoIP)
│       ├── eastore 1.11   ← EA Store / DLC / entitlements
│       └── admanager 3.06 ← in-game advertising
│
├── external/        ← third-party / cross-EA middleware
│   └── lynx 1.7.1   ← parameter/tuning + particle system
│
├── (engine kernel)  ← EA RenderWare 4 — namespace rw::*  (see §4)
│   └── eastl / EA::Allocator / ea::movablebuffer  (EA STL + memory)
│
└── (platform)       ← MS Xbox 360 XDK Feb-2011 (D3D, XMA, shader ucode compiler)
```

**Engine identity — CONFIRMED:** the `RENDERWARE` banner string plus pervasive
`rw::core::*`, `rw::math::*`, `rw::audio::core::*`, `rw::movie::*` RTTI prove the
game runs on **EA's RenderWare 4** ("rw4") graphics/runtime kernel — the
post-Criterion EA Tech stack used across EA Sports titles of the PS3/360 era.
`MemoryFramework\…\renderware.cpp` shows RenderWare is wired through EA's memory
framework. `[RTTI][P4]`

---

## 3. The game: `internal/` packages

### 3.1 `internal/nhl` → module `cmn` (common runtime) — CONFIRMED package
The shared game core. Path form: `…\internal\nhl\dev\source\cmn\<module>\…`.
Confirmed files/modules:

| Path `[P4]` | Inferred role |
|---|---|
| `cmn\comm\gamesetup\clubsetup.cpp` | Match/club setup: team selection, roster assignment into a game session. `comm` = the "commerce"/communication-or-common layer that wires a game to be played. |
| `cmn\test\vector_test.cpp` | Unit test for the math `vector` type (left in retail). |

> `cmn` is the bridge package between frontend (which configures a game) and
> gameplay (which simulates it). `ClubSetup` is where a configured match becomes a
> live session — a key call-chain target when tracing "Play Now → on ice".

### 3.2 `internal/nhlgameplay` — the on-ice simulation — CONFIRMED package
The richest recovered package. Path form:
`…\internal\nhlgameplay\dev\source\nhlgameplay\<module>\…`. Modules:

#### `ai/` — the gameplay "brain" (32+ confirmed files)
This is where hockey is actually played. Confirmed files `[P4]`:

| File | Inferred role |
|---|---|
| `ai_math.cpp` / `ai_math.h` | Shared AI vector/geometry math (intercept points, angles, lead targeting). |
| `aistruct.cpp` | Core AI data structures / per-player AI state block. |
| `bench.cpp` | Bench / line-change / substitution management. |
| `ccheck.cpp` | Body-checking logic ("c-check"). |
| `checkingstatemachine.cpp` | **State machine** governing a check attempt → contact → result. |
| `cstickcheck.cpp` | Stick-check / poke-check logic. |
| `choreo/choreoglide.cpp` | **Choreography**: scripted/blended "glide" movement (canned positional moves). |
| `dynamic_items/dynamic_items_interface.cpp` | Interface to dynamic on-ice items (loose pucks, sticks, gloves, etc.). |
| `faceoff.cpp` | Faceoff logic (the AI/sim side). |
| `livefaceoff.cpp` | "Live" (in-progress, interactive) faceoff resolution. |
| `tface.cpp` | Faceoff helper (`t` = team/timed faceoff?). |
| `fight.cpp` | Fighting minigame logic. |
| `injury.cpp` | In-game injury occurrence/evaluation (paired with frontend `InjuryGen`). |
| `loosestickmanager.cpp` | Manages dropped/loose sticks on the ice. |
| `objects/referee.cpp` | Referee/linesman on-ice actor behaviour. |
| `postwhistle/postwhistlebrain.cpp` | **Post-whistle behaviour**: scrums, escorting, returning to position after a stoppage. |
| `randomd0.cpp` | Deterministic RNG / "random d0" source for sim (determinism-critical). |
| `rules/oniceofficial.cpp` | On-ice official decision-making (calls). |
| `rules/rulebook.cpp` | **The rulebook**: penalties, offside, icing, goal validity. |
| `stratoff_rush.cpp` | Offensive strategy: the **rush** (transition attack). `stratoff` = strategy-offense. |
| `toffplay.cpp` | Team offensive play execution. |
| `twoplayeranim.cpp` | **Two-player interaction animations** (checks, ties, board battles) — the gameplay↔animation coupling layer. |
| `tutorialmode/faceoffdrill.cpp` | Tutorial: faceoff drill. |

##### `ai/goalie/` — goaltender AI (its own sub-tree) — CONFIRMED
| File | Inferred role |
|---|---|
| `goalie.cpp` | Goalie top-level controller. |
| `goalie_analysis.cpp` | Reads the play (shooter angle, screen, lane) to choose a response. |
| `goaliepose.cpp` | Goalie stance/pose selection (butterfly, stand-up, post-hug). Animation-coupled. |
| `goaliesave.cpp` | Save execution (glove, blocker, pad, poke). |
| `savespace.cpp` | The "save space" model — net coverage geometry the goalie tries to fill. |

#### `physics/` — gameplay physics — CONFIRMED
| File | Inferred role |
|---|---|
| `physmodule.cpp` | The gameplay physics module entry/integrator. |
| `puck.cpp` | **Puck physics** (the single most timing-sensitive object). |
| `waterbottle.cpp` | The goalie-net **water-bottle** physics (the iconic "top-shelf knocks the bottle off" detail). A small, self-contained physics body. |

#### Other `nhlgameplay` modules — CONFIRMED
| Path | Inferred role |
|---|---|
| `anim/animplayer.cpp` | **Animation player** — drives skeletal animation playback for gameplay actors. |
| `analysis/dumpanalysis.cpp` | Debug/telemetry dump of analysis state. |
| `storytelling/randomorder.h` | "Storytelling" sequencing (commentary/presentation beats; `randomOrder` = non-repeating shuffle). |

> `nhlgameplay` is organised as **AI brain + physics + animation glue**, with the
> rulebook and officials as first-class modules. The presence of
> `checkingstatemachine`, `postwhistlebrain`, and `twoplayeranim` shows hockey
> contact is modelled as cooperating state machines that drive paired animations.

### 3.3 `internal/nhlfrontend` — menus, modes, league logic — CONFIRMED package
Path form: `…\internal\nhlfrontend\dev\source\nhlfrontend\<module>\…`.

| Path `[P4]` | Inferred role |
|---|---|
| `resourcekernel/resourcekernel.cpp` | **Resource kernel** — frontend resource/asset lifetime & loading manager (UI screens, fonts, textures). |
| `leaguelogic/gmmodedata.cpp` | **GM Mode / franchise** ("Be A GM") data model. |
| `leaguelogic/injurygen.cpp` | **Injury generation** for season/franchise sim (paired with gameplay `injury.cpp`). |

> The `leaguelogic` module confirms season/franchise ("GM Mode") simulation lives
> in the frontend package, not gameplay — a common EA split (the *on-ice* sim is
> `nhlgameplay`; the *between-games* sim is `nhlfrontend/leaguelogic`).

### 3.4 `internal/nhlrender` — game rendering glue — CONFIRMED package
Path form: `…\internal\nhlrender\dev\source\nhlrender\<module>\…`.

| Path `[P4]` | Inferred role |
|---|---|
| `particle/particleactionrender.cpp` | Renders particle effects (ice spray, snow, breath, glass shatter) — the *render* side of the Lynx particle system (see §5). |

> `nhlrender` is thin in the recovered set — most rendering is RenderWare (`rw::`)
> with `nhlrender` providing the NHL-specific scene/material/particle glue. The
> renderer is documented in depth in [`../graphics/`](../graphics/) and the
> existing investigation notes.

---

## 4. The engine: RenderWare 4 (`rw::*`) — CONFIRMED via RTTI

Namespaces recovered from RTTI/strings `[RTTI]`. This is the runtime kernel the
whole game sits on:

| Namespace | Recovered symbols (examples) | Role |
|---|---|---|
| `rw::core::filesys` / `filesystem` | `Manager::Allocate` | **Virtual filesystem** — the layer that opens `cache:\…`. The thing currently blocked on cache population (see HANDOFF). |
| `rw::core::controller` | `Manager`, `LLManager`, `DeviceState` (`mButtonValues`, `mAxisValues`, `mDigitalButtonValues`), `DeviceInfo`, `DeviceEffect` | **Input** — pads, buttons, axes, rumble (`DeviceEffect`). |
| `rw::core::codec` | `ZlibInflate::mStream` | Compression codecs (zlib). |
| `rw::core::timer` | `Stopwatch` | Timing. |
| `rw::math` | `Vector`, `VecFloat` | SIMD vector math (VMX/AltiVec-backed on 360). |
| `rw::audio::core` | `System`, `Voice` (`IsExpelled`, `Release`, `GetDecayTime`), `Pan`/`HwPan`, `StreamPool`, `SamplePlayer` | **Audio engine** (see [`../audio/`](../audio/)). Backed by `basekit/audio/rwaudiocore 6.02`. |
| `rw::movie` | `MoviePlayer`, `SubtitleDecoder` | **Full-motion video** playback (intro/cutscene movies). Relevant to the "green video" bug. |
| `rw::SimpleVector` | `reserve` | RW container. |

Plus EA's standard library layer `[RTTI]`:
- `eastl::*` (`hash_map`, …) — **EASTL** containers, used everywhere.
- `EA::Allocator::FixedAllocator`, `ea::movablebuffer::{BufferHandle,BufferReference}` — EA memory primitives.

---

## 5. Middleware & basekit detail (CONFIRMED packages + versions)

| Package `[P4]` | Version | Role / recovered files |
|---|---|---|
| `basekit/sys/MemoryFramework` | 1.10.00-nhl.01 | Allocator framework. `category.cpp` (memory categories/budgets), `renderware.cpp` (RW↔MemFramework bridge). |
| `basekit/audio/rwaudiocore` | 6.02.00-eac.03-nhl.01 | RenderWare Audio Core. Includes a Speex decoder (`decoders\speex\libspeex\nb_celp.cpp`) → **Speex voice** (commentary/voice streams). |
| `basekit/online/osdk` | 6.01.04.06-eac.01-nhl.01 | EA **Online SDK**: `downloadstreamer`, `modulemanager`, `presencedownloadmanagerabstract`, `setting/*`, `weboffer/*`, `xenon/platformuseridlookupoperation`, EASTL allocators. |
| `basekit/online/blazesdk` | 3.09.04.1-eac.01-nhl.01 | **Blaze** client: `blazeconnection`, `blazenetworkadapter/connapivoipmanager` (VoIP), `playgroups`, `rooms`, `statsapi` (`lbapi`/`lbfolder` = leaderboards). |
| `basekit/online/eastore` | 1.11.00-eac.01-nhl.01 | **EA Store / DLC**: `contentconsumable`, `contentmanager`, `dimemanager`, `statemanager`. |
| `basekit/online/admanager` | 3.06.02 | In-game **advertising** (`AdManagerAbstract`). |
| `external/lynx` | 1.7.1-nhl.01 | **Lynx**: a parameter/tuning + particle system. `parameter/{registry,attributed,hardblend}` (data-driven tunables, blendable params), `particles/{particleeffect,particleeffectinstance,particleset,particleactiondynamics}` (particle sim). Pairs with `nhlrender/particle`. |

> The entire `online/*` subtree (OSDK, Blaze, EAStore, AdManager) is **out of scope
> for the recomp MVP** and is bulk-stubbed at the import layer (the 47 `NetDll*`
> imports). It is documented for completeness and future EASHL/online work.

---

## 6. How this maps to the recompiled code

There is **no direct module→file mapping** in the recompiled output: RexGlue
partitions the 103,714 `sub_<addr>` functions into 154 `nhl12_recomp.N.cpp` files
**by address**, not by original module. To go from "a module here" to "code":

1. The original modules occupy **contiguous-ish address ranges** in the 27 MB image
   (the linker lays out a `.cpp`'s functions together). A `[P4]` assert string sits
   in `.rdata`; the function that references it (via a `lis/ori` address pair) is
   the one in that module — that is the anchor for pinning a module to addresses.
2. Once an address is pinned, find its translated body by searching
   `app/generated/default/` for `sub_<ADDR>` (see
   [`function-index.md`](function-index.md)).
3. Cross-reference callers/callees by scanning the image for `bl` encodings to/from
   the address (see `tools/add_unresolved.py` for the branch-decode pattern).

This is the standing method for deepening any subsystem doc from "named module"
to "pinned functions". Most subsystem docs below are at the **named-module** stage;
promoting them to **pinned-address** stage is the ongoing RE work tracked in
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).

---

## 7. Quick index → subsystem docs

| If you want… | Go to |
|---|---|
| Boot / main loop / threads | [`../architecture/`](../architecture/) |
| Faceoffs, checking, rules, goalies, AI | [`../gameplay/`](../gameplay/) |
| Skating / choreo glide | [`../gameplay/skating.md`](../gameplay/skating.md) |
| Puck / water bottle physics | [`../physics/collision.md`](../physics/collision.md), [`../gameplay/puck-physics.md`](../gameplay/puck-physics.md) |
| Animation player / two-player anim | [`../animation/animation-system.md`](../animation/animation-system.md) |
| Rendering, shaders, the blue/green bugs | [`../graphics/`](../graphics/) |
| Audio (rw::audio::core, Speex, XMA) | [`../audio/audio-system.md`](../audio/audio-system.md) |
| Menus, GM Mode, resource kernel | [`../ui/frontend.md`](../ui/frontend.md) |
| `.big` archives, cache:\ layout | [`../assets/asset-pipeline.md`](../assets/asset-pipeline.md) |
| The recompilation itself | [`../recompilation/`](../recompilation/) |
