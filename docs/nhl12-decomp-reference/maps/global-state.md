# Global State

Where the game keeps its global/singleton state. Mostly an **open RE target** — this
doc records the confirmed platform-provided globals and the method to find the game's.

## 1. Runtime-provided guest globals (CONFIRMED `[IMP]`)
The recomp must populate **13 data-variable import slots** as fixed structs in guest
memory with 360 field layout + endianness (the game reads them directly):
`KeDebugMonitorData`, `XboxHardwareInfo`, `KeTimeStampBundle`, `ExLoadedImageName`,
and others (enumerated in `docs/kernel_imports.csv`). Getting these wrong (wrong
offset/endianness) causes obscure boot failures. See
[`../recompilation/rexglue-runtime.md`](../recompilation/rexglue-runtime.md).

## 2. The game's global managers (CONFIRMED to exist via RTTI, addresses UNKNOWN)
RenderWare uses **Manager singletons**; recovered ones `[RTTI]`:
- `rw::core::filesystem::Manager` — the VFS (`cache:\`).
- `rw::core::controller::Manager` / `LLManager` — input.
- `rw::audio::core::System` — audio.
- `rw::movie::MoviePlayer` — video.
- (RenderWare graphics/world manager — inferred.)

Plus EA framework singletons (INFERRED): MemoryFramework allocator/categories
(`category.cpp`), OSDK `modulemanager`, the frontend `ResourceKernel`. These are the
"global ownership" roots referenced in
[`../architecture/overview.md`](../architecture/overview.md) §5.

## 3. Gameplay global state (INFERRED)
The on-ice world has a root game-state object (teams, players, puck, clock, score,
officials) that the sim tick mutates and snapshots for the render thread. Its anchor
is likely the `aistruct`/game-session struct created by `cmn/ClubSetup` `[P4]`.
Address UNKNOWN — pin `ClubSetup`/`aistruct` to find it (see
[`function-index.md`](function-index.md) §3).

## 4. Memory categories as a state map (CONFIRMED framework)
MemoryFramework's **categories** (`category.cpp`) are effectively a map of which
subsystem owns which memory. Reading the category list at init would enumerate the
major global allocators/budgets. See
[`../architecture/memory.md`](../architecture/memory.md).

## 5. How to find a global (method)
1. A global is a fixed `.data`/`.bss` guest address loaded via `lis`/`ori` (or
   referenced from many functions).
2. The singletons above are usually a static pointer set once at init and read
   widely — find the init `bl` that stores it, then grep for that address as a load.
3. Cross-reference with the owning module's `[P4]` assert string to name it.

## Open questions (all UNKNOWN addresses)
- The root game-session/world object address.
- Each Manager singleton's storage address.
- MemoryFramework category table location + contents.
- The sim/render snapshot buffer(s) (the threading handoff — see
  [`../architecture/threading.md`](../architecture/threading.md)).

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
