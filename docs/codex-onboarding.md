# NHL Legacy Recomp — Onboarding for Codex (secondary dev agent)

> You (Codex) are a secondary coding agent on this project, brought in to keep momentum when the
> primary agent (Claude Code) hits its session limit. This doc brings you up to speed on the project
> as a whole, the current focus, and the roadmap. It is self-contained — it does not assume access to
> the primary agent's private memory store (the key facts from it are inlined here).
>
> Date of writing: 2026-06-09. Treat any code `file:line` citation as a hint, not gospel — **verify
> against current source before asserting**. The codebase moves fast.

---

## 1. What this project is

A **static recompilation of the Xbox 360 game "NHL Legacy" (NHL 09-era engine) to native Windows
x64**, built on the **ReXGlue SDK** — a Xenia-derived X360 static-recomp toolkit by Tom Clay
(BSD-3-Clause; portions Ben Vanik / Xenia). The recompiled title runs as `nhllegacy.exe`.

The work has three intertwined goals:
1. **Faithful native execution** of the game (CPU recomp + GPU backend on real D3D12).
2. **Moddability** — loose-file asset overrides, editable team/player database, texture replacement.
3. **A custom owned GPU backend ("beta")** that reproduces the SDK's D3D12 output exactly, then
   becomes the seam for rendering enhancements and asset injection. **This is the current main focus.**

The repo is NOT a git repository (treat it as a plain working tree; there is no version control to
lean on — be careful with destructive edits).

---

## 2. Environment & build (READ THIS FIRST — most "my fix did nothing" bugs are build issues)

- **Platform:** Windows 11, PowerShell. Primary working dir: `e:\Repositories\nhl-legacy-recomp`.
- **SDK:** `E:\Tools\rexglue-sdk\0.8.0\win-amd64` (a `0.8.1.31-dev` build sits alongside). Headers under
  `.../include/rex/`. **It is a BINARY SDK — headers + static libs/DLL only, NO SDK source.** This
  matters constantly: you cannot read the SDK's `.cpp`; you reverse its behavior from headers + the
  fact that it is Xenia-derived (consult open-source Xenia `src/xenia/gpu/...` to infer internals).
- **Compiler:** clang at `C:\Program Files\LLVM\bin` (NOT on PATH by default). MSVC STL/Windows SDK
  come from **standalone VS 2022 Build Tools** vcvars64 (NOT full VS Community here).
- **Game data root:** `H:\Emulators\games\XBOX\NHL Legacy - Vanilla` (~31 GB extracted).

### Build command (PowerShell tool, ONE line — the multi-line caret form silently no-ops via Bash)
```
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && set "PATH=C:\Program Files\LLVM\bin;%PATH%" && cmake --build "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo" --target nhllegacy'
```
- Build dir: `out/build/win-amd64-relwithdebinfo` (RelWithDebInfo, links `rexruntimerd.dll`).
- Editing `CMakeLists.txt` triggers an automatic reconfigure on next `--build`.
- First-ever configure needs `-DCMAKE_PREFIX_PATH=E:/Tools/rexglue-sdk/0.8.0/win-amd64`.

### Build gotchas that have each cost multiple wasted iterations
1. **Ninja "no work to do" staleness:** ninja sometimes links WITHOUT recompiling an edited file, so
   your change never enters the binary. When an edit MUST take effect: delete its `.obj`
   (`out/build/.../CMakeFiles/nhllegacy.dir/.../<file>.cpp.obj`) or bump the source mtime, rebuild, and
   **confirm the `[n/m] Building CXX object ...<file>.obj` line appears**. Verify behavior via a unique
   log marker the new code emits.
2. **File lock:** a stray/hung `nhllegacy.exe` locks the output → link "permission denied". Run
   `Get-Process nhllegacy | Stop-Process -Force` before rebuilding.
3. **Run with the quoted path as ONE arg:** `-ArgumentList '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'` (otherwise the space splits it).
4. **Crash diagnosis:** native crashes land in the Windows Application-Error event log
   (`Get-WinEvent -ProviderName 'Application Error'`) with the exception code (e.g. heap corruption
   `0xC0000374`, div-by-zero `0xC0000094`).

### Run
```
nhllegacy.exe --game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"   # from the build dir
```
Logs: `out/build/.../logs/nhllegacy_NNN.log` (NNN increments per launch).

---

## 3. Capture / replay / screenshot (the core dev workflow for rendering)

Rendering work is driven by **GPU trace capture + offline-ish replay**, not by playing the game live.

