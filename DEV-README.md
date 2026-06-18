# NHL Legacy Recomp — developer guide

Native PC port of NHL Legacy (Xbox 360) by static recompilation, built on the
ReXGlue SDK (Xenia-derived runtime). The user-facing overview is in
[README.md](README.md); **[docs/current-status.md](docs/current-status.md) is the
authoritative pointer to "what is current."**

## Layout

- `nhllegacy_manifest.toml` / `nhllegacy_functions.toml` — ReXGlue codegen config +
  hand-curated scanner-missed function addresses (specific to the vanilla
  `default.xex`).
- `generated/` — recompiled C++ emitted by `rexglue codegen` (181 files; not tracked,
  regenerated from the manifest + XEX).
- `src/` — the host app: cvar config, cache VFS mount, camera stubs, diagnostics.
  `OnConfigurePaths` defaults `game_data_root` to an exe-relative `game/` dir so
  end-user installs launch with no arguments.
- `renderer/` , `gpu/` — the Vulkan backend (`renderer/core/nhl_vk_backend.cpp`), the
  in-game overlay, and the (reference-only) high-cut plume hooks.
- `tools/packager/` — **nhl-legacy-builder**, the end-user CLI that turns a disc dump
  (raw `.iso` or extracted folder) into a ready-to-run install: validates
  `default.xex` by SHA-256 against the payload manifest, extracts the XDVDFS image,
  lays down the prebuilt port binaries, and unpacks the `.big` archives into a loose
  modding tree.
- `scripts/` — all dev/build/diagnostic drivers (`_*.bat` build scripts, `_*.ps1` GPU
  trace / UI driving / capture helpers). Run them **from the repo root**, e.g.
  `scripts\_game_ffx_build.bat`.
- `tools/*.py` — RE/diagnostic Python (GPU trace analysis, guest stack annotation,
  `.cba` animation parsing, etc.).
- `release/package.ps1` — dev-side release pipeline: stages the prebuilt port +
  builder, generates the payload manifest (hash pinned from `assets/default.xex`),
  zips the end-user package.
- `assets/default.xex` — reference vanilla XEX (hash source for releases; not tracked,
  copyrighted).

## Dev build

**The canonical build is `win-amd64-vk-ffx`** — the SDK-native Vulkan `fsi` backend
with FidelityFX. It is configured directly by the build scripts (no CMake preset).
Requires LLVM/Clang, Ninja, CMake 3.25+, the Vulkan SDK (1.4.350), VS2022 BuildTools,
and the ReXGlue SDK built from source with `REXGLUE_USE_VULKAN=ON`.

```bat
:: from the repo root
scripts\_ffx_sdk_build_install.bat   :: SDK -> E:\Tools\rexglue-sdk\src\out\install\win-amd64-ffx  (only when patching rexglue source)
scripts\_game_ffx_build.bat          :: game -> out\build\win-amd64-vk-ffx
```

Canonical exe: `out\build\win-amd64-vk-ffx\nhllegacy.exe`. Run on the Vulkan path with
`NHL_VK_BACKEND=1` (defaults to `fsi`). Diagnostic driver: `scripts\_vknet.ps1`.

Re-run codegen only after the manifest/XEX change:
`cmake --build out\build\win-amd64-vk-ffx --target nhllegacy_codegen`.

Run the dev build against a game folder:

```
out\build\win-amd64-vk-ffx\nhllegacy.exe --game_data_root "H:\...\NHL Legacy - Vanilla"
```

**Optimized / PGO builds** (the #1 perf lever) link the same FFX SDK and are now
`Release`-based (`-O3 -DNDEBUG -march=x86-64-v3 -flto=thin`, lld). Use a PORTABLE
arch, **never `-march=native`** — native on the AMD dev box emits SSE4a `EXTRQ`
that `#UD`s on Intel (commit `49e6650`). `scripts\_build_vk_opt.bat` is the
non-PGO optimized build; the 3-stage PGO flow is `scripts\_build_vk_pgogen.bat` →
play a session → `llvm-profdata merge -output=pgo/nhllegacy.profdata *.profraw` →
`scripts\_build_vk_pgo.bat`. Re-capture the profile after any base/flag change.

The shipped runtime DLL is the SDK's `Release` config (rexruntime.dll, no `rd`
suffix), which strips the Tracy/perf-counter instrumentation that the
RelWithDebInfo dev runtime carries (`REXGLUE_ENABLE_TRACY=OFF` at configure +
the `$<NOT:$<CONFIG:Release>>` profiling gate). On Intel hybrid CPUs the port
pins itself to the P-cores at startup (`NHL_PIN_PCORES`, default on; set `=0` to
disable).

## Cutting a release

```powershell
.\release\package.ps1 -Version 0.1.0 -Preset win-amd64-vk-pgo -TestInput "H:\...\NHL Legacy - Vanilla"
```

Produces `out/release/nhl-legacy-recomp-<ver>.zip`: the builder CLI + `payload/`
(prebuilt `nhllegacy.exe`, runtime DLLs incl. `amd_fidelityfx_vk.dll`, the `.big`
extractor, manifest) + docs/license notices. vk presets have no CMake preset, so the
script auto-packages the **prebuilt dir as-is** (build it first with the scripts
above). The `-TestInput` self-check unzips the artifact and `verify`s a real dump
against the baked hash. `v0.1.0` ships the playtested PGO (`vk-pgo`) flavor.

## End-user flow

```
nhl-legacy-builder install --iso "C:\dumps\NHL Legacy.iso" --out "C:\Games\NHL Legacy"
nhl-legacy-builder verify  --iso "C:\dumps\NHL Legacy.iso"   :: validate only
```

or double-click `nhl-legacy-builder.exe` for interactive prompts. The result is
`<out>\nhllegacy.exe` + `<out>\game\` — launches with no arguments.

## Gotchas

- **FFX DLL / `0xC0000135`** — on the FFX build, `rexruntime` hard-imports
  `amd_fidelityfx_vk.dll`. Both the port and the builder need it beside their exe or
  the loader fails with `STATUS_DLL_NOT_FOUND`. The build scripts don't always stage
  it, so `package.ps1` falls back to copying it from the SDK install's `bin/`.
- **Wide entry point** — the packager must use `wmain`: rexruntime's
  `GetExecutablePath` calls `_get_wpgmptr`, which fail-fasts (`0xC0000409`) in
  narrow-CRT processes. See `tools/packager/src/main.cpp`.
- **Gated integration tests** — configure with `-DNHLLEGACY_TEST_ISO=<path>` /
  `-DNHLLEGACY_TEST_GAME_DIR=<path>` and run `ctest` in the build dir.
- The high-cut plume path (`gpu/hooks/`, `NHL_HIGHCUT*`) and the beta owned-D3D12
  takeover (`NHL_BACKEND=beta`) are **reference-only** — superseded by the Vulkan
  backend, kept for A/B correctness comparison.
