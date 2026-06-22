# RexGlue Runtime — The Host Platform Layer

The recompiled NHL 12 code (see [`xenondecomp-notes.md`](xenondecomp-notes.md))
runs against **RexGlue v0.8.0**, a Xenia-derived SDK that supplies everything the
Xbox 360 kernel/hardware used to: threads, memory, VFS, the **D3D12/Vulkan GPU**,
and **XMA audio**. This replaces the bottom layer of
[`../architecture/overview.md`](../architecture/overview.md) §1.

> Current state (Phase 4): the build **boots with zero crashes / zero missing
> functions** — D3D12 on the host GPU, Xenos→DXBC shader translation, EDRAM render
> targets, full engine threads. It **stalls before the first frame** because
> `cache:\` is not pre-populated (a *content* problem, not a code problem). See
> [`HANDOFF.md`](../HANDOFF.md) and [`../assets/asset-pipeline.md`](../assets/asset-pipeline.md).

---

## 1. Why RexGlue (not the hand-rolled runtime)
Phase 2 had a stub `runtime/` (4 GiB guest reserve, import table, logging stubs) and
later a hand-rolled "GPU" that was really a **PM4 ring scanner + GDI blit** — it could
only show bytes something else populated, never render. Phase 4 **adopted RexGlue
fully**: it ships real D3D12+Vulkan command processors, a Xenos→DXBC/SPIR-V shader
translator, texture/RT caches, EDRAM emulation, XMA audio, a kernel, a VFS, and its
**own recompiler**. The hand-rolled path is superseded — do not extend it.

## 2. The ABI fork (why RexGlue re-recompiles the game)
RexGlue's `PPCContext` is **not layout-compatible** with XenonRecomp's: it inserts
`uint8_t vscr_sat` (after `fpscr`) and `uint32_t last_indirect_target` (before `f0`),
shifting every float/vector offset. RexGlue's GPU/kernel/memory are bound to *its*
context. Therefore NHL 12 is **re-recompiled with RexGlue's codegen** so the whole
program shares RexGlue's ABI, then linked against RexGlue's integrated runtime. You
cannot link RexGlue's GPU into a XenonRecomp-built exe.

## 3. Import surface (CONFIRMED `[IMP]`)
**317 unique imports** (200 `xboxkrnl` + 117 `xam`). Full dispositions in
[`../kernel_imports.md`](../kernel_imports.md) / `kernel_imports.csv`. Summary:

| Family | # | Disposition | Role |
|---|---|---|---|
| `NetDll*` | 47 | **Stub** | Winsock/XNet — offline MVP; return "no link". |
| `Ke*`/`Ki*`/`Kf*` | 34 | **RexGlue** | Threads, critsecs, events, spinlocks, DPCs. |
| `Nt*` | 32 | **RexGlue** | File I/O (VFS), events, virtual memory. |
| `Vd*` | 25 | **Override** | GPU ring boundary — see §4. |
| `Rtl*` | 19 | **RexGlue** | critsec, unicode, snprintf. |
| `XMA*` | 17 | **Override** | Audio decoder contexts — see §5. |
| `XamShow*` | 15 | **Stub** | System UI dialogs → auto-dismiss. |
| `XeCrypt*`/`XeKeys*` | 15 | **RexGlue/Override** | Content signing/verify (rosters, saves) — must be correct or EA init fails silently. |
| `XamUser*` | 11 | **Stub** | Fake signed-in profile. |
| `Ex*` | 10 | **RexGlue** | `ExGetXConfigSetting` (video mode/locale) is boot-critical. |
| `Io*`/`Ob*`/`Fsc*` | 17 | **RexGlue** | IO completion, object refs, FS cache hints. |
| `XamInput*` | 4 | **Override** | → XInput/SDL pads. |
| Data variables | 13 | **Override** | Fixed guest structs RexGlue must populate (`XboxHardwareInfo`, `KeTimeStampBundle`, `ExLoadedImageName`, …). |

**Boot-gate trio** (the classic "EA silently exits"): `ExGetXConfigSetting`,
`XexCheckExecutablePrivilege`, `XeCrypt*` — implement honestly and early.

## 4. GPU (CONFIRMED working `[RT]`)
- Boundary is the standard **`Vd*` ring buffer** (Strategy A) — no MMIO needed.
- `VdInitializeRingBuffer` arms RexGlue's command processor; `SetInterruptCallback`
  drives the render thread.
- **Xenos shaders are translated to DXBC** (D3D12) / SPIR-V (Vulkan); VS+PS pipelines
  are built live. **EDRAM** render targets are emulated.
- **D3D12 is required for NHL 12's gameplay path** (the renderer uses ROV-style
  resolve; `render_target_path_d3d12=rov`). The renderer was the subject of extensive
  bring-up — see [`../graphics/`](../graphics/) and the existing investigation docs.

## 5. Audio (CONFIRMED path `[RT][IMP]`)
- The game's engine is `rw::audio::core` / RWAudioCore (see [`../audio/`](../audio/)).
- **XMA** decode is reached via the 17 `XMA*` kernel imports (overridden, not
  recompiled) → RexGlue's XMA (FFmpeg-backed). The XMA worker busy-spins on register
  `0601` when its music streams are missing (a *content* gap, not a code bug).
- Speex voice (`rwaudiocore/decoders/speex`) decodes on the CPU in recompiled code.

## 6. VFS (CONFIRMED `[RT]`)
- Host `--game_data_root` is mounted; the game's `rw::core::filesys` opens `cache:\…`.
- **Device-registration order matters:** RexGlue's `NullDevice` sits at
  `\Device\Harddisk0`; VFS picks the first prefix match, so `cache:`/`update:` are
  mounted on **`\Device\Harddisk1`** to avoid being shadowed
  (`app/src/nhl12_app.h::OnPostSetup`).
- `cache:` → a writable host dir; `update:` → an empty dir. The game reads ALL assets
  from `cache:` and never falls back to `d:\`.

## 7. Threading (host side)
`ExCreateThread` → real Win32 threads (entry/stack/affinity from the guest args).
RexGlue adds its own **GPU Command** and **VSync** threads. The PPC weak memory model
must be honoured by host fences — see [`../architecture/threading.md`](../architecture/threading.md).

## 8. Bring-up robustness knobs (keep during dev)
- `function_dispatcher.cpp` `InvalidFunctionTrap`: changed from `REX_FATAL`(abort) to
  **log-once + `r3=0` + continue**, so a missing indirect target surfaces without
  killing the run.
- `app/src/nhl12_crash.cpp`: `SetUnhandledExceptionFilter` writes faulting RVA +
  stack-scan to `build/nhl12_crash.txt` (recompiled code has no rbp frame chain → scan
  rsp for module-range return addresses). Symbolize with
  `llvm-symbolizer --obj=nhl12.exe --relative-address 0x<RVA>`.

## 9. Build/run quick reference
- Env: `cmd /c 'call tools\rexenv.bat && …'` (VsDevCmd amd64 + LLVM clang 22 + ninja).
  Use the **PowerShell** tool, not Bash `cmd //c` (quoting).
- Codegen: `rexglue.exe --log-level info codegen app\nhl12_manifest.toml` (cwd=`app`,
  Release CLI ≈ 2.5 min, validates before emit).
- Build: `cmake --build app/out/build/win-amd64-debug --target nhl12`.
- Run: `nhl12.exe --game_data_root extracted --log_file build\run.log --log_level debug`.
- Manifest includes: `[fixups, switch_tables, gapfuncs, extrafuncs]`.

---
See [`xenondecomp-notes.md`](xenondecomp-notes.md) for the translation side and
[`../quirks/gotchas.md`](../quirks/gotchas.md) for failure modes to avoid regressing.
