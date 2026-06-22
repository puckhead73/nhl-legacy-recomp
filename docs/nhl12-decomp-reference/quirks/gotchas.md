# Quirks, Gotchas & Fragile Systems

Failure modes and traps — for the recompilation, the binary, and the game's own
design. Most are CONFIRMED from project memory + bring-up experience; intent quirks
are marked.

## A. Recompilation traps (CONFIRMED — don't regress)

1. **setjmp/longjmp must be configured.** EA wraps init in setjmp/longjmp; only the
   *configured* addresses (`0x83366050` / `0x833643B0`) get host non-local return.
   Miss them → protected init returns garbage → null-object AV in the front end. The
   single highest-leverage fix. See
   [`../recompilation/xenondecomp-notes.md`](../recompilation/xenondecomp-notes.md) §5.
2. **EA jump tables are invisible to stock analysis.** EA codegen interleaves nops;
   XenonAnalyse finds 0 tables. Use the structural detector (505 tables). A missed
   table = a `bctr` into the void at runtime.
3. **Indirect-only ("gap") functions** (860) aren't found by static analysis. Emit
   them with **no size** (analyzer CF-scans the extent); an explicit size *caps* the
   scan → "use of undeclared label" at compile time.
4. **Codegen file partitioning reshuffles on any function add/remove** → all 154 TUs
   change → full rebuild. Batch config changes; validate on the Release CLI first.
5. **Import stubs must use the `PPC_FUNC` macro signature** — MSVC mangles `__restrict`
   otherwise → link errors.
6. **~50 EA-toolchain instruction emitters** are a *patch* to XenonRecomp (`ddd128bc`)
   — reapply after a fresh clone or you get 6,681 missing-instruction errors.
7. **libmspack symlinks** in RexGlue don't materialize on Windows (`core.symlinks`)
   → clang compiles the link path text. Copy the real files after a fresh submodule init.
8. **VFS device order:** mount `cache:`/`update:` on `\Device\Harddisk1`, not
   Harddisk0 — RexGlue's `NullDevice@Harddisk0` prefix-shadows later mounts.
9. **`xboxkrnl_usbcam.cpp` is shipped but not compiled** by RexGlue's kernel CMake →
   `__imp__XUsbcam*` undefined at link. Stub it.
10. **ABI fork is load-bearing.** RexGlue's `PPCContext` ≠ XenonRecomp's (extra
    `vscr_sat` + `last_indirect_target`). You cannot mix runtimes; re-recompile with
    RexGlue's codegen. See [`../recompilation/rexglue-runtime.md`](../recompilation/rexglue-runtime.md) §2.

## B. Numeric / endianness hazards (CONFIRMED relevance)
11. **Big-endian everywhere.** Every guest access byte-swaps; all serialized data is
    BE. A dropped swap = silent corruption.
12. **FP rounding/flush modes** (`fpscr`, `disableFlushMode()`) must match, or physics/
    AI drift subtly without crashing.
13. **VMX lane order + saturation** (`vscr_sat`) — `rw::math`/physics are vector-heavy;
    wrong lanes = wrong geometry.
14. **Determinism (`randomd0`)** must be bit-exact or replays/netcode diverge.

## C. Threading hazards (CONFIRMED relevance)
15. **PPC weak memory model.** EA relies on explicit `lwsync`/`sync` barriers; a dropped
    fence becomes a rare, irreproducible sim/render desync or streaming corruption.
    **Highest-risk silent recomp bug class.**
16. **Two-player animations advance in lockstep** on the sim thread; ordering
    perturbation can desync the pair (`twoplayeranim`). See
    [`../animation/animation-system.md`](../animation/animation-system.md).
17. **Busy-spins assume the 360 scheduler** (e.g. XMA worker on reg `0601`). Benign
    when fed; can peg a core when starved.

## D. Content/runtime traps (CONFIRMED)
18. **The game reads ALL assets from `cache:\` and never falls back to `d:\`.** An empty
    `cache:` stalls boot before the first frame. The game does **not** unpack the
    `.big`s itself — it expects a pre-populated HDD install. This is the current
    blocker. See [`../assets/asset-pipeline.md`](../assets/asset-pipeline.md).
19. **The boot-gate trio** (`ExGetXConfigSetting`, `XexCheckExecutablePrivilege`,
    `XeCrypt*`) causes *silent* exit if wrong. Implement honestly/early.
20. **ROV is mandatory for the 3D scene.** The RTV path leaves gameplay black while UI
    renders — a deceptive "half-working" failure. See
    [`../graphics/rendering-pipeline.md`](../graphics/rendering-pipeline.md) §2.
21. **Shader translation key must encode every visual mode** (ROV, MSAA, gamma, res
    scale) or a cached translation is reused in the wrong state → corruption. The
    blue/green texture bugs live here.

## E. Binary/source quirks (CONFIRMED observations)
22. **Two Perforce roots** in the strings (`c:\p4\nhl\main\…` and
    `c:\p4\sync1\nhl12\release\…`) — the build merged an EA-wide tech branch with the
    NHL 12 release branch. Don't assume one consistent tree. `[P4]`
23. **Retail build leaked full source paths + RTTI + unit tests** (`vector_test.cpp`
    shipped). Lucky for us — this is the entire basis of the codebase map.
24. **XDK Feb-2011 shader compiler is statically linked** (`xgraphics\ucode`) — XDK
    code runs as recompiled game code. `[P4]`
25. **TUs 580–588 were excluded** from the Phase 2 XenonRecomp build (it emitted a
    duplicated tail range — an upstream bug). Phase 4 RexGlue codegen doesn't have this.

## F. Things not to do (from the renderer guardrails `[INV]`)
- Don't apply movie-DXT hacks or disable MSAA to "fix" textures — they reintroduce the
  blue bug. The known-good state is the `WORKING_RENDERER_NEVER_DELETE` backup.
- See [`../nhl12_renderer_regression_guardrails.md`](../nhl12_renderer_regression_guardrails.md)
  for the full list.

See [`magic-values.md`](magic-values.md) for the constants behind these and
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).
