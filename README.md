# nhl-legacy-recomp

Native PC port of NHL Legacy (Xbox 360) by static recompilation, built on the
ReXGlue SDK (Xenia-derived runtime). Reverse-engineering notes and analysis
live in `../nhl-re`.

## Layout

- `nhllegacy_manifest.toml` / `nhllegacy_functions.toml` - ReXGlue codegen
  config + hand-curated scanner-missed function addresses (specific to the
  vanilla `default.xex`).
- `generated/` - recompiled C++ emitted by `rexglue codegen` (181 files).
- `src/` - the host app: cvar config, cache VFS mount, camera stubs,
  diagnostics. `OnConfigurePaths` defaults `game_data_root` to an
  exe-relative `game/` dir so end-user installs launch with no arguments.
- `tools/packager/` - **nhl-legacy-builder**, the end-user CLI that turns a
  disc dump (raw .iso or extracted folder) into a ready-to-run install:
  validates `default.xex` by SHA-256 against the payload manifest, extracts
  the XDVDFS image, lays down the prebuilt port binaries.
- `tools/*.py|ps1` - RE/diagnostic scripts (GPU trace analysis, guest stack
  annotation, UI driving).
- `release/package.ps1` - dev-side release pipeline: build port + builder,
  generate the payload manifest (hash pinned from `assets/default.xex`),
  stage and zip the end-user package.
- `assets/default.xex` - reference vanilla XEX (hash source for releases).

## Dev build

Requires LLVM/Clang, Ninja, CMake 3.25+, and the ReXGlue SDK
(`-DCMAKE_PREFIX_PATH=<sdk>/win-amd64` on first configure).

```powershell
cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=E:/Tools/rexglue-sdk/0.8.0/win-amd64
cmake --build --preset win-amd64-relwithdebinfo                       # port + builder
cmake --build --preset win-amd64-relwithdebinfo --target nhllegacy_codegen  # re-run codegen (only after manifest/XEX changes)
```

Run the dev build against a game folder:

```powershell
out\build\win-amd64-relwithdebinfo\nhllegacy.exe --game_data_root "H:\...\NHL Legacy - Vanilla"
```

## Cutting a release

```powershell
.\release\package.ps1 -Version 0.1.0 -TestInput "H:\...\NHL Legacy - Vanilla"
```

Produces `out/release/nhl-legacy-recomp-<ver>.zip`: the builder CLI +
`payload/` (prebuilt `nhllegacy.exe`, runtime DLLs, manifest) + docs/license
notices. The `-TestInput` self-check unzips the artifact and verifies a real
dump against the baked hash. Ships the relwithdebinfo (`rd`) runtime flavor -
the only one play-tested so far.

## End-user flow

```
nhl-legacy-builder install --iso "C:\dumps\NHL Legacy.iso" --out "C:\Games\NHL Legacy"
nhl-legacy-builder verify  --iso "C:\dumps\NHL Legacy.iso"   # validate only
```

or double-click `nhl-legacy-builder.exe` for interactive prompts. The result
is `<out>\nhllegacy.exe` + `<out>\game\` - launches with no arguments.

## Gotchas

- The packager must use a **wide entry point** (`wmain`): rexruntime's
  `GetExecutablePath` calls `_get_wpgmptr`, which fail-fasts (0xC0000409)
  in narrow-CRT processes. See `tools/packager/src/main.cpp`.
- Gated integration tests: configure with `-DNHLLEGACY_TEST_ISO=<path>` /
  `-DNHLLEGACY_TEST_GAME_DIR=<path>` and run `ctest` in the build dir.
