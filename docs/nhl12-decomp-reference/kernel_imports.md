# Kernel/XAM Import Disposition — `nhlzf.exe`

*From `tools/list_imports.py` → full list in `docs/kernel_imports.csv`.
**317 unique imports** (304 functions + 13 data variables); xboxkrnl.exe 200,
xam.xex 117. Record math cross-validates: 317 slots + 304 thunks = 621 records.*

Dispositions: **R** = ReXGlue expected to provide (Xenia-derived kernel) ·
**S** = stub (log + benign return) · **O** = override/own implementation ·
**?** = audit needed.

| Family | # | Disp. | Notes (MVP perspective) |
|---|---|---|---|
| `NetDll*` (winsock/XNet) | 47 | **S** | No online for MVP. Return "no link"; XNetStartup OK-but-dead. Largest single family — good news: stubbable. |
| `Ke*` + `KfAcquire/Release`, `Ki*` | 34 | **R** | Threads, critsecs, events, DPC-ish. Core boot path. |
| `Nt*` | 32 | **R** | File I/O (VFS), events, virtual mem. Boot + streaming path. |
| `Vd*` | 25 | **O** | GPU boundary — Strategy A interception point (Phase 4). Enumerate exact 25 before designing the command processor. |
| `Rtl*` | 19 | **R** | critsec, unicode, snprintf-family. Trivial/known. |
| `XMA*` | 17 | **O** | XMA decoder context registers — MMIO-adjacent! Phase 1 risk item: these must be overrides, never recompiled. Pairs with FFmpeg XMA plan (Phase 5). |
| `XamShow*` | 15 | **S** | UI dialogs → log + auto-dismiss (plan §Phase 3.5). |
| `XeCrypt*` / `XeKeys*` | 15 | **R/O** | EA signs/verifies content (rosters, saves). Xenia-derived impls exist; verify SHA/RC4/RSA correctness — silent failures here cause mysterious EA init failures. |
| `XamUser*` | 11 | **S** | Fake signed-in profile, success returns. |
| `Ex*` | 10 | **R** | `ExGetXConfigSetting` critical for video mode/locale at boot. |
| `Io*` / `Ob*` / `Fsc*` | 17 | **R** | I/O completion, object refs, file-system cache hints. |
| `Mm*` | 7 | **R** | Physical/virtual queries; watch 64K page assumptions. |
| `Xex*` | 6 | **R** | Module queries; `XexCheckExecutablePrivilege` at boot. |
| `XAudio*` | 6 | **O** | Host mixer output (Phase 5). |
| `XamContent*` | 6 | **S** | Content enumeration → empty (no saves for MVP). |
| `XMsg*` | 5 | **S** | Async message system — needs correct overlapped completion semantics even when stubbed, or boot hangs. |
| `XamVoice*` | 5 | **S** | Headset — dead. |
| `XamInput*` | 4 | **O** | → XInput/SDL (Phase 5). Small surface as planned. |
| `XamTask*`, `XamLoader*`, `XamSession*`, `XamAlloc/Free`, `XNotify*` etc. | ~15 | **S/R** | Misc XAM plumbing; mostly benign success. |
| `Dbg*`, `_snprintf`, `St*` (string), `Hal*` | ~6 | **R** | Trivial. |
| **Data variables** (13) | 13 | **O** | `KeDebugMonitorData`, `XboxHardwareInfo`, `KeTimeStampBundle`, `ExLoadedImageName`, etc. — fixed structs in guest memory ReXGlue must populate. Enumerated in CSV. |

## Headline conclusions

1. **No surprises, bounded surface.** 317 imports is mid-pack for a 360 title.
   Nothing exotic (no XHV engine, no camera, no kinect, no system link beyond
   standard XNet).
2. **The 47 NetDll imports are bulk-stubbable** for MVP (offline only), taking
   the "must actually work" set down to ~270.
3. **XMA (17) + Vd (25) = the override budget** — matches plan §Phase 1.5's
   MMIO audit prediction. These two families are exactly where recompiled code
   must *not* run.
4. **EA boot-path risk**: `ExGetXConfigSetting`, `XexCheckExecutablePrivilege`,
   `XeCrypt*` are the classic "game silently exits at boot" trio. Implement
   honestly, early.
5. Per-import call tracing (plan §Phase 3.6) should log by this CSV's names —
   wire the CSV into the runtime's import-thunk generator.
