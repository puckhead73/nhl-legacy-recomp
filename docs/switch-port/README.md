# Nintendo Switch (Horizon) port — roadmap

The goal is to run the recompiled NHL Legacy natively on Horizon, the Switch OS,
as homebrew (devkitA64 + libnx under Atmosphère).

## Why staged

The game's code ports easily: it is ReXGlue-generated C++, and its VMX128 math
goes through SIMDe, which builds as NEON. The platform layer and the GPU are the
hard parts:

| Blocker | Why it's hard on Horizon |
|---|---|
| Renderer | Homebrew Horizon has no Vulkan driver. The renderer is the SDK's Vulkan backend (`fsi` path) and translates Xenos shaders to SPIR-V at runtime. It needs a deko3d (or devkitPro OpenGL/mesa) backend plus an on-device shader compiler. |
| Guest address space | The runtime reserves 4 GB with `mmap` and maps the same physical memory at several addresses with `shm_open` (the 0xA0000000+ physical mirrors). Horizon has neither, so the options are `svcMapProcessMemory` on our own process or codegen folding the mirrors. |
| Write watches / MMIO | The runtime catches page faults (SIGSEGV) to detect GPU register writes and physical-memory writes. Horizon has no POSIX signals, so this needs the libnx exception handler or codegen changes. |
| CPU budget | 3 usable A57 cores at 1–2 GHz, against a desktop that does 30–84 fps. Title-takeover mode is needed for the RAM. |
| Windows-only extras | FidelityFX, the `ffmpeg.exe` subprocess (Horizon has no subprocesses, so FFmpeg is linked in), D3D12/plume. |

## Stages

1. **Linux aarch64 build.** Status: done up to codegen.
   See [linux-arm64-build.md](linux-arm64-build.md). It checks the code on
   ARM64 and measures speed on the actual Switch CPU (L4T) before any
   Horizon-specific work.
2. **Boot and profile on Switch L4T** with real codegen output. Go/no-go on
   speed; tune `-mcpu=cortex-a57`, LTO and PGO.
3. **Horizon platform layer for the SDK** (`REX_PLATFORM_HORIZON`): memory
   reserve/alias, exception handling, threads, fibers, clocks, file paths
   (sdmc:/romfs), using a devkitA64 CMake toolchain. `src/platform/host_compat`
   gets a Horizon branch (`VirtualQuery` → `svcQueryMemory`).
4. **Headless boot on Horizon** (null GPU) to check the CPU side and get early
   speed numbers.
5. **deko3d graphics backend.** This is most of the work.
6. **Audio, input and movies** through SDL3 or libnx (audren/hid) and a
   statically linked FFmpeg for the VP6 bridge.
