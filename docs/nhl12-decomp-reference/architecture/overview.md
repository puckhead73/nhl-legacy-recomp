# Architecture Overview

How NHL 12 is structured from boot to gameplay, reconstructed from `[P4]` module
paths, `[RTTI]` engine symbols, `[IMP]` imports, and `[RT]` observed runtime
behaviour of the recompiled build. See [`../maps/codebase-map.md`](../maps/codebase-map.md)
for the module tree this narrative refers to.

---

## 1. The stack, top to bottom

```
┌─────────────────────────────────────────────────────────────┐
│ GAME (internal/*)                                            │
│  nhlfrontend ── menus, modes, GM/franchise, ResourceKernel  │
│  cmn (nhl)   ── game setup / session glue (ClubSetup)       │
│  nhlgameplay ── on-ice sim: AI · physics · anim glue · rules│
│  nhlrender   ── NHL scene/material/particle render glue     │
├─────────────────────────────────────────────────────────────┤
│ MIDDLEWARE                                                  │
│  Lynx (params+particles) · OSDK/Blaze/EAStore/AdManager     │
├─────────────────────────────────────────────────────────────┤
│ ENGINE — RenderWare 4 (rw::)                                │
│  rw::core (filesys · controller · timer · codec)            │
│  rw::math · rw::audio::core · rw::movie                     │
│  EASTL · EA::Allocator · MemoryFramework                    │
├─────────────────────────────────────────────────────────────┤
│ PLATFORM — Xbox 360                                         │
│  xboxkrnl (threads, files, mem) · xam (UI, user, input)     │
│  Xenos GPU (Vd ring) · XMA audio · XDK D3D/shader ucode     │
└─────────────────────────────────────────────────────────────┘
```

The recompilation replaces the bottom layer: `xboxkrnl`/`xam`/`Vd`/`XMA` become
RexGlue's host runtime (Win32 threads, host VFS, D3D12 translation of Xenos, FFmpeg
XMA). Everything above is the **original game/engine code**, recompiled 1:1. See
[`../recompilation/`](../recompilation/).

---

## 2. Boot flow (CONFIRMED by `[RT]` + `[IMP]`, sequence INFERRED)

Observed when running the recompiled `nhl12.exe` (Phase 4 build):

1. **XEX entry → EA CRT startup.** Image base `0x82000000`, entry `0x828588A8`.
   EA's C runtime initialises; ~19,000 import calls deep before the first thread
   spawns. `[RT]`
2. **Platform/config probe.** `ExGetXConfigSetting` (video mode, locale),
   `XexCheckExecutablePrivilege`, `XeCrypt*` — the classic EA boot-gate trio. Must
   return honest values or the game silently exits. `[IMP]`
3. **setjmp barrier.** EA wraps engine init in `setjmp`/`longjmp` (addresses
   `0x83366050` / `0x833643B0`). Protected init regions use non-local return for
   error handling. `[ASM]`
