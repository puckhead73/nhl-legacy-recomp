# Kickoff — SPIKE: evaluate the SDK's native Vulkan ROV backend as the renderer foundation

**Use this as the opening prompt for a fresh session. This is a time-boxed SPIKE (investigation), not a
commitment to rewrite. Goal: a confident GO / NO-GO on replacing the hand-rolled high-cut plume renderer
with the rexglue SDK's own Vulkan ROV backend.**

---

## 0. TL;DR

The rexglue SDK we link **already ships a complete Vulkan ROV/EDRAM backend** (`rex/graphics/vulkan/*`).
A parallel reverse-analysis of the NHL 12 team's WIP build proved that *this same backend* runs the real
game faithfully at **450–1800 fps** (RTX 4080) via fragment-shader-interlock EDRAM. Meanwhile our
hand-rolled high-cut **plume** path (decode→packet→bridge→rebuild every draw every frame) is stuck at
**~3 fps** in dense gameplay — an architectural wall, not a tuning problem.

**Spike question:** can the recomp drive the SDK's Vulkan backend end-to-end (boot→menu→gameplay),
rendering NHL Legacy correctly and fast, and does subclassing it expose enough hook surface for the
enhancements the project wants? If yes, it likely **obsoletes the entire high-cut plume perf treadmill**
and gives a faithful + fast + portable foundation for far less code.

**This spike is cheap and non-destructive:** it's an opt-in graphics-system selection (env-gated). It does
not modify the committed high-cut work.

---

## 1. Where we are (committed state)

The high-cut live-takeover path now **renders the live game continuously** (was frozen/crashing). Recent
commits on branch `highcut-c5g-jersey-numbers`:
- `fd25f18` by-ID resource cache (fixed the dense-gameplay freeze/crash)
- `e0ff2b1` perf pass (untile prefix hash + buffer floor)
- `cd72c01` batch consumer texture uploads (one fence wait/frame)
- `20b1717` address-keyed producer untile cache (bounded; kills thrash)

Measured ceiling: **~3 fps** at ~1300–1600 draws. Breakdown (`NHL_HIGHCUT_PROFILE=1`,
`[highcut-perf]`): producer `untile≈197ms` (re-decoding per-frame-dynamic textures — bone palettes — an
inherent cost of re-decoding every frame) + consumer rebuild `≈0.15ms/draw ≈ 270ms` (per-draw buffer +
descriptor-set creation). Both are intrinsic to "rebuild every draw every frame across a thread bridge."
Squeezing further (buffer pooling, dynamic-texture redesign) might reach single-digit fps — not interactive.

See [[highcut-live-takeover-freeze-fix]] in auto-memory for the full perf log.

---

## 2. The decisive finding (verified facts)

1. **The SDK ships the whole Vulkan backend.** Headers present under
   `E:\Tools\rexglue-sdk\0.8.1.31-dev\win-amd64\include\rex/graphics/vulkan/`:
   `command_processor.h`, `graphics_system.h`, `render_target_cache.h` (ROV/EDRAM), `pipeline_cache.h`
   (async PSO), `primitive_processor.h`, `shader.h`, `shared_memory.h`, `texture_cache.h`,
   `deferred_command_buffer.h`.
   - `class VulkanGraphicsSystem : public GraphicsSystem` — public ctor; overrides
     `CreateProvider(bool with_presentation)` and `CreateCommandProcessor()`; `IsAvailable()` → true.
   - `class VulkanCommandProcessor : public CommandProcessor` — owns its own
     `rex::ui::vulkan::presenter` / `provider` / `upload_buffer_pool` (i.e. **the SDK handles Vulkan
     presentation itself** — no plume window needed).
   - cvar `render_target_path_vulkan` exists (set to the ROV/`fsi` path), plus `resolution_scale`,
     `draw_resolution_scale_x/y`, `native_2x_msaa` — so **internal-res enhancement is built in**.

