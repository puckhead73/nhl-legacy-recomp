# Linux / ARM64 build (Switch port, stage 1)

This is the first stage of the Nintendo Switch port. It produces a Linux
**aarch64** build of the port: the same recompiled game, the SDK's Vulkan
renderer, and the SDL3 window, input and audio.

It does two things before any Horizon-specific work starts:

1. **It checks the recompiled code on ARM64.** The recompiled code is C++ whose
   vector (VMX128) math goes through SIMDe, which builds as NEON on aarch64.
2. **It shows real speed on the Switch's own CPU** (4x Cortex-A57). It runs
   on a Switch booted into Linux (L4T Ubuntu), which has a real NVIDIA Vulkan
   driver, or on any ARM64 Linux board.

The later stages are a Horizon (libnx) platform layer for the SDK, then a
deko3d/OpenGL renderer. They are tracked in [`README.md`](README.md).

## What changed in the tree for this

| Area | Change |
|---|---|
| `CMakeLists.txt` | New `NHLLEGACY_D3D12` option (default ON only on Windows). It controls the beta D3D12 backend (`renderer/core/nhl_command_processor.cpp`, `nhl_graphics_system.cpp`), the high-cut plume path (`gpu/hooks/plume_present.cpp` with plume, dxc and d3d12 libs) and the path-C SPIR-V translator port (`gpu/spirv/*`). When it is OFF, `NHLLEGACY_VULKAN_BACKEND` defaults ON and is required. Snappy is linked by platform naming. A fresh checkout with no `generated/` falls back to `cmake/rexglue_fallback.cmake`. |
| `src/platform/host_compat.{h,cpp}` | Off Windows, this provides `VirtualQuery` (from `/proc/self/maps`), `_byteswap_*`, `_stricmp`, `Sleep`, `GetTickCount`, `SetEnvironmentVariableA` and a stub `GetAsyncKeyState`. It also adds `nhl_host::HardExitProcess` / `SetProcessEnv`, which replace `TerminateProcess(GetCurrentProcess())` / `_putenv_s` in `nhllegacy_app.h`. On Windows it just includes `<windows.h>`. |
| Diagnostic scanners | `overall_weights_dump.h`, `stick_list_scan.h`, `tunable_registry_dump.h`, `anim_decode.cpp` and `diag_hooks.cpp` now use the shim, so they work on Linux as well. The `NHL_VP6_TRAP` write-trap stays Windows-only because it needs SEH, `VirtualProtect` and the x64 `CONTEXT`. |
| `src/vp6_bridge.cpp` | Off Windows, `ffmpeg` is started with `posix_spawnp` and a pipe. The command line is read from `/proc/self/cmdline`. Host paths use `/`. |
| `src/cpu_affinity.cpp` | No-op off Windows. The Intel P/E-core issue does not apply, and the Tegra X1 has no hybrid cores. |
| `renderer/core/nhl_vk_backend.cpp` | The F9 capture hotkey is Windows-only for now (stub elsewhere). |
| `src/nhllegacy_app.h` | The D3D12 fallback and the `d3d12_*` cvars are gated on `NHL_HAVE_D3D12_BACKEND`. Vulkan is always selected when there is no D3D12. |
| `cmake/toolchains/linux-aarch64-clang.cmake` | Cross toolchain: clang 20 (or 19) with lld, a Debian multiarch or L4T sysroot, `-mcpu=cortex-a57`, and qemu-user as the cross-compiling emulator. |
| `scripts/setup_sdk.sh`, `scripts/build_linux.sh` | Bash versions of `setup_sdk.ps1` and the `.bat` build drivers. |

The SDK itself needed **no source changes**. The pinned `bd9b519` already has
`linux-arm64` support (SIMDe, POSIX memory/threads/fibers, SDL3).

## Prerequisites

### Native ARM64 host (Switch L4T Ubuntu 24.04, or any arm64 distro)

```bash
sudo apt install git cmake ninja-build clang-20 lld-20 llvm-20 python3 pkg-config \
  libgtk-3-dev libx11-xcb-dev libxcb1-dev libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxkbcommon-dev \
  libwayland-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
  libunwind-dev libusb-1.0-0-dev libpipewire-0.3-dev libdrm-dev libgbm-dev \
  libegl-dev libgl-dev libgles-dev
```

### Cross-compiling from x86_64 Ubuntu 24.04

Add arm64 multiarch. Limit the stock sources to amd64, then add the arm64 ports
mirror:

```bash
sudo dpkg --add-architecture arm64
```

In `/etc/apt/sources.list.d/ubuntu.sources`, add `Architectures: amd64` under
each `Types: deb` stanza. Then create
`/etc/apt/sources.list.d/ubuntu-ports-arm64.sources`:

```
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports/
Suites: noble noble-updates noble-security
Components: main restricted universe multiverse
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
```

```bash
sudo apt update
```

```bash
sudo apt install clang-20 lld-20 llvm-20 binutils-aarch64-linux-gnu qemu-user-static \
  libc6-dev:arm64 libstdc++-13-dev:arm64 libgcc-13-dev:arm64 \
  libgtk-3-dev:arm64 libx11-xcb-dev:arm64 libxcb1-dev:arm64 libx11-dev:arm64 \
  libxext-dev:arm64 libxrandr-dev:arm64 libxcursor-dev:arm64 libxi-dev:arm64 \
  libxss-dev:arm64 libxfixes-dev:arm64 libxkbcommon-dev:arm64 libwayland-dev:arm64 \
  libasound2-dev:arm64 libpulse-dev:arm64 libudev-dev:arm64 libdbus-1-dev:arm64 \
  libunwind-dev:arm64 libusb-1.0-0-dev:arm64 libpipewire-0.3-dev:arm64 \
  libdrm-dev:arm64 libgbm-dev:arm64 libegl-dev:arm64 libgl-dev:arm64 libgles-dev:arm64
```