- **Capture a frame/stream:** in-process via the SDK's `RequestFrameTrace`. Env-driven from our CP:
  - `NHL_CAPTURE_FRAME=<n>` → single-frame `.xtr`.
  - `NHL_CAPTURE_STREAM=<start>:<end>` → multi-frame streaming `.xtr` (use this when a resource was
    uploaded on an earlier frame — single-frame capture only dumps the resident set at capture time).
  - `NHL_CAPTURE_FULL=1` → force re-upload of ALL cached GPU resources during the window
    (`TracePlaybackWroteMemory` over all guest RAM) so textures uploaded before the window are
    captured. Makes traces self-contained (big: ~86-165 MB for 3D scenes) — this is what fixed the
    "edit-player model renders as a black silhouette" texture gap.
  - `NHL_HOTKEY_CAPTURE=1` → F9 in-game writes to `gpu_trace/scene_NN/`.
- **Replay through the live backend + presenter:** `NHL_REPLAY_XTR=<path.xtr>` (the host replays the
  trace instead of launching the guest, then captures `replay_frame.png`). The replay model executes
  every leaf PM4 packet in stream order but MUST SKIP three opcodes: `0x3F` INDIRECT_BUFFER, `0x37`
  INDIRECT_BUFFER_PFD (both → zero-length ring div0), and `0x3C` WAIT_REG_MEM (→ deadlocks the CP
  thread forever). All scene types replay faithfully (menu, player-select, edit-player, gameplay,
  instant-replay).
- **Live screenshot (ground truth):** `NHL_SHOT_FRAME=<n>` → `live_frame.png`. The faithful image
  oracle is `presenter()->CaptureGuestOutput()` — **do NOT** read guest RAM directly as an oracle (on
  the ROV path resolves write GPU-side memory, so CPU guest RAM is stale).
- **Five canonical scenes** are captured under `gpu_trace/scene_00..04/` (menu, intro/language,
  player-select, **scene_02 = create-a-player / EditPlayer**, **scene_04 = 3D gameplay/arena**).
- **Parse/inventory:** `tools/parse_gpu_trace.py`, `tools/trace_mem_inventory.py`,
  `tools/trace_reg_inspect.py` (needs `pip install python-snappy`).

There is also a standalone headless player `tools/replay` (`nhl-trace-replay`) — it replays draws
headless but can't extract the final image (no presenter), so the **in-host `NHL_REPLAY_XTR` path is
what you use for pixel validation**.

---

## 4. Repository / component map

- `renderer/core/` — **the custom GPU backend.** `nhl_command_processor.{cpp,h}` is THE main file
  (~3250 lines): subclasses the SDK `D3D12CommandProcessor`. Both the legacy delegate path and the new
  owned "beta" backend live here. `NhlD3D12GraphicsSystem` / `NhllegacyApp` wire it in. Injection seam
  is `OnPreSetup`/`SetupContext` (the concrete D3D12 backend's symbols ARE exported, so we can
  construct the SDK's worker caches ourselves).
- `src/` — app/runtime glue: `union_device.{h,cpp}` (loose-file overlay VFS), `nhllegacy_app.h`
  (`OnPreSetup`/`OnPostSetup`/`LaunchModule` overrides, device registration, replay hooks).
- `tools/replay/` — `xtr_player.cpp` (the .xtr replay model), `image_dump.cpp` (PNG writer). Compiled
  into the host for `NHL_REPLAY_XTR`; also builds the standalone `nhl-trace-replay` (option OFF by
  default).
- `tools/*.py` — trace parsing/inventory.
- `docs/` — design + handoff docs. **Start here:** `tier1-backend-build-order.md` (backend plan),
  `faithful-edram-fold-handoff.md` (the active hard task, see §6/§7), this file.
- Sibling repos (NOT under this tree):
  - `nhl-database-studio/` — Rust workspace for the team/player DB. `crates/tdb-core` (read/write the
    `.db`/TDB format, `write_tdb`), `crates/tdb-expand` (grow tables). `crates/tdb-rx2-ffi` (static
    lib the recomp links for runtime `.dds`→`.rx2` texture rebuild). `nhl-database-studio-old/` is
    DEPRECATED — ignore it.

---

## 5. Recomp & asset architecture (context you'll need)

- **Loose-file overrides (IMPLEMENTED):** `UnionDevice` union-mounts a writable upper
  (`cache_root()/guest`) over a read-only lower (`game_data_root()/_compiled`). The guest's native
  loader already tries `cache:\<path>` (loose) before falling back to the `.big` archive, so dropping
  a loose file into `_compiled` overrides it with **zero format cracking and zero guest patching**.
  Verified for rendering `.rx2`, attribdb `.vlt/.bin`, FE `.bin`.
- **`.big` archives are EA `EB\0\3` format** — hashed names + chunk-LZX compression, **un-reversed and
  NOT repackable** (no LZX encoder exists). So the `.big` stay on disk as inert fallback; all
  modding happens via the loose `_compiled` overlay. Do not attempt to repack `.big`.
- **RX2 loose textures:** edit a loose `.dds`; the recomp rebuilds the `.rx2` at load time via the
  `tdb-rx2-ffi` static lib. (Runtime `.dds`→`.rx2` override.)
- **Database:** team/player/career data is in TDB-format `.db` files (`nhlng.db` holds career stats).
  Editing/growing tables is done in `nhl-database-studio` (`tdb-core::write_tdb`, `tdb-expand`).
  Expansion ceilings: u16 row cap (65535/table), ID bit-widths, and game-side caps — check
  `nhl-database-studio` docs before growing tables.
- **rexruntime wide-CRT gotcha:** any tool linking `rexruntime.dll` needs a wide entry (`wmain`) +
  dynamic CRT, else `_get_wpgmptr` fail-fasts at startup.

---

## 6. The "beta" owned GPU backend — current state (the active arena)

**What it is:** instead of delegating draws to the SDK's command processor (the "alpha"/delegate path,
kept as a pixel-exact regression oracle), the beta backend constructs **our own instances** of the
SDK's five worker caches (SharedMemory, RenderTargetCache, TextureCache, PrimitiveProcessor,
PipelineCache) and reimplements `IssueDraw/IssueCopy/IssueSwap/LoadShader` to drive them. This reuses
the hard, correct machinery (EDRAM tiled store/resolve, ucode→DXBC translation, primitive expansion)
while giving us ownership of orchestration + a texture-cache instance to hook for injection.

