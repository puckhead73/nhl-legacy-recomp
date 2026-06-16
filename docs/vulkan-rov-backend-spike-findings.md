# SPIKE findings — SDK native Vulkan ROV backend as the renderer foundation

**Verdict: GO — PARITY ACHIEVED (2026-06-16). The SDK Vulkan `fsi` backend renders NHL Legacy at
parity with the high-cut renderer's correctness (lighting, jersey numbers/names, equipment all
verified by the user in live gameplay), at ~20× the framerate (66–84 fps dense gameplay vs high-cut's
~3 fps), with far less code. Reached via four small source/cvar fixes diffed against our high-cut
code (NOT the NHL12 docs): (1) numbers — `exp_adjust` read from fetch-constant word 3 not word 4;
(2) goalie equipment — `readback_resolve=full` cvar; (3) equipment normals — signed BC5_SNORM for
k_DXN; lighting cleared once (1) landed. SDK patches captured in docs/rexglue-vulkan-nhl-legacy.patch;
porting guide in docs/vulkan-migration-plan.md.**

_History of the verdict: started GO-leaning (built the Vulkan SDK, title screen rendered), then hit
two correctness gaps (cartoonish lighting, missing numbers). A wrong turn — porting NHL12-reference
fixes (a different game) — did nothing. The fix was to diff our own proven-correct high-cut renderer
against the SDK source, which surfaced the real verified bugs above. → full parity GO._
Date: 2026-06-16. Branch: `highcut-c5g-jersey-numbers`. Companion to
[vulkan-rov-backend-spike-prompt.md](vulkan-rov-backend-spike-prompt.md).

> **History note.** This doc first concluded a HARD NO-GO because the *prebuilt* SDK zips
> (0.8.0 and 0.8.1.31-dev) ship the Vulkan backend OFF (`REXGLUE_USE_VULKAN OFF`). That was
> correct *for those binaries*. The source repo is public, so we rebuilt with the flag ON and the
> blocker dissolved. The original NO-GO analysis is preserved below for the record; the **Phase A
> results** section at the bottom is the current state.

---

## TL;DR

The rexglue SDK we link is a **binary-only, D3D12-only distribution**. The Vulkan ROV/EDRAM
backend ships **as headers only** (`rex/graphics/vulkan/*.h`) — its *implementation* (the
`VulkanGraphicsSystem`/`VulkanCommandProcessor` ctors, the SPIR-V translator, the
`render_target_path_vulkan` cvar) is **not compiled into the runtime DLL**, is **not in any
separate lib**, and there is **no rexglue source** to rebuild it from. The recomp therefore
cannot link, let alone drive, the SDK Vulkan backend. This invalidates the spike prompt's
"decisive finding #1" at the binary level: the *headers* are present, but the *backend* is not.

No game was run — the link failure made it unnecessary and impossible.

---

## What was tested (Phase A, steps 1–2)

1. Added the opt-in env gate `NHL_VK_BACKEND` in [src/nhllegacy_app.h](../src/nhllegacy_app.h)
   `OnPreSetup`: when set, instantiate stock `rex::graphics::vulkan::VulkanGraphicsSystem` and
   `REXCVAR_SET(render_target_path_vulkan, "rov")` (overridable via `NHL_VK_RT_PATH`), else the
   existing `NhlD3D12GraphicsSystem`. Added `#include <rex/graphics/vulkan/graphics_system.h>`.
   **Compiled cleanly** — the headers resolve.
2. Built with `_build_beta.bat`. **Link failed** (`BUILD_EXIT=1`) with two undefined symbols:
   - `rex::graphics::vulkan::VulkanGraphicsSystem::VulkanGraphicsSystem(void)`
   - `FLAGS_render_target_path_vulkan_storage_(void)` (the cvar storage)

The change was **reverted**; the tree rebuilds clean (`BUILD_EXIT=0`) and is byte-for-byte the
prior committed state. The spike was non-destructive as designed.

---

## Root cause (binary evidence)

