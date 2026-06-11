# M0 — License review & vendoring plan (high-cut engine)

> Read-only investigation for plan milestone M0. Decides what we may vendor vs. only reference, and how the
> portable RHI integrates. No third-party code has been pulled in yet.

## License review (decisive)

| Upstream | Repo | License | Decision |
|---|---|---|---|
| **plume** (RHI) | `renderbag/plume` | **MIT** | **VENDOR** — the D3D12/Vulkan/Metal RHI. |
| **XenosRecomp** (shader tool) | `hedge-dev/XenosRecomp` | **MIT** | **VENDOR** — ucode→HLSL→DXIL+SPIR-V. |
| **XenonRecomp** (CPU lifter) | `hedge-dev/XenonRecomp` | (MIT) | Not needed — we use rexglue's lifter. Reference only. |
| **UnleashedRecomp** (app, incl. `gpu/video.cpp`) | `hedge-dev/UnleashedRecomp` | **GPL-3.0** | **REFERENCE ONLY** — do **not** copy code into our permissively-licensed (BSD-3/rexglue) tree. Study the D3D9→RHI architecture and reimplement fresh. |

**Constraint that follows:** the D3D9→plume translation layer (our `gpu/`) must be **clean-room reimplemented** —
read `video.cpp` for the approach (architecture/ideas are not copyrightable), but write our own expression against
plume + our BSD-3 primitives. This is exactly the plan's "reference-only where copyleft" path.

## plume — structure, dependencies, fit

- **What it is:** a low-level RHI = "lowest common denominator" over **D3D12 / Vulkan / Metal**, **bring-your-own
  shader compiler** (we hand it DXIL for D3D12 and SPIR-V for Vulkan from XenosRecomp — perfect fit), C++ with
  `unique_ptr` ownership. Originally built for RT64.
- **Dependencies (all permissive, small tree):** `volk` (Vulkan loader, MIT), `VulkanMemoryAllocator` (MIT),
  `Vulkan-Headers` (Apache/MIT), `D3D12MemoryAllocator` (MIT). No vcpkg requirement for the core RHI.
- **Caveat (real, accepted):** upstream states the API is **not yet stable** — "barrier management and texture
  transitions" still being refined. Acceptable (Unleashed ships on it), but pin to a known-good commit and expect
  to track/patch.

## Proposed vendoring layout (M0c)

```
third_party/plume/                 <- renderbag/plume @ pinned commit (MIT) + its 4 contrib deps
tools/xenos_recomp/                <- hedge-dev/XenosRecomp @ pinned commit (MIT)
gpu/                               <- OUR clean-room D3D9->plume engine (new)
gpu/hooks/d3d9_hooks.cpp           <- REX_HOOK table over the 181 entry points
docs/reference/UnleashedRecomp/    <- (optional) read-only checkout OUTSIDE the build for video.cpp reference;
                                      NOT compiled, NOT redistributed (GPL) — or just browse on GitHub.
```
CMake: `add_subdirectory(third_party/plume)`, link the `gpu` target to plume + `rex::runtime`; XenosRecomp runs
at build time (offline shader translation). Integrates alongside the existing `rexglue_setup_target` wiring in
`CMakeLists.txt`.

## ⚠️ Risk flag: this project is NOT under version control

The environment reports `Is a git repository: false`. Vendoring multiple third-party trees + rewiring CMake on an
**un-versioned** project has no clean revert path. **Recommendation before M0c:** `git init` + an initial commit
(or a manual backup snapshot) so the large structural changes are reversible. (plume's "unstable API" caveat
makes a safety net doubly worthwhile.)

## Vendored (M0c) — pinned, fetched, not committed

Both deps are MIT and fetched at pinned commits by `tools/fetch_thirdparty.ps1`. They are **gitignored** (not
committed) — ~350M is mostly prebuilt binaries (`dxc-bin`) and headers (`Vulkan-Headers`) that don't belong in
our history; the pins below make them reproducible.

| Dep | Path | Pinned commit | Notes |
|---|---|---|---|
| **plume** | `third_party/plume` | `4f556be1531698174a597e7e0a215c22d3238a24` | RHI + contrib: volk, VulkanMemoryAllocator, Vulkan-Headers, D3D12MemoryAllocator. ~57M. |
| **XenosRecomp** | `tools/xenos_recomp` | `990d03b28a27b50277ee5d8d942e1c5f873869d1` | shader tool + thirdparty: smol-v, zstd, xxHash, fmt, **dxc-bin** (prebuilt DXC). ~293M. |

Repair/reproduce: `pwsh tools/fetch_thirdparty.ps1`.

## Status / next

- **VCS safety net: DONE** — `git init` + baseline commit `b25b75c` on `master` (hand-written source only; big
  regenerable/copyrighted artifacts gitignored).
- **M0a/M0b: DONE** (licenses clean, plume fit confirmed).
- **M0c vendoring: DONE** — plume + XenosRecomp fetched at the pins above.
- **Next (M0c build):** wire CMake (`add_subdirectory(third_party/plume)`, new `gpu` target) and stand up a plume
  cleared-window standalone on D3D12 + Vulkan. Then M1: pin the core D3D9 entry points via runtime correlation
  and route the first hooked frame through plume.
