# Threading Model & Hazards

**Status: 🟡 partial.** Thread set CONFIRMED `[RT]`; synchronisation details and
recomp hazards are part-confirmed, part-inferred.

## Confirmed threads `[RT]`
From the running recompiled build: **NHL Sim**, **NHL Render**, **JobManager**
worker pool, **AssetStream** (load/unpack/translate), **XMA audio worker**, plus
RexGlue's own **GPU Command** and **VSync** threads (host side, not original).

## Original 360 threading (CONFIRMED primitives `[IMP]`)
The game uses the standard Xbox 360 kernel threading surface:
- `ExCreateThread` / thread teardown — engine threads.
- `Ke*` synchronisation: `KeWaitForSingleObject`, events, semaphores, mutants,
  `KfAcquireSpinLock`/`KfReleaseSpinLock`, DPC-style primitives. (34 `Ke*`/`Ki*`
  imports.)
- `Nt*` events/IO for async asset streaming (`NtCreateFile`, overlapped IO).
- **Hardware threads:** the 360 CPU is 3 cores × 2 SMT = 6 hardware threads. EA
  pins engine threads to specific HW threads via `XSetThreadProcessor`-style calls.
  Thread→core affinity is **UNKNOWN** here but matters for reproducing timing.

## The critical boundary: Sim ↔ Render
Gameplay runs on **NHL Sim**; drawing on **NHL Render**. They exchange a
**synchronised state snapshot** each frame (double/triple-buffered transforms +
game state). This is the central concurrency contract. INFERRED from the thread
names + EA Sports architecture; the exact buffer and lock are not yet pinned.

## Recomp hazards (CONFIRMED relevance to the port)

1. **Memory ordering across threads.** The 360's PPC has a weaker memory model than
   x86; EA code relies on explicit `lwsync`/`sync` barriers. The recompiler must
   preserve these as host fences — a dropped barrier becomes a rare, irreproducible
   sim/render desync or a streaming corruption. **Highest-risk class of recomp
   bug.** `[ASM]`
2. **Spinlocks & busy-waits.** `KfAcquireSpinLock` and engine busy-spins assume the
   360 scheduler. The XMA worker's observed busy-spin on register `0601` (when music
   is missing) is a benign example; a malign one would peg a core. `[RT]`
3. **`ExCreateThread` semantics.** Phase 2 saw the game "exit cleanly when the
   stubbed `ExCreateThread` never runs its worker" — i.e. early bring-up depended on
   real host threads being created with correct entry/stack/affinity. `[RT]`
4. **Determinism.** `randomd0` (deterministic RNG) implies the sim is meant to be
   reproducible. Any threading nondeterminism injected by the recomp (e.g. job
   ordering) could diverge replays/netcode from the original. `[P4]`
5. **Interrupt callback / DPC emulation.** `SetInterruptCallback` (GPU) and
   DPC-style kernel calls must be serviced with correct timing or the render thread
   stalls. `[RT][IMP]`

## How to pin
- Enumerate each `ExCreateThread` call site: entry address, stack size, affinity
  arg. Name each thread by the work its entry function does.
- Find the sim/render handoff: a buffer index flip guarded by an event/critsec near
  the end of the sim tick and the start of the render frame.
- Inventory every `lwsync`/`sync`/`eieio` in the recompiled output around shared
  data — these mark the lock-free contracts that must survive translation.

## Open questions
- Thread→hardware-thread affinity map.
- The exact sim/render synchronisation object and buffering depth.
- Whether JobManager work-stealing introduces ordering the sim relies on being
  stable.

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md) and
[`../quirks/gotchas.md`](../quirks/gotchas.md).
