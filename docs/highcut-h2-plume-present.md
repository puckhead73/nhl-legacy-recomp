# H-2 — first plume-driven frame, in-process (DONE ✅)

> Milestone H-2 of the hybrid high cut. Link plume into the live `nhllegacy` target,
> stand up a plume device in-process, and drive a plume swapchain from the intercepted
> guest Present (`sub_827F1C88`). Source: `gpu/hooks/plume_present.cpp`, env-gated
> `NHL_HIGHCUT_PRESENT`. Still pass-through for the game's own presentation (additive).

## Result ✅

`gpu/hooks/plume_present.cpp` owns a plume device + its own Win32 window + swapchain on
a **dedicated thread**, and renders one animated clear per guest Present. The guest
Present hook (in `d3d9_resources.cpp`) only bumps an atomic frame counter — it never
blocks a guest thread. Verified live (menu/attract boot, `NHL_HIGHCUT_PRESENT=1`):

```
[highcut-plume] backend = Vulkan
[highcut-plume] device + swapchain ready: 1280x720, 2 buffers
[highcut-plume] presented plume frame for guest Present 120
[highcut-plume] presented plume frame for guest Present 240
... 360 ... 1200
[nhl-gpu] frame 1451 swap: draws=61 copies(resolves)=2 ... 1280x720   <- game fully alive
```

- plume's swapchain advances in **lock-step with the guest Present count** (120, 240, …)
  — genuinely driven by `sub_827F1C88`.
- The game ran to **1451 swaps** (vs ~1388 H-1 baseline in the same wall-clock) with **no
  slowdown, no device loss, no TDR** — plume coexists cleanly with the live game.

## The key finding: plume must use VULKAN, not D3D12 (here)

rexglue owns a **live, actively-submitting D3D12 device**. Standing up a **second D3D12
device** in the same process triggers a GPU **device reset** — `D3D12CreateDevice`
returns `0x887A0007` (DXGI_ERROR_DEVICE_RESET) and rexglue's device is lost ("Graphics
device lost"). Diagnosed precisely:

- A raw `D3D12CreateDevice` probe on the RTX 4080 **succeeds** (`hr=0`) in-process — so a
  second device is not categorically refused…
- …but ~14 ms later plume's own `D3D12CreateDevice` returns `0x887A0007`: bringing up the
  second D3D12 device/swapchain alongside the busy game **TDRs the GPU**. Reproduced with
  and without the probe, synchronous and threaded — it is a real two-D3D12-device
  conflict on this rexglue + NVIDIA setup, not a timing or clang-build issue (the
  clang-built `gpu/smoke` proves plume's D3D12 device creation works **standalone**).

**Fix:** default plume to its **Vulkan** backend. A plume Vulkan device sits on a
separate driver stack and coexists with the D3D12 game cleanly (the standard
overlay arrangement) — confirmed above. `NHL_HIGHCUT_PRESENT=d3d12` forces the
(conflicting) D3D12 backend for testing; any other value ⇒ Vulkan.

> Implication for later milestones: when the high cut eventually **takes over**
> presentation (rexglue GPU disabled), the D3D12 path becomes viable again because there
> is then only one D3D12 device. Until then (additive/coexisting), Vulkan is the safe
> backend.

## Design notes

- **Dedicated plume thread, deferred init.** The first guest Present fires during the
  game's GPU init (~1.7 s in, at `SetInterruptCallback`); initializing a GPU device on
  the guest thread there is the worst time. The plume thread instead waits until the
  game has issued `kInitAfterGuestFrames` (30) presents, then inits and renders one
  clear per subsequent guest Present. Guest threads are never blocked.
- **Additive, reversible.** We still pass through to the real guest swap, so the game
  presents to its own window as before; plume adds a second window. Default build/run is
  unchanged (gate unset ⇒ `HighcutPlumeTick()` is a no-op).
- **Build.** `add_subdirectory(third_party/plume)` into the `nhllegacy` target; plume
  compiles clean under the recomp's clang++ (MSVC ABI), links `plume d3d12 dxgi dxguid`.
  Vulkan uses volk (dynamic load) — no extra link libs.

## Reproduce

```
_build_beta.bat
$env:NHL_HIGHCUT_PRESENT=1 ; <launch nhllegacy.exe --game_data_root "H:\…\NHL Legacy - Vanilla">
#  a "NHL high-cut (plume Vulkan)" window shows an animated clear, ticked once per game frame.
#  grep "highcut-plume" logs/nhllegacy_*.log   -> backend, swapchain, per-Present frames
```

## Next (H-3)

Render guest draws into **flat, logical-sized** render targets (sized from the H-1
graph) and validate the **fold is gone** on a 3D scene. Here the deferred draw-execution
fork is decided (SDK-D3D12-into-flat-RTs vs reimplement-on-plume).
