# NHL12-recomp — Handoff (Phase 4: GPU done, blocked on cache data)

Status as of 2026-06-13. Read this top-to-bottom before continuing.

## TL;DR

- **Strategy = adopt RexGlue fully** (Xenia-derived SDK in `RexGlue/`). We re-recompiled NHL12 with
  RexGlue's *own* codegen and link against its integrated runtime (kernel + memory + VFS + **D3D12 GPU** + XMA audio).
  The earlier XenonRecomp path + hand-rolled `runtime/` (PM4-scan + GDI blit) is **superseded** — do not extend it.
- **The GPU translation works.** `app/out/build/win-amd64-debug/nhl12.exe` (225 MB) boots with **zero crashes,
  zero missing functions**: D3D12 on the host GPU, Xenos→DXBC shader translation building real VS+PS pipelines,
  EDRAM render targets, full engine threads (sim/render/JobManagers/AssetStream/XMA/rw::core::filesys).
- **Only remaining blocker = content pipeline, not code.** The game loads ALL assets from `cache:\` (never falls
  back to `d:\`). `cache:` must be pre-populated with the unpacked `.big` data. The game does NOT unpack the
  `.big`s itself. Loading stalls before the first frame (0 `VdSwap`, never calls `VdInitializeRingBuffer`).
- **NEXT TASK (agreed): write an unpacker for EA's "EB" `.big` format, populate `cache:\`, rerun for the first frame.**

## Layout / key paths

- `RexGlue/` — the SDK (git repo, submodules initialized). Built to `RexGlue/out/`.
  - Debug runtime libs: `RexGlue/out/win-amd64/Debug/*.lib` (rexruntimed.lib etc.)
  - CLIs: `RexGlue/out/win-amd64/Debug/rexglue.exe` (slow), `RexGlue/out/win-amd64/Release/rexglue.exe` (**use this**, ~5x faster codegen)
  - Installed SDK (for `find_package`): `RexGlue/out/install/win-amd64/` (registered in CMake user pkg registry)
- `app/` — the NHL12 host app (created by `rexglue init`).
  - `app/nhl12_manifest.toml` — codegen manifest. `includes = [fixups, switch_tables, gapfuncs, extrafuncs]`.
  - `app/nhl12_fixups.toml` — setjmp/longjmp addrs + manual function boundaries.
  - `app/nhl12_switch_tables.toml` — 505 jump tables (converted from `config/switch_tables.toml`).
  - `app/nhl12_gapfuncs.toml` — 860 analyzer-missed indirect-only functions (size-0).
  - `app/nhl12_extrafuncs.toml` — shared-tail branch targets (size-0).
  - `app/src/nhl12_app.h` — `Nhl12App : rex::ReXApp`; `OnPostSetup()` mounts `cache:`/`update:` devices.
  - `app/src/nhl12_stubs.cpp` — XUsbcamGetState/SetConfig stubs (RexGlue omits xboxkrnl_usbcam.cpp from its build).
  - `app/src/nhl12_crash.cpp` — unhandled-exception filter: writes faulting RVA + stack-scan to `build/nhl12_crash.txt`.
  - `app/generated/default/` — 154 generated C++ files (308 MB), incl. `nhl12_init.cpp` (PPCFuncMappings) + `nhl12_init.h`.
  - Built exe: `app/out/build/win-amd64-debug/nhl12.exe` (+ `nhl12.pdb`).
- `extracted/` — game data: the `.big` archives (`data0/cache/nocache/cacherender/nocacherender/boot/audioboot*.big`),
  `default.xex`, partial `fe/`, `gamedata/`. **The loose asset tree the game needs is NOT here — it's inside the `.big`s.**
- `tools/` — `rexenv.bat` (build env), `convert_switch_tables.py`, `find_gap_functions.py`, `add_unresolved.py`, `xdvdfs.py` (ISO extractor), `powerpc-none-elf-objdump` lives in `RexGlue/tools/binutils/`.

## Build / run commands (Windows; use the PowerShell tool, NOT Bash `cmd //c` — it mangles quoting)

Env helper sets VsDevCmd amd64 + LLVM clang 22 + VS18 ninja on PATH:
```
cmd /c 'call tools\rexenv.bat && <command>'
```
- Codegen (regen C++): `<rexglue Release>\rexglue.exe --log-level info codegen app\nhl12_manifest.toml` (run with cwd=app)
- Build app: `cmake --build app/out/build/win-amd64-debug --target nhl12`
- Configure app (once): `cmake --preset win-amd64-debug` (in app/)
- Run: `nhl12.exe --game_data_root extracted --log_file build\run.log --log_level debug`

### Iteration economics (IMPORTANT)
- **Release `rexglue.exe` codegen ≈ 2.5 min**, fails fast at the Validate phase BEFORE emit. Iterate config there.
- Codegen writer content-hashes outputs and skips unchanged files — BUT adding/removing functions reshuffles the
  function→file partitioning, so essentially all 154 files change → **full app rebuild (~15–40 min)**. Batch all
  codegen-config changes, validate clean on the Release CLI, THEN do ONE full rebuild.
- **Runtime reruns are cheap (~4s boot to the stall).** Iterate runtime/VFS/app-code fixes freely (incremental relink only).

## What was fixed to get here (don't regress)

1. **libmspack symlinks**: `RexGlue/thirdparty/libmspack/cabextract/mspack/*.{c,h}` are git symlinks that don't
   materialize on Windows → clang compiles the path text. Already materialized (copied real files). Redo after fresh clone.
2. **setjmp/longjmp**: EA engine wraps init in setjmp/longjmp. RexGlue emits host ppc_setjmp/ppc_longjmp ONLY for
   configured addrs (builders/context.cpp). `app/nhl12_fixups.toml`: `setjmp_address=0x83366050`, `longjmp_address=0x833643B0`.
   Without these, exception handling returns garbage → front-end crash. (Verified by disasm: setjmp saves f14-f31/r13-r31/
   CR-LR/v64-v127 to jmp_buf@r3; longjmp restores.)
3. **Manual function boundaries** (`nhl12_fixups.toml`): the 5 UnresolvedCall sites the analyzer leaves (jump-table
   shared-tails @ 0x82C1F5DC region, leaves @ 0x82A72C68/0x82F313F8/0x82F14648). Sized via objdump.
4. **Switch tables**: `tools/convert_switch_tables.py config/switch_tables.toml app/nhl12_switch_tables.toml`
   (our `base`/`r`/`labels` → RexGlue `[[switch_tables]]` address/register/labels; `default` dropped).
5. **Indirect-only functions (860+)**: reached only via runtime-built pointers — analyzer can't find them statically.
   `tools/find_gap_functions.py extracted/nhlzf_image.bin app/generated/default/nhl12_init.cpp app/nhl12_gapfuncs.toml app/nhl12_switch_tables.toml`
   finds code that's (after a terminator) AND (not registered) AND (not padding) AND (not a static branch target) AND
   (not a switch label). **Emit with NO size (`"0xADDR" = {}`)** — the analyzer CF-scans the real extent; an explicit
   size CAPS the scan → "use of undeclared label" at compile time. Regenerate against a BASELINE `nhl12_init.cpp`
   (codegen with only fixups+switch includes), else it finds 0. Then iterate `tools/add_unresolved.py <codegen.log>
   ... app/nhl12_extrafuncs.toml` to add shared-tail branch targets until codegen validates 0 errors.
6. **VFS devices** (`app/src/nhl12_app.h OnPostSetup`): mount `cache:` + `update:` on `\Device\Harddisk1\...` (NOT
   Harddisk0 — RexGlue's NullDevice@`\Device\Harddisk0` prefix-shadows later mounts). Currently `cache:`→`game_data_root()`
   (extracted) writable; `update:`→empty dir.
7. **xboxkrnl_usbcam.cpp omission**: RexGlue ships it but its kernel CMake doesn't compile it → `__imp__XUsbcam*`
   undefined at link. Stubbed in `app/src/nhl12_stubs.cpp`. (Upstream fix: add it to RexGlue's kernel target.)
8. **InvalidFunctionTrap**: changed `RexGlue/src/system/function_dispatcher.cpp` from `REX_FATAL`(abort) to
   log-once + r3=0 + continue, so missing indirect targets surface without killing the run (keep for bring-up).

## THE BLOCKER (your task): cache: not populated

The game reads ONLY from `cache:\` (full distinct list captured in `build/nhl12_run.log` NtCreateFile lines):
- `.big` archives: `cache:\{data0,cache,nocache,audioboot}.big` — these DO exist in `extracted/`.
- Loose tree (lives INSIDE the `.big`s, NOT loose on disc — ISO root = ~23 entries): `cache:\audio\music\*.csi/.sbr/.xml`,
  `cache:\audio\tuning\mixer_*`, `cache:\audio\reverb\*.irf`, `cache:\audio\NA_En_loadfile.xml`,
  `cache:\rendering\boot\texlib_*.rx2`, `cache:\rendering\skeleton_bindpose\*.rx2`, `cache:\fe\profile\default.png`,
  `cache:\AttribDB\renddb.{bin,vlt}`, `cache:\scrape\{boot.scrape,scenedef.lua}`, `cache:\shaders\*.fxo`,
  `cache:\debug\courbd.ttf`, `cache:\467414.ver`.
- Game does NOT unpack the `.big`s itself (empty-cache run wrote only RexGlue's 2 shader-cache files). It expects
  `cache:` pre-populated (360 HDD install). So it stalls; audio busy-spins on XMA reg 0601 (missing music).

### `.big` format
First bytes of every `.big` = `45 42 00 03` ("EB" + version 3), then big-endian fields (file count at off 4: data0=2,
boot=0x228, cache=0x3282; 0x400 at off 8). This is an EA "EB"/BIG variant, NOT standard BIGF — needs format RE.
Watch for refpack compression on entries.

### Recommended next steps
1. RE the "EB" `.big` TOC (offsets/sizes/names; check compression). Start with the SMALL ones the menu needs:
   `boot.big` (9.7MB), `audioboot.big` (1.9MB), maybe `data0.big`. Write `tools/unpack_big.py`.
2. Unpack the loose tree into a real writable cache dir (e.g. `extracted/cache_hdd/`), preserving paths
   (`audio/music/...`, `rendering/...`, `fe/...`, etc.).
3. Point `cache:` at that dir in `app/src/nhl12_app.h` (`cache_host = <that dir>`), keep writable. Incremental rebuild.
4. Rerun; watch `build/nhl12_run.log` for `VdInitializeRingBuffer` then `VdSwap` (= first presented frame).
5. If the loose paths still differ from what's inside the `.big` (naming), map them; the game's `rw::core::filesys`
   may also accept the `.big` mounted directly — investigate whether opening `cache:\cache.big` is enough (the game
   may read members internally) vs. needing loose files.
- Alternative: RE the game's own cache-build/install trigger (FileCopier::Thread / AssetStream::Unpack) and make it run.

## Debug tooling
- Crash: `build/nhl12_crash.txt` (faulting RVA + stack-scan) → symbolize: `llvm-symbolizer --obj=app/out/build/win-amd64-debug/nhl12.exe --relative-address 0x<RVA>`. Also Windows Event Log → Application Error → fault offset.
- objdump: `RexGlue/tools/binutils/powerpc-none-elf-objdump.exe -D -b binary -m powerpc:common64 -EB --adjust-vma=0x82000000 [--start-address/--stop-address] extracted/nhlzf_image.bin`
- Find callers/data refs to an addr: scan `extracted/nhlzf_image.bin` for the BE word / branch encodings (see add_unresolved.py for the branch-decode pattern).
- Filter the run log noise: `grep -aivE 'XMA:|AudioWorker|SubmitFrame|NullDevice|ResolvePath|SetThreadName|GetProcAddressByOrdinal'`.

## Gotchas
- Use the PowerShell tool for `cmd /c 'call rexenv.bat && ...'`; the Bash tool's `cmd //c` mangles quotes.
- The `vswhere.exe is not recognized` line from rexenv is harmless.
- Run log is appended across runs unless deleted; delete `build/nhl12_run.log` before a clean run.
- `recompiled/` (old XenonRecomp output) and `runtime/`, `RexGlue-main/` are the superseded path — ignore for GPU.
