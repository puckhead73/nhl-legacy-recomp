# NHL12 Crash Debugging

This project uses two crash-capture paths for renderer work:

- Windows Error Reporting LocalDumps, enabled by `tools\enable_nhl12_crash_dumps.ps1`.
- WinDbg, launched by `tools\run_nhl12_windbg.ps1`, which breaks on access violations and writes a full dump plus a debugger log.

## Stable Launch Under WinDbg

Use this when testing the current stable renderer path:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_nhl12_windbg.ps1 -LogFile build\nhl12_windbg_stable.log
```

To verify the launcher without starting the game:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_nhl12_windbg.ps1 -DryRun
```

## Reproduce The Experimental Cube/2D Crash

Use this only when deliberately testing the disabled cube-material fallback:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_nhl12_windbg.ps1 -Cube2DFallback -LogFile build\nhl12_windbg_cube2d_crash.log
```

Press through the EA intro until the rink/first 3D scene starts. If the game hits an access violation, WinDbg should write:

- `build\debugger\nhl12_windbg.log`
- `build\crash_dumps\nhl12_windbg_av_*.dmp`
- the game log passed with `-LogFile`

If WinDbg Preview does not catch the crash cleanly, Windows Error Reporting should still write a full dump to `build\crash_dumps`.

If WinDbg stops at the first loader breakpoint and the commands were not armed, paste this in the WinDbg command box:

```text
$$><C:\Users\thrif\Documents\Github\nhl12-recomp\build\debugger\nhl12_windbg_startup.wds
```

If WinDbg is already stopped on an access violation, paste this directly:

```text
sxd -c2 ".ecxr; r; kv; !analyze -v; lm; .dump /ma /u C:/Users/thrif/Documents/Github/nhl12-recomp/build/crash_dumps/nhl12_secondchance_av.dmp" av; g
```

The launcher intentionally ignores first-chance access violations. Those can be normal handled exceptions during startup, especially around the pre-launch menu. A useful dump is the second-chance exception, where the process is actually going down.

If WinDbg is already stopped at `nhl12+0x229d08d`, write the dump from that exact crash with:

```text
.dump /ma /u C:/Users/thrif/Documents/Github/nhl12-recomp/build/crash_dumps/nhl12_manual_229d08d.dmp
```

## Collect The Result

After a crash:

```powershell
powershell -ExecutionPolicy Bypass -File tools\collect_nhl12_crash_info.ps1
```

The important fields are the exception code, fault offset/RVA, crashing module, call stack, and the nearest renderer log lines before the crash.

## Current Renderer Debug Context

The experimental `nhl_cube_material_2d_fallback` path is disabled by default because it crashed before reaching the known equipment `tfetchCube`/2D DXT1 mismatch. Keep `nhl_fix_cube_reflection_fetches=true` enabled for the stable NHL12 cubemap reflection path, but only enable `nhl_cube_material_2d_fallback=true` when running under the debugger.

## Dump Finding: `nhl12+0x229d08d`

The June 15, 2026 WinDbg dump for the experimental cube/2D fallback crashed at:

```text
nhl12+0x229d08d
mov eax,dword ptr [rdx+rcx]
read address 00000001`00000038
```

Native disassembly maps this to guest function `sub_82B7F838` in `app/generated/default/nhl12_recomp.50.cpp`. The generated guest code had:

```cpp
ctx.r30.u64 = REX_LOAD_U32(ctx.r31.u32 + 0);
ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 56);
```

`*(r31)` was zero, so `r30 + 56` became guest address `0x38`. The function already has a cleanup/failure path at `loc_82B7F8B0` that calls `sub_82BA4E78`, clears `*r31`, and returns `2`. The NHL12 patch adds a guard after loading `r30` and routes null through that existing failure path.