**How it's gated (important for no-regression):**
- `NHL_BACKEND=beta` — builds the beta caches + validates shader translation; **does NOT take over
  rendering** (live path unchanged). Byte-identical to default.
- `NHL_BETA_TAKEOVER=1` — full-frame takeover: our IssueDraw renders every draw into our own targets,
  base draws are no-op'd (no shared-command-list contamination → no device loss). This is the mode all
  the rendering work runs in.
- `NHL_BETA_EDRAM=1` — opt-in: route the owned draw through the RT cache's EDRAM/resolve path. **OFF by
  default**, so the default takeover path is the **offscreen** path (render to one flat 1280×720 RT).
- `NHL_BETA_DEPTH=1` — depth target bring-up (functional). Plus many `NHL_BETA_*` diagnostics
  (GPUDUMP, EDRAM_DIAG, RESOLVE_DIAG, DEPTH_DIAG, SKIP_RESOLVE, BIND_DIAG, SKIP_DRAW, ONLY_DRAW, ...).

**Proven working:**
- **The textured 2D menu renders at PARITY** — verified 45 dB PSNR vs the real oracle (the alleged
  gamma/color and flag-scale "gaps" were a bogus reference and don't exist). Required: a
  pipeline-state-tracking fix (`SetExternalPipeline`/`SetExternalGraphicsRootSignature` so the texture
  cache's untile compute Dispatch actually runs), the `color_exp_bias` fix, and `kQuadList` →
  `LINELIST_ADJ` topology to feed the SDK quad geometry shader. Bindful (not bindless) root sigs are
  forced via `d3d12_bindless=false` cvar in `OnPreSetup` (gated on beta).
- **Depth** is implemented + functional (resource/DSV/clear/bind via RT cache Update; zfunc reaches HW).
- **Texture injection (Stage 0 + Stage 1 plumbing)** works: addr→asset map via an injection registry
  (FNV-prefix hash), IssueDraw correlation, `<trace>.inject.json` sidecar, auto-load on replay.
- **The offscreen path renders full 3D scenes** — scene_02 create-a-player renders the complete UI +
  a fully-textured 3D player model. (See the caveat in §7.)

**Minimal recipe that "just works" for the menu:** `NHL_BACKEND=beta NHL_BETA_TAKEOVER=1` (tex/samp
default-on, shmem default 512 MB).

---

## 7. Current focus & roadmap

### Immediate focus: get 3D rendering correct in the beta backend
3D scenes (create-a-player `scene_02`, gameplay `scene_04`) are the frontier. Status:

- **SHIPPED (pragmatic):** the **offscreen path** renders the textured 3D player now. It is already
  the default takeover path (no flag needed). **Known caveat:** on a flat RT the player has a
  positional artifact — it lands far-right with a small left wrap-fragment, where the oracle has one
  centered player. Cause: the model is drawn via two Xenos `PA_SC_WINDOW_OFFSET` passes into a folded
  640-pitch EDRAM surface; a flat RT can't express that fold, so the two passes splat apart.
- **THE HARD TASK (next, deferred to a focused session):** the **faithful EDRAM fold**. Drive the SDK
  render-target cache so its native un-fold resolve reconstructs the player at the correct position —
  fixing the artifact AND generalizing to all 3D (scene_04 has the same root cause). **A full handoff
  brief exists: [`docs/faithful-edram-fold-handoff.md`](faithful-edram-fold-handoff.md).** Read it
  before touching this — it lists what's proven, what's been ruled out (VP_VPORT, WIDE-RT, WINOFF
  block-stack, bypass-fold), the closed-SDK constraint (use Xenia source to infer the cache's driving),
  the validation loop, and concrete first moves.
- A cheaper interim option (documented but not chosen): correct the offscreen player POSITION via a
  screen-space un-fold viewport transform — per-scene-fragile, doesn't generalize, but quick parity.

### Broader roadmap (rough priority order)
1. **3D rendering parity** (above) — the gate for everything 3D.
2. **Tier-2 rendering enhancements** — once the owned backend drives all scenes, the texture/pipeline
   caches are the hook for HD texture replacement, etc. (subclass the abstract `TextureCache` base only
   for per-resource upload hooks; never subclass EDRAM).
3. **Asset modding pipeline** — loose-file overlay is done; broaden coverage (audio sbs, movies, FE
   nested archives are unverified) and polish the `.dds`→`.rx2` texture workflow.
4. **Database expansion** — bigger rosters/teams via `tdb-expand`, respecting the ceilings.

---

## 8. Working conventions & hard constraints

- **No-regression bar (non-negotiable):** the default/live path and `NHL_BACKEND=beta` WITHOUT
  takeover must stay **byte-identical**. The validated menu (`scene_00`) and intro/language
  (`scene_03`) frames must not regress. Every new behavior is **env-gated** and takeover-only. 2D menu
  draws (vte=0x300, viewport-transform disabled, guest_w==RT width) take an untouched 1:1 viewport
  branch — keep it that way.
- **Validate visually.** Rendering claims must be backed by the actual PNG (`replay_frame.png` /
  `beta_owned_draw.png` vs `live_frame.png` / an `oracle_*.png`). A "dark-pixel" metric is unreliable
  (injected/raw player content can render GREEN via mis-swizzle and be missed) — use a color-agnostic
  non-background metric or just look at the image.
- **The SDK is closed.** When you need to know how the SDK behaves internally, read the headers under
  `E:/Tools/rexglue-sdk/0.8.0/win-amd64/include/rex/` and cross-reference **open-source Xenia**
  (`xenia-project/xenia`, `src/xenia/gpu/...`) — the SDK is a fork. Many classes are `final` (can't
  subclass) and some symbols are unexported (e.g. `TraceDump`/`TracePlayer`/`TraceReader` — that's why
  offline replay was self-written).
- **Beta uses a DEFERRED command list** that only executes at IssueSwap. You can't CPU-readback a
  just-recorded draw mid-frame; downstream draws that sample a resolved dest are later in the SAME
  list, so the dest must be populated in `beta_shared_memory_` before they run. Fixes must live inside
  this model.
- **Keep diagnostics.** The many `NHL_BETA_*` env toggles are intentional and should stay in-tree
  (env-gated, off by default) — they're the bisection toolkit.
- **Coordinate with the primary agent.** This is a shared codebase with an evolving design captured in
  `docs/` and the primary agent's memory. When you finish a chunk, leave a short written note (in the
  relevant `docs/*.md` or a new handoff file) describing what changed, what's proven, and what's next —
  mirror the style of `faithful-edram-fold-handoff.md`. That's how state survives the agent handoff.

---

## 9. Quick-start checklist for your first session

1. Confirm the build is green (run the §2 build command; watch for the compile line, not "no work").
2. Reproduce a known-good render: `run_edram.ps1 -Scene scene_02 -Frame 30 -Depth 1 -Edram 0` →
   open `out/build/.../beta_owned_draw.png` (should show the create-player screen + textured player).
3. Read `docs/faithful-edram-fold-handoff.md` if you're picking up the 3D fold task.
4. Make changes env-gated; validate against the oracle PNG; keep menu/intro byte-identical.
5. Write down what you did before you stop.
```
# Common launchers (from the build dir / repo root)
run_edram.ps1   -Scene scene_02 -Frame 30 -Depth 1 [-Edram 0]   # beta takeover replay -> beta_owned_draw.png
run_oracle.ps1                                                   # base-path reference  -> live_frame.png
```