4. **Engine bring-up.** RenderWare core comes up: memory framework, `rw::core`
   filesystem (`cache:\` device), controller manager, timer. Threads spawn:
   **NHL Sim**, **NHL Render**, **JobManager** workers, **AssetStream**
   (load/unpack/translate), **XMA audio worker**. `[RT]`
5. **GPU init.** `VdInitializeRingBuffer` arms the command processor;
   `SetInterruptCallback`; EDRAM render targets created; Xenos→DXBC shader
   pipelines built. `[RT]`
6. **Asset load from `cache:\`.** The game opens `.big` archives and a loose tree
   under `cache:\` (rendering/audio/AttribDB/shaders/scrape). **This is where the
   current build stalls** — `cache:\` must be pre-populated; the game does not
   unpack the `.big`s itself. No `VdSwap` (presented frame) is reached yet. `[RT]`
7. *(Target)* First `VdSwap` → boot movies (`rw::movie`) → frontend → Play Now.

> Boot ordering between steps 4–6 is INFERRED from thread-spawn and file-open
> ordering in the run log; the exact init call graph is not yet pinned.

## 3. Execution model: the sim/render split (CONFIRMED threads, INFERRED roles)

Runtime thread names confirm a classic EA Sports **decoupled simulation/render**
architecture `[RT]`:

- **NHL Sim thread** — fixed-step gameplay simulation: AI (`nhlgameplay/ai`),
  physics (`physmodule`/`puck`), rules (`rulebook`), animation state advance
  (`animplayer`). Almost certainly a fixed tick (hockey sims of this era run a
  fixed 30 or 60 Hz gameplay step). **UNKNOWN:** exact tick rate — needs the sim
  loop pinned. See [`main-loop.md`](main-loop.md).
- **NHL Render thread** — consumes a snapshot/interpolated view of sim state and
  submits RenderWare draw work to the Xenos ring. Decoupled from sim so rendering
  can interpolate between sim ticks.
- **JobManager workers** — a job/task pool for parallelisable work (animation
  sampling, culling, particle sim, asset transforms). `[RT]`
- **AssetStream thread(s)** — background asset load → unpack → "translate"
  (endian/format fixup into runtime form). `[RT]`
- **XMA audio worker** — feeds the hardware/host audio mixer; busy-spins on XMA
  register `0601` when its source streams (music) are missing. `[RT]`

This split is the central thing to understand for both gameplay and recomp work:
**gameplay correctness lives on the Sim thread; rendering correctness on the Render
thread; the two communicate through a synchronised state buffer.** Threading
hazards in the recomp (memory ordering across this boundary) are a known risk —
see [`threading.md`](threading.md).

## 4. Memory model (CONFIRMED framework, INFERRED layout)

- The 360 has a **flat, unified 512 MB** address space; the guest sees a single
  4 GiB space (the recomp reserves 4 GiB so guest pointers are valid host
  pointers via `base + addr`). `[ASM]`
- All engine allocation goes through **MemoryFramework 1.10** with **categories**
  (`category.cpp`) — per-subsystem budgets/heaps. RenderWare is bridged in via
  `renderware.cpp`. EASTL containers and `EA::Allocator::FixedAllocator` /
  `ea::movablebuffer` sit on top. `[P4][RTTI]`
- **Big-endian** throughout (PPC). Every guest memory access in the recomp
  byte-swaps; original code assumes BE layout for all serialized structures. `[ASM]`
- Detail of the heap layout (where each category lives, sizes) is **UNKNOWN** —
  resolving it requires reading `MemoryFramework` init. See
  [`memory.md`](memory.md).

## 5. Module boundaries & ownership (INFERRED from package split)

| Owns | Package | Talks to |
|---|---|---|
| What match to play (teams, mode, rules toggles) | `nhlfrontend` + `cmn/ClubSetup` | hands a configured session to `nhlgameplay` |
| The on-ice world (players, puck, officials) | `nhlgameplay` | reads input (`rw::core::controller`), drives anim (`animplayer`), submits to `nhlrender` |
| Between-games sim (season, injuries, GM) | `nhlfrontend/leaguelogic` | shares injury model with `nhlgameplay/injury` |
| Drawing the world | `nhlrender` + RenderWare | Xenos ring via `Vd*` |
| Sound | `rw::audio::core` / RWAudioCore | XMA, asset streams |
| Files | `rw::core::filesys` | `cache:\` VFS |

## 6. Shutdown (UNKNOWN — evidence missing)

No clean shutdown path has been exercised (the build stalls at asset load, well
before any quit flow). The Phase 2 stub build "exits cleanly when the stubbed
`ExCreateThread` never runs its worker" — i.e. shutdown has only been seen as
*failure-to-start*, not an orderly teardown. Documenting the real shutdown/quit
sequence is an open task once the game reaches the frontend. See
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).

---

### Cross-references
- Module tree: [`../maps/codebase-map.md`](../maps/codebase-map.md)
- Main loop detail: [`main-loop.md`](main-loop.md)
- Threading hazards: [`threading.md`](threading.md)
- Memory: [`memory.md`](memory.md)
- How the platform layer is replaced: [`../recompilation/rexglue-runtime.md`](../recompilation/rexglue-runtime.md)
