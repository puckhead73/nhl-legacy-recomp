# High-cut C, P-2b — port the Xenia Xenos→SPIR-V *translator* to rex:: (kickoff prompt)

> **Self-contained kickoff for a fresh session.** Mission: make the SDK's `SpirvShaderTranslator`
> linkable by porting Xenia's translator `.cc` set into the project, adapted to `rex::`, so the
> high-cut renderer (path C) can translate the game's Xenos shaders to SPIR-V for plume-Vulkan.
> **This is a build/port task — get the 5 files compiling + linking, then translate one real shader.**

## Why this exists (context — don't re-derive)

The project is building **high-cut path C**: render the game on a renderer *we own* (plume RHI,
Vulkan, flat RTs, no EDRAM) for the enhancement/performance ceiling. The beta CommandProcessor
decodes the inlined PM4 draws + analyzes shaders; path C reuses that decode but renders the output
through **plume** instead of the SDK's D3D12 backend. Full plan: `docs/highcut-c-plume-renderer-plan.md`.
Memory: `[[highcut-c-plume-renderer]]`.

**Shaders for plume-Vulkan need SPIR-V.** The SDK is D3D12-only: it *ships the header*
`rex/graphics/pipeline/shader/spirv_translator.h` (= Xenia's `SpirvShaderTranslator`) but **the
implementation is not in `rexruntime.lib`** (`llvm-nm` → 0 `SpirvShaderTranslator` symbols), and the
header needs glslang's `SPIRV/SpvBuilder.h` which the SDK doesn't ship either. So we **port the impl
from Xenia** (BSD, same source the SDK forked). The device-share-D3D12 alternative was tested and
**rejected** (sharing rexglue's live device wedges the driver — see `[[highcut-c-plume-renderer]]`).

## Already done (DO NOT redo)

- **C-1** (committed `c58da0f`): plume renders geometry in-process (Vulkan), verified.
- **P-1** (committed `e19d996`): **glslang 14.3.0 vendored** via FetchContent in `CMakeLists.txt`
  (`ENABLE_OPT`/`HLSL`/tests off; links target `SPIRV`; `${glslang_SOURCE_DIR}` on the include path
  so `<SPIRV/SpvBuilder.h>` and `<SPIRV/GLSL.std.450.h>` resolve). Verified: `spv::Builder` emits valid
  SPIR-V (magic `0x07230203`) and the SDK's `spirv_builder.h` compiles against it. Probe:
  `gpu/hooks/highcut_spirv_probe.cpp` (gate `NHL_HIGHCUT_SPIRV_PROBE`).
- **P-2a** (committed `82766b8`): **ported `gpu/spirv/spirv_builder.cc`** (Xenia → `rex::graphics`,
  mechanical: namespace + include remap, bodies unchanged). Compiles clean. This is your template.

## The source

Xenia, **pinned commit `95a5c3e`** (github.com/xenia-project/xenia, 2026-02-18). Re-clone shallow:
`git clone --depth 1 --no-recurse-submodules https://github.com/xenia-project/xenia.git "$TEMP/xenia-src"`
then `git -C "$TEMP/xenia-src" checkout 95a5c3e` (the depth-1 clone is already at master ≈ this; if
master moved, fetch the pinned SHA). Translator files (in `src/xenia/gpu/`):

| Xenia .cc | lines | port to |
|---|---|---|
| `spirv_shader_translator.cc` | 3309 | `gpu/spirv/spirv_shader_translator.cc` |
| `spirv_shader_translator_alu.cc` | 1320 | `gpu/spirv/spirv_shader_translator_alu.cc` |
| `spirv_shader_translator_fetch.cc` | 2439 | `gpu/spirv/spirv_shader_translator_fetch.cc` |
| `spirv_shader_translator_memexport.cc` | 950 | `gpu/spirv/spirv_shader_translator_memexport.cc` |
| `spirv_shader_translator_rb.cc` | 3463 | `gpu/spirv/spirv_shader_translator_rb.cc` |

The matching **header is the SDK's** `rex/graphics/pipeline/shader/spirv_translator.h` (already
present, do NOT re-port it). **Port the `.cc` to match the SDK header, not Xenia's `.h`.** Recon found
the `spirv_builder.h` adaptation was purely cosmetic (reformatting + namespace), but the *translator*
header had ~365 diff lines vs Xenia's — so expect **some real reconciliation** (renamed/reformatted
signatures, members) beyond pure mechanical substitution. Diff the two headers first:
`diff "$TEMP/xenia-src/src/xenia/gpu/spirv_shader_translator.h"  <SDK>/spirv_translator.h`.