The build links `rexruntimerd.dll` from **`E:/Tools/rexglue-sdk/0.8.0/win-amd64`**
(`CMAKE_PREFIX_PATH` in CMakeCache). Inspected both 0.8.0 **and** 0.8.1.31-dev (the version the
prompt's NHL12 claim referenced):

| Probe | D3D12 (positive control) | Vulkan |
|---|---|---|
| `dumpbin /EXPORTS` symbol hits | `D3D12GraphicsSystem` → **9** | `*Vulkan*` → **0** (both versions) |
| DLL string scan (ASCII+UTF-16) | `render_target_path_d3d12` → **present**, `D3D12GraphicsSystem` → **present** | `render_target_path_vulkan` → **absent**, `VulkanGraphicsSystem`/`VulkanCommandProcessor` → **absent**, `SpirvShaderTranslator`/`spirv` → **absent**, `vkCreateInstance` → **absent** |
| `dumpbin /DEPENDENTS` | — | no `vulkan-1.dll` / `vk_*` import |

So the runtime contains the **D3D12** graphics system and its cvars, and zero Vulkan code or
even a Vulkan loader import. Both shipped SDK versions are D3D12-only builds.

Searched for an alternative source of the implementation — there is none:
- No separate Vulkan `.lib`/`.dll` in `lib/` or `bin/` (only `rexruntime*`, SDL3, fmt, etc.).
- No rexglue **source tree**: the CMakeCache "rexglue-sdk source tree" entry is an empty comment;
  rexglue is consumed as an installed binary package (`rexglue_DIR=.../lib/cmake/rexglue`). No
  `.cpp` exists anywhere under the SDK — only headers.

### Definitive cause: built with the Vulkan option OFF (not missing — *disabled*)

`E:/Tools/rexglue-sdk/0.8.1.31-dev/win-amd64/lib/cmake/rexglue/rexglueConfig.cmake` records:

```cmake
set(REXGLUE_USE_VULKAN OFF)
if(REXGLUE_USE_VULKAN) ...
```

**Vulkan is a compile-time option in rexglue**, and both published win-amd64 builds (0.8.0 and
0.8.1.31-dev, version `0.8.1.31-dev.gbd9b519`) ship with it **OFF**. That is the whole story: the
headers are always installed, the backend is conditionally compiled, and the public builds disable
it. The pkgconfig prefix `D:/a/rexglue-sdk/rexglue-sdk/out/install/win-amd64` is a GitHub-Actions
runner path → these SDK zips are CI artifacts of a `rexglue-sdk` repo. `nightly.zip` in the SDK
root **is** 0.8.1.31-dev (identical 13.3 MB DLL) — no newer/Vulkan build hides there.

### The real GO prerequisite (precise ask)

A rexglue build compiled with **`REXGLUE_USE_VULKAN=ON`** for win-amd64 — obtainable only by:
1. the SDK maintainer publishing a Vulkan-enabled CI artifact (also needs the Vulkan SDK +
   glslang/SPIRV-Tools present at rexglue build time), **or**
2. access to the `rexglue-sdk` **source repo** to build it ourselves with that flag.

Neither exists locally. Switching our build to 0.8.1.31-dev does NOT help — it is also OFF. The
NHL12 team's fast build was simply rexglue with this flag ON; the NO-GO is a missing build flag,
not an architectural wall.

---

## Why the prompt's premise didn't hold

The spike was built on header inspection of `rex/graphics/vulkan/*.h` (public ctors, `IsAvailable()
→ true`, the `render_target_path_vulkan` cvar) plus the NHL12-build claim of 450–1800 fps. The
headers are real, but a header is a promise, not an implementation. The **shipped binaries** these
headers belong to were built without the Vulkan backend. Whatever build the NHL12 team ran at
450–1800 fps was a **Vulkan-enabled rexglue build we do not possess** (and cannot reproduce
without rexglue source + the SDK's own Vulkan build switch).

Finding #2 of the prompt (the default D3D12 **ROV** path already renders the real game faithfully
in our tree, [src/nhllegacy_app.h:99](../src/nhllegacy_app.h#L99)) **stands** — but it is the
*D3D12* ROV path, which we already use. It is not evidence the Vulkan path is reachable.

---

## The only GO path, and why it's out of spike scope

To pursue Vulkan we would need to **rebuild rexglue from source with its Vulkan backend enabled**
and re-vendor the SDK. That requires (a) obtaining rexglue source (not present locally; the SDK is
binary-only), (b) finding/flipping the SDK's Vulkan build option, (c) rebuilding all
configurations (release/debug/relwithdebinfo) and re-pointing `CMAKE_PREFIX_PATH`. That is an
SDK-engineering effort, not the "one-line, opt-in, cheap, non-destructive backend swap" this spike
was scoped to be. It should be its own decision with its own prompt, gated on whether rexglue
source is even available to us.

---

## Recommendation

- **Do not** pursue the SDK Vulkan backend further on the current binary SDK — it is unreachable.
- **Keep the high-cut plume path** and, if the perf ceiling matters, resume its grind (consumer
  buffer/descriptor pooling, dynamic-texture-by-address reuse, `texCache` OOM bound) per
  `highcut-live-takeover-freeze-fix` in auto-memory.
- If a faithful+fast+portable foundation is still wanted, the realistic prerequisite question is
  **"can we get rexglue source / a Vulkan-enabled SDK build?"** Answer that first; the
  one-line-swap spike is moot until then.

## Reproduce / verify (read-only, ~1 min)

```powershell
$dumpbin = (gci "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe")[0].FullName
$dll = "E:\Tools\rexglue-sdk\0.8.1.31-dev\win-amd64\bin\rexruntimerd.dll"
& $dumpbin /EXPORTS $dll | Select-String "Vulkan"          # -> 0 hits (D3D12GraphicsSystem -> 9)
$a = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll))
$a.Contains("render_target_path_vulkan"); $a.Contains("SpirvShaderTranslator")   # -> False, False
$a.Contains("render_target_path_d3d12")                                          # -> True (control)
```

---

# Phase A RESULTS (2026-06-16) — blocker resolved, renders correctly

## How the blocker was removed
- Source repo is public: `github.com/rexglue/rexglue-sdk` (BSD-3). `REXGLUE_USE_VULKAN` defaults
  **OFF on Windows**, ON on Linux — the reason every prebuilt win-amd64 zip lacks Vulkan.
- Cloned, checked out commit **`bd9b519`** (`git describe` = `nightly-20260603-...-gbd9b519` =
  our linked `0.8.1.31-dev.gbd9b519`, exact API match), `git submodule update --init --recursive`.
- **Windows symlink gotcha:** `core.symlinks=false` wrote 15 `libmspack` symlinks
  (`cabextract/mspack/*.c|*.h` → `../../libmspack/mspack/*`) as text stubs → clang
  `expected identifier` at `lzxd.c:1`. Fix = materialize them (copy each target over the stub;
  `git ls-files --recurse-submodules -s` → mode `120000`). One-time, the maintainer's Linux CI
  never hits it.
- Build (inside vcvars64 + LLVM on PATH):
  `cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=ON -DREXGLUE_BUILD_TESTS=OFF`
  → `Graphics: D3D12=ON Vulkan=ON`; then `cmake --build --preset win-amd64-relwithdebinfo
  --target install`. Install at `E:\Tools\rexglue-sdk\src\out\install\win-amd64`. All Vulkan deps
  are vendored (`thirdparty/`: glslang, spirv-tools, vulkan-headers, VMA, **volk**) — volk = no
  system Vulkan SDK needed to build. Scripts: `src/_configure_vk.bat`, `src/_build_vk.bat`.
- Verified the new `rexruntimerd.dll` (17.5 MB vs 13.3 D3D12-only) **exports** `VulkanGraphicsSystem`
  (8), `VulkanCommandProcessor` (128), the `render_target_path_vulkan` cvar, and `SpirvShaderTranslator`.

## nhllegacy wiring (opt-in, non-destructive)
- `src/nhllegacy_app.h`: `#ifdef NHL_HAVE_VULKAN_BACKEND` gate — when env `NHL_VK_BACKEND` is set,
  `config.graphics = make_unique<vulkan::VulkanGraphicsSystem>()` +
  `REXCVAR_SET(render_target_path_vulkan, NHL_VK_RT_PATH | "rov")`.
- `CMakeLists.txt`: `option(NHLLEGACY_VULKAN_BACKEND OFF)` → defines `NHL_HAVE_VULKAN_BACKEND`.
- Separate build dir `out/build/win-amd64-vk` (CMAKE_PREFIX_PATH → the Vulkan SDK); the default
  `out/build/win-amd64-relwithdebinfo` (D3D12, 0.8.0) is **untouched** and still builds clean.
  Driver: `_build_vk.bat {configure|build}`. Run probes: `_vktest.ps1`, `_vklongshot.ps1`.

## Run result (`NHL_VK_BACKEND=1`, no NHL_BACKEND/NHL_HIGHCUT, RT path = rov)
- Gate fired: `[nhl-vk-spike] Vulkan backend selected (render_target_path_vulkan=rov)`.
- `Vulkan instance API 1.4`, device = **NVIDIA RTX 4080 SUPER**, `VK_KHR_swapchain`.
- `VulkanPresenter: Created 1920x1080 swapchain`; `GPU system initialized (presentation=true)` —
  **the SDK's own presenter drives the window; no plume needed.**
- Xenos→SPIR-V translation + Vulkan pipeline creation working (`Creating graphics pipeline state
  with VS … PS …`), FE + roster DBs load (`db-open fe\loc\…`, `db\nhlng.db`).
- **Renders correctly:** by ~70 s the title screen is faithful — dark arena + stadium-light bokeh,
  "LEGACY EDITION", "PRESS START", "© 2015 Electronic Arts Inc." (`out/build/win-amd64-vk/vkrov_t130.png`).
- The transient green/red spiky frames at ~40–45 s are the **early-boot/intro 3D stage** — the
  known-good **D3D12 baseline shows the identical thing at 45 s** (`d3d12_baseline_shot.png`), so it
  is boot-stage, NOT a Vulkan defect.
- Stable 130 s, no crash, **no Vulkan validation errors**. "Skipping Vulkan frame presentation due
  to async placeholder draw" fired **once** at early boot (benign; cvar `vulkan_async_skip_incomplete_frames`).

## Still open (interactive / next session)
- **fps number**, especially **dense in-game** (the high-cut path's ~3 fps wall). No fps-overlay
  cvar in the SDK; `vsync` is forced on (caps 60). Needs Tracy (REXGLUE_ENABLE_TRACY=ON, TracyClient
  shipped) or a human watching, AND navigating PRESS START → Play Now into live gameplay.
- **Phase B** — subclass `VulkanGraphicsSystem`/`VulkanCommandProcessor` (128 exports available),
  confirm the `IssueDraw`/texture/shader hook surface for the enhancement vision; internal-res via
  `resolution_scale`/`draw_resolution_scale_*` cvars; FidelityFX is a build option (needs a rebuild
  with `REXGLUE_ENABLE_FIDELITYFX=ON`).