**Clang 19 or newer is required.** With libstdc++, `<expected>` is hidden from
clang 18, because clang 18 reports `__cpp_concepts` 201907L. The SDK uses
`std::expected` (`rex/system/map_parser.h`).

## Build

```bash
scripts/setup_sdk.sh
```

```bash
scripts/build_linux.sh arm64 Release sdk
```

On a native arm64 host the same command builds natively.

`setup_sdk.sh` puts the SDK in `../rexglue-sdk`, a sibling of the repo, not
`third_party/`. Override that with an argument, or with `REXGLUE_SDK_SRC` for
both scripts. The reason is in the codegen warning below.

### Codegen (needs your vanilla `default.xex`)

Point `nhllegacy_manifest.toml` at your dump, as in BOOTSTRAP.md. Then:

```bash
scripts/build_linux.sh arm64 Release game
```

```bash
cmake --build out/build/linux-arm64-vk --target nhllegacy_codegen_emu
```

```bash
scripts/build_linux.sh arm64 Release game
```

The first `game` pass only configures. Its compile then fails because
`generated/default/nhllegacy_init.h` does not exist yet. Codegen writes
`generated/`, and the second pass compiles the recompiled sources and links.

- **Cross build (x86_64 host):** use `nhllegacy_codegen_emu`. It runs the
  aarch64 `rexglue` under qemu-user. CMake does not apply
  `CMAKE_CROSSCOMPILING_EMULATOR` to the SDK's imported `rex::rexglue`, so the
  stock `nhllegacy_codegen` target would try to run the ARM binary directly.
- **Native arm64 host:** use the stock `nhllegacy_codegen` target.

> **Codegen migration prompt: answer `N` if it lists SDK files.** The
> manifest says `sdk_version = "0.8.1"` while the pinned SDK reports
> `0.8.1.31-dev`, so `rexglue codegen` runs a migration scan over the whole
> project tree first. If the SDK lives inside the repo (for example
> `third_party/rexglue-sdk`, the `setup_sdk.ps1` default), the scan proposes
> rewriting the SDK's own `src/rexglue/commands/migration_scan.cpp`, which holds
> the legacy identifiers as string literals. Accepting would corrupt the SDK.
> Declining aborts codegen. Keep the SDK outside the repo, as
> `setup_sdk.sh` does. This was observed under qemu with no XEX present, so the
> scan and prompt come before the XEX is loaded. Non-interactive stdin counts as
> "no".

The generated C++ should not depend on the host. Platform differences such as
`REX_PHYS_HOST_OFFSET` are macros resolved at compile time in
`nhllegacy_init.h`. So an existing Windows `generated/` tree from the same SDK
pin and manifest should be reusable. This has not been verified yet.

Run on the device:

```bash
out/build/linux-arm64-vk/nhllegacy --game_data_root /path/to/game
```

## Status (2026-11-26)

- **SDK `bd9b519` + title patches: builds for aarch64** (Release, Vulkan ON,
  Tracy OFF) with the toolchain above and no SDK source changes.
- **Game host code: compiles and links for aarch64**, apart from codegen output.
  This was checked against a stand-in `nhllegacy_init.h` that declares only the
  guest functions the host code references, because the XEX is not in the repo.
  All 21 host translation units compile. The final link's only unresolved
  symbols are the 39 hooked or called guest functions (`__imp__sub_*`) and
  `PPCImageConfig`, all of which codegen provides.
- **The aarch64 `rexglue` tool runs** (under qemu-user) and reaches the
  manifest/migration stage.
- **The `VirtualQuery` shim** is unit-checked natively and as aarch64 under
  qemu: reserved, committed and free regions, and region walking.
- **x86_64 Linux** (`build_linux.sh amd64`) is wired up but not built yet.
- **Not yet run on hardware.** The next step is a boot on L4T with the real
  `generated/` tree, then fps numbers.

## Known gaps and risks to check on hardware

- **Page size.** `kernel/memory.cpp` checks that the host page size matches how
  the build applies the 0xE0000000 physical offset. L4T and most distros use
  4 KiB pages. 16K/64K-page kernels (Asahi, some server distros) will fail that
  check.
- **Physical-memory write watches** use `mprotect` and SIGSEGV
  (`exception_handler_posix.cpp`). This is fine on Linux, but it is the main
  piece the Horizon layer must replace.
- **FSI (fragment shader interlock).** The `fsi` RT path needs
  `VK_EXT_fragment_shader_interlock`. Check `vulkaninfo` on L4T for the Tegra
  X1 (Maxwell GM20B). Without it the in-game 3D renders black (see
  `docs/current-status.md`).
- **VP6 movies.** The bridge needs an `ffmpeg` on `PATH` or `NHL_VP6_FFMPEG`.
  It also needs to know which movie the guest opened. That comes from
  `NHL_VP6_LAST_OPEN_HOST` (loose-tree opens) or from SDK-side
  `NHL_VP6_LAST_OPEN`. The SDK side exists only in
  `sdk/rexglue-vp6-sharedmem-fixes.patch`, which `setup_sdk.*` does not apply
  because it overlaps the title patch. Movie corruption fixes therefore depend
  on the loose-tree install layout.
- **FidelityFX** is off (Windows-only DLL). Presentation is bilinear only.
- **The `.dds` -> `.rx2` loose-texture override** needs the Windows-only
  `tdb_rx2_ffi.lib` and is disabled elsewhere, as it already was on Windows
  without the lib.
- **Debug hotkeys** (F9 capture, F10/F11 on the beta path) are not wired off
  Windows.