## The proven port recipe (from P-2a)

For each `.cc`: copy into `gpu/spirv/`, keep the BSD copyright header + add a `@modified … P-2b …`
note, then mechanically:
- `namespace xe { namespace gpu {` → `namespace rex::graphics {`  and the matching
  `}  // namespace gpu` / `}  // namespace xe` close → `}  // namespace rex::graphics`.
- Remap includes (table below). Bodies otherwise unchanged unless the SDK header reconciliation
  forces a signature/member fix.

### Include remap table (resolve every `xenia/…` and `third_party/…`)

| Xenia include | rex / project mapping | notes |
|---|---|---|
| `"xenia/gpu/spirv_shader_translator.h"` | `<rex/graphics/pipeline/shader/spirv_translator.h>` | **renamed** by the SDK |
| `"xenia/gpu/spirv_builder.h"` | `<rex/graphics/pipeline/shader/spirv_builder.h>` | |
| `"xenia/gpu/ucode.h"` | `<rex/graphics/format/ucode.h>` | |
| `"xenia/gpu/render_target_cache.h"` | ⚠ SDK has only `d3d12/` + `vulkan/` variants, no base | check what's referenced; may only need enums/consts — pick the lightest, or a small shim |
| `"xenia/gpu/spirv_shader.h"` | ⚠ **not in SDK shader dir** — `SpirvShader` appears in `rex/graphics/pipeline/shader/spirv.h` and `rex/graphics/vulkan/shader.h` | find where `SpirvShader` lives; may need its header too |
| `"xenia/gpu/draw_util.h"` | ⚠ not found by name — but `rex::graphics::draw_util` exists (used in `nhl_command_processor.cpp`) | locate the actual header |
| `"xenia/base/assert.h"` | `<rex/assert.h>` | provides `assert_true`/`assert_null` |
| `"xenia/base/math.h"` | `<rex/math.h>` | |
| `"xenia/base/string_buffer.h"` | ⚠ not found by name — `string::StringBuffer` is used in `shader.h::AnalyzeUcode` | locate the rex StringBuffer header |
| `"third_party/fmt/include/fmt/format.h"` | `<fmt/format.h>` | available |
| `"third_party/glslang/SPIRV/GLSL.std.450.h"` | `<SPIRV/GLSL.std.450.h>` | glslang on the P-1 include path |

## Known risks / watch-list

1. **Translator-header drift** (~365 lines). Some may be signature reformatting (harmless), some may
   be real (renamed members, changed param lists). Reconcile each compile error against the SDK
   `spirv_translator.h` / `translator.h` / `shader.h`.
2. **Missing/relocated headers** (⚠ rows above): `spirv_shader.h`/`SpirvShader`, `draw_util.h`,
   `string_buffer.h`, base `render_target_cache.h`. Each may need locating, a shim, or (worst case)
   a small additional port. `SpirvShader` (the `Shader` subclass the translator instantiates) is the
   most important — find its rex equivalent.
3. **Base `ShaderTranslator` symbols.** The `.cc` calls protected base-class helpers
   (`StartTranslation`, `EmitInstructionDisassembly`, register-access helpers, …). The base
   `ShaderTranslator::TranslateAnalyzedShader` IS exported (verified) and the DXBC translator uses the
   same base, so most should link — but **watch for unresolved-external** on any base helper the
   SPIR-V path uses that the SDK didn't export. If one is missing, it may also need porting.