2. **Faithful EDRAM already works in our tree on D3D12.** [src/nhllegacy_app.h:99](../src/nhllegacy_app.h#L99)
   sets `render_target_path_d3d12="rov"` with the note *"Accurate EDRAM path (in-game scene renders black
   under the rtv path)."* So the **default** (non-beta) recompiled game already renders the real game via
   the SDK's D3D12 ROV backend. **Basic rendering/faithfulness is NOT the open problem** — the ~3 fps is
   purely the hand-rolled high-cut plume path. (Confirm the default path's fps early in the spike — it is
   the strongest evidence the Vulkan ROV path will be fast too.)

3. **The fold premise is moot.** The high-cut pivot ([[high-cut-pivot-decision]]) moved away from EDRAM to
   escape the wide-RT *fold* — but that fold was an artifact of the D3D12 *flat-texture* EDRAM emulation,
   and [[tiling-verdict-no-game-tiling]] showed the game does no predicated tiling. The Vulkan **ROV** path
   does faithful per-tile EDRAM (handles pitch<width correctly), so the fold should not recur — same as the
   D3D12 ROV path already in use.

4. **The swap point is one line.** [src/nhllegacy_app.h:85](../src/nhllegacy_app.h#L85):
   `config.graphics = std::make_unique<nhl::graphics::NhlD3D12GraphicsSystem>();`. We currently subclass
   the SDK's **D3D12** graphics system ([renderer/core/nhl_graphics_system.cpp](../renderer/core/nhl_graphics_system.cpp));
   we can instantiate / subclass the **Vulkan** one instead.

---

## 3. Hypothesis to test (the GO/NO-GO)

> The SDK's Vulkan ROV backend can render NHL Legacy faithfully and fast under our recomp, and subclassing
> `VulkanCommandProcessor` exposes the same kind of hook surface (`IssueDraw`, texture/shader access) the
> beta D3D12 path uses — making it a better foundation for the enhanceable live renderer than high-cut plume.

If true → pivot the enhancement foundation to the SDK Vulkan backend; retire/repurpose the high-cut plume
stack. If false → keep high-cut and resume its perf grind (pooling, dynamic-texture reuse).

---

## 4. Spike steps (in order; stop at the first hard NO-GO)

**Phase A — Stock backend bring-up (cheapest go/no-go; ~½–1 day).**
1. Add an env gate `NHL_VK_BACKEND` in [src/nhllegacy_app.h](../src/nhllegacy_app.h#L80) `OnPreSetup`:
   when set, `config.graphics = std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>();`
   (stock, no subclass yet) and set `REXCVAR_SET(render_target_path_vulkan, std::string("rov"))` (try
   `"fsi"` too) **instead of** the D3D12 graphics system + `render_target_path_d3d12`. Leave the default
   (unset) path exactly as today. Include `<rex/graphics/vulkan/graphics_system.h>`; link should already
   resolve (same static lib).
2. Build (`_build_beta.bat`). Run **without** any `NHL_BACKEND=beta` / `NHL_HIGHCUT_*` env — just the plain
   recompiled game on the Vulkan backend.
3. **Answer:** does it boot → intro → menu → gameplay and render correctly? At what fps? (Compare to the
   default D3D12 ROV path on the same scenes.) Watch the SDK Vulkan presenter's own window.

**Phase B — Enhancement hook surface (only if A renders; ~1 day).**
4. Subclass `vulkan::VulkanGraphicsSystem` (mirror [renderer/core/nhl_graphics_system.h](../renderer/core/nhl_graphics_system.h))
   and `vulkan::VulkanCommandProcessor`, overriding the same entry points the beta D3D12 path uses
   (`IssueDraw`, `IssueCopy`, shader/texture access). Confirm you can intercept a draw and reach the
   translated shaders + textures (the SDK Vulkan `shader.h` / `texture_cache.h` are the analogues of the
   D3D12 ones [nhl_command_processor.cpp](../renderer/core/nhl_command_processor.cpp) already uses).
5. **Answer:** can the project's intended enhancements (texture injection [[beta-injection-poc-validated]],
   internal-res upscaling, any custom draws) be expressed on this backend's hook surface? Internal-res is a
   cvar (`resolution_scale`/`draw_resolution_scale_*`) — verify it works here.

**Phase C — Decision.** Write a short findings doc (renders? fps? enhancement-hookable? presentation OK?)
and the GO/NO-GO.

---

## 5. Go / No-Go criteria

- **GO** if: (A) the Vulkan backend renders NHL Legacy correctly across screens at clearly-interactive fps
  (tens–hundreds), AND (B) its subclass exposes draw/texture/shader hooks sufficient for the planned
  enhancements. → Pivot the enhanceable-renderer foundation here; plan a migration off high-cut plume.
- **NO-GO** if: it won't present under the recomp, mis-renders NHL Legacy in ways that need per-title fixes
  bigger than the high-cut investment, OR its hook surface can't host the enhancement vision. → Keep
  high-cut; resume perf (consumer buffer/descriptor pooling, dynamic-texture-by-address reuse, `texCache`
  OOM bound) per [[highcut-live-takeover-freeze-fix]].

---

## 6. Open questions & risks (resolve these explicitly)

- **Presentation under the recomp.** The Vulkan backend brings its own `rex::ui::vulkan::presenter`.
  Confirm the recomp's windowing (`rex::ReXApp` / `SetupPresentation`) drives it the same way it drives the
  D3D12 presenter. (The recomp's D3D9 hooks translate *guest* D3D9; the GPU backend consumes the *PM4*
  command stream — independent of backend choice — but window/swapchain ownership must be verified.)
- **NHL 12 ≠ NHL Legacy.** The NHL12 build proves the backend is fast+faithful on *a* title; our title +
  game data + SDK version may need per-title fixes (the NHL12 team made some). Not a pixel oracle.
- **Enhancement ceiling.** The original high-cut rationale was a fully custom renderer for arbitrary
  enhancements. If the vision truly needs ground-up re-rendering (not faithful + upscaled + injected), the
  SDK backend may not host it — that's the real NO-GO axis. Pin down the concrete enhancement list first.
- **Why not done before?** Most likely the team went D3D12 (the SDK default), hit the fold, and pivoted to
  hand-rolled high-cut without trying the SDK's *own* Vulkan ROV path. The NHL12 analysis surfaced it; this
  is genuinely new information, not a re-tread.
- **Sunk cost is bounded.** The committed high-cut work got us frozen→observable and is preserved; the
  spike is opt-in and doesn't touch it. Spiking *before* more high-cut perf work avoids throwing good
  effort after a path that caps at single-digit fps.

---

## 7. Build / run

- Build: `_build_beta.bat` → `BUILD_EXIT=0`.
- Run the spike: `NHL_VK_BACKEND=1` (the new gate), **no** `NHL_BACKEND`/`NHL_HIGHCUT_*`. Plain game on the
  Vulkan backend. Logs: `out/build/win-amd64-relwithdebinfo/logs/nhllegacy_*.log`.
- Compare baseline: run the default (no env) D3D12 ROV path on the same scenes for an fps reference.
- SDK: `E:\Tools\rexglue-sdk\0.8.1.31-dev\win-amd64`. Game data: `H:\Emulators\games\XBOX\NHL Legacy - Vanilla`.

## 8. NHL12 tactical learnings — verdict (assessed; mostly deferred)

From the NHL12 analysis (`C:\Users\puckh\.claude\plans\i-ve-been-given-a-logical-clover.md`). All are LOW
impact vs. the backend question, and most become moot or free if we adopt the SDK Vulkan backend:
- **Async PSO cache (VS,PS,state):** the *cache* is already done in high-cut (consumer `pipelineCache`); the
  SDK Vulkan backend has its own `vulkan/pipeline_cache.h` (free if we pivot).
- **VK_EXT_robustness2+nullDescriptor / non_seamless_cube_map / custom_border_color:** minor parity/cleanup;
  the SDK Vulkan backend manages residency/cubes/samplers natively (likely already uses what it needs).
- **Selective resolve / dynamicRendering / Tracy / xxHash / resolve_resolution_scale_fill_half_pixel_offset:**
  N-A or low in high-cut; SDK-internal or only relevant once internal-res scaling is on (which the SDK gives
  via cvars). Defer all.
