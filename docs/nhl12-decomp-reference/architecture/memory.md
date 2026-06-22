# Memory Model & Ownership

**Status: 🟡 partial.** The allocator framework is CONFIRMED `[P4][RTTI]`; the
concrete heap layout (category sizes, addresses) is UNKNOWN pending a read of
`MemoryFramework` init.

## Confirmed facts

- **Allocator framework:** EA **MemoryFramework 1.10** (`basekit/sys/MemoryFramework`).
  Recovered files: `category.cpp` (memory **categories** — per-subsystem budgets),
  `renderware.cpp` (bridges RenderWare allocation through MemoryFramework). `[P4]`
- **On top of it:** EASTL containers (`eastl::hash_map`, …),
  `EA::Allocator::FixedAllocator` (pooled fixed-size blocks), and
  `ea::movablebuffer::{BufferHandle,BufferReference}` (relocatable buffers — implies
  a **compacting/movable heap** for some asset memory). `[RTTI]`
- **Endianness:** big-endian. All serialized structures, asset headers, and the
  `.big` TOCs are BE. The recomp byte-swaps every guest memory access. `[ASM]`
- **Address space:** guest is a flat 4 GiB space; the recomp reserves 4 GiB so a
  guest address `A` is simply `base + A` on the host. The real 360 had 512 MB
  unified RAM. `[ASM]`

## Inferred structure

EA titles of this era partition memory into **named categories** with fixed
budgets set at boot (so the game never fragments past its console budget). Typical
categories (INFERRED): Rendering, Audio, Animation, AI/Gameplay, Frontend/UI,
AssetStream/IO, Network, Debug. `category.cpp` is where these are declared.

The `movablebuffer` primitive suggests **streamed assets live in a relocatable
pool** that can be compacted as assets load/unload — consistent with the heavy
`cache:\`-streaming design (4+ GB of `*render.big`/`nocache*.big`).

## Recomp implications (CONFIRMED relevance)

- **64 KB page assumptions.** The 360 uses 4 KB *and* 64 KB pages;
  `MmQueryAddressProtect`/`NtAllocateVirtualMemory` paths may assume large pages.
  Watch alignment when emulating `Mm*`/`Nt*` virtual-memory imports. `[IMP]`
- **Guest pointers are 32-bit.** Stored pointers are 4 bytes, BE. The recomp keeps
  the guest in the low 4 GiB so 32-bit guest pointers round-trip. `[ASM]`
- **No host pointer may leak into guest memory** at a width the game will
  re-interpret — runtime-provided structs (the 13 data-variable import slots:
  `XboxHardwareInfo`, `KeTimeStampBundle`, …) must be laid out with 360 field
  offsets/endianness. `[IMP]`

## Open questions
- Total heap size requested at boot and per-category budgets (read `category.cpp`'s
  init / the first big `NtAllocateVirtualMemory`).
- Whether the movable pool is GC'd on a thread or inline at load.
- Stack sizes per engine thread (the `ExCreateThread` args).

See [`../unknowns/open-questions.md`](../unknowns/open-questions.md) and the recomp
memory notes in [`../recompilation/rexglue-runtime.md`](../recompilation/rexglue-runtime.md).