4. **`Features(const ui::vulkan::VulkanDevice*)`** ctor (in `spirv_shader_translator.cc`) references
   the SDK Vulkan device (impl not shipped). P-3 will construct `Features(bool all)` instead (no
   device) — if the `VulkanDevice` ctor won't link, guard/stub it; we don't need it.
5. **glslang version drift in the `.cc`.** P-2a proved the builder `.cc` matches glslang 14.3.0, but
   the translator uses more `spv::` surface (`GLSL.std.450` ext-inst, decorations, capabilities). If a
   `spv::` call mismatches 14.3.0, either adjust the call or re-pin glslang to Xenia's version (Xenia's
   `third_party/glslang` submodule SHA is the ground truth — check `.gitmodules`/submodule status).

## Build / verify

- Add the 5 `gpu/spirv/spirv_shader_translator*.cc` to `NHLLEGACY_SOURCES` in `CMakeLists.txt`
  (next to the P-2a `gpu/spirv/spirv_builder.cc` line). Build: `_build_beta.bat` (touch a `.cpp`
  first if ninja says "no work to do"). Iterate compile errors file-by-file.
- **P-2b done = the 5 files compile AND `SpirvShaderTranslator` links** (no unresolved externals).
  Prove linkage by extending `gpu/hooks/highcut_spirv_probe.cpp` to construct a
  `rex::graphics::SpirvShaderTranslator` (`Features(true)`, `false,false,/*edram_fsi=*/false`) and call
  a method (e.g. `GetDefaultVertexShaderModification`) — build it in.
- **Then P-3** (next milestone, can be same session if time): translate a REAL analyzed shader. The
  beta CP already holds `beta_current_vs_`/`beta_current_ps_` (analyzed Xenos `Shader*`). In
  `RenderBetaOwnedDraw` (gated, e.g. `NHL_HIGHCUT_XLAT_TEST`, first draw): build a
  `SpirvShaderTranslator`, `mod = GetDefaultVertexShaderModification(vs->GetDynamicAddressableRegisterCount(SQ_PROGRAM_CNTL.vs_num_reg), result.host_vertex_shader_type)`,
  `t = vs->GetOrCreateTranslation(mod, &is_new)`, `if (is_new) TranslateAnalyzedShader(*t)`, log
  `t->is_valid()` + `t->translated_binary().size()`. **Validate the bytes with `spirv-val`** (dump to
  a file and run it, or check magic `0x07230203` + run the swdtools validator) before trusting them.
  SAFETY: only translate when `is_new` (a fresh SPIR-V `Translation`) so you never overwrite the live
  DXBC translation the beta path uses (its modification differs; see the reverted P-2 probe note in
  git history / `[[highcut-c-plume-renderer]]`).

## Done criteria

- **Primary:** all 5 translator `.cc` ported to `gpu/spirv/`, compile clean, and
  `rex::graphics::SpirvShaderTranslator` links (probe constructs one + calls a method).
- **Stretch (P-3):** a real `beta_current_vs_` translates to **`spirv-val`-clean SPIR-V**.
- Commit P-2b (the 5 files + CMake) and P-3 separately. Update `[[highcut-c-plume-renderer]]` +
  `docs/highcut-c-plume-renderer-plan.md`. After P-3, the C milestones resume at **C-3** (bridge one
  real decoded guest draw into plume).

## Scope / discipline

- Keep changes **gated** (`NHL_HIGHCUT_*`); the beta/DXBC path and all validated scenes must stay
  byte-identical. The ported translator only runs when explicitly invoked.
- Preserve BSD copyright headers on all ported files (license compliance).
- The port is mostly mechanical — resist rewriting logic. When a compile error appears, first ask "is
  this a namespace/include/signature-reformat mismatch?" before changing any translation logic.
