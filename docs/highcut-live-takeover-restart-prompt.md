# Kickoff — High-cut LIVE TAKEOVER, clean restart

**Use this as the opening prompt for a fresh session. This is a deliberate restart of the live-takeover
work. Do NOT build on the previous session's experiments — start the rendering/frame model fresh.**

---

## 0. Goal

Make **plume render the LIVE game** (high-cut path C: decode the guest command processor, render the
output through the **plume** RHI at flat logical sizes — no EDRAM) so the in-process plume window tracks
the running game across **all** screens: boot/intro, menus, create-a-player, loading, and gameplay.

**Priority order is fixed: PARITY first, then performance, then enhancements.** "Parity" = the plume
window shows what the game shows. Do not optimize or add enhancements until a screen renders correctly.

---

## 1. Start clean — discard the previous session's work

A prior session layered a stack of **uncommitted** experiments on top of the committed C-6 foundation —
per-draw pipeline/untile caches, framebuffer "retention", game-driven clears, partial-frame skipping,
and a filmstrip diagnostic. **They did not solve the visual problems and are to be discarded.** They are
not committed, so reverting is clean:

```
git status                       # confirm the only TRACKED modifications are this session's
git checkout -- gpu/hooks/plume_present.cpp \
                renderer/core/nhl_command_processor.cpp \
                renderer/core/nhl_command_processor.h \
                _live.ps1
git log --oneline -3             # HEAD should be the C-6 foundation: "highcut C-6 (foundation): live feed"
```

(Leave untracked files alone — the large `docs/nhl12-decomp-reference/` set and the `_*.ps1` helper
scripts predate that session. The `out/.../live_shots/` PNG folder, if present, is throwaway.)

Then **build clean from the committed foundation before writing anything new** (see §3). Do not reuse the
caches / retention / clear / skip logic. Approach the frame and render model from scratch.

> Auto-memory may surface notes from the discarded session (e.g. `highcut-composition-resolve-hostcopy`
> has a long C-6 log of failed attempts). Treat those attempt-logs as **history, not direction** — the
> empirical *observations* in §6 are worth keeping; the *approaches* are not.

---

## 2. What the committed foundation gives you (the real starting point)

The committed C-6 "live feed" is the foundation; build the new rendering model on this transport, or
replace it if a fresh look says so:

- **Co-run is proven.** beta-takeover (a D3D12 `CommandProcessor` subclass) and plume (Vulkan) run in
  **one process on separate threads** with no device-removed/TDR. They cannot share a GPU device, so each
  guest draw is decoded on the producer thread, serialized into a self-describing **packet**, handed across
  an in-memory **bridge**, and rebuilt + rendered on the plume thread.
- **Transport is proven.** The producer pushes each owned draw's packet (`HighcutLivePushDraw`) and commits
  a frame at a boundary (`HighcutLiveCommitFrame`); the plume consumer swaps the committed frame in and
  rebuilds its renderable draws.
- **The translator works.** A ported Xenos→SPIR-V translator (`gpu/spirv/`) turns guest VS/PS ucode into
  SPIR-V that plume accepts; the disk-replay path renders captured frames **correctly** (verifiable
  headless), so the per-draw decode/translate/untile/bind/draw chain is sound.

**The open problem is the LIVE result across screens** (see §6 for what was actually observed).

---

## 3. Build / run / verify

- **Build (from a normal shell, not agent cmd-from-bash):** `_build_beta.bat` → expect `BUILD_EXIT=0`.
  It runs vcvars64 + puts LLVM on PATH + `cmake --build out/build/win-amd64-relwithdebinfo --target nhllegacy`.
- **Run LIVE (interactive — the only way to exercise the co-run):** `_live.ps1` (committed version after
  the revert). It sets `NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_BETA_LIVE=1 NHL_BETA_LIVE_HOTKEY=1
  NHL_HIGHCUT_FRAME_CAPTURE=1 NHL_HIGHCUT_PRESENT=1 NHL_HIGHCUT_C5=1 NHL_HIGHCUT_LIVE_FEED=1`, launches the
  game, and you press **F10** at a live scene to arm the takeover. Watch the "NHL high-cut (plume Vulkan)"
  window. Logs: `out/build/win-amd64-relwithdebinfo/logs/nhllegacy_*.log`.
- **Headless verify of the CONSUMER render path:** `_c5render.ps1` replays a captured frame from disk (no
  F10). `$env:NHL_HIGHCUT_C5_SHOT="x.png"` dumps the rendered framebuffer to a PNG you can read. Use this
  to confirm any consumer change didn't regress the known-good disk render.
- **SDK:** `E:\Tools\rexglue-sdk` (headers/lib; `0.8.1.31-dev/win-amd64`). **Game data:** `H:\Emulators\
  games\XBOX\NHL Legacy - Vanilla`. **plume RHI:** `third_party/plume` (Vulkan backend).

### Key files
- `gpu/hooks/plume_present.cpp` — plume device/swapchain/present + the consumer: `RenderClear` (per-frame
  render), `LoadC5Frames` (build renderable draws from packets), `BuildRenderableDraw` (one draw → shaders/
  pipeline/buffers/textures/descriptors), the live bridge globals + `HighcutLivePushDraw`/`HighcutLiveCommitFrame`.
- `renderer/core/nhl_command_processor.cpp` / `.h` — the producer (`NhlD3D12CommandProcessor`): `IssueDraw`
  → `RenderBetaOwnedDraw` decodes/translates/untiles/builds the packet; the frame-boundary commit; `IssueCopy`
  (EDRAM resolves). Frame boundary currently keys on a guest-present counter.
- `gpu/hooks/highcut_draw_packet.h` — the per-draw packet layout (header + fetch/sys/vtx/shaders/textures/...).
- `gpu/spirv/` — the ported Xenos→SPIR-V translator.

### Hard architectural constraints (not negotiable, from prior sessions)
- plume **must be Vulkan** — a 2nd D3D12 device in-process TDRs rexglue's live device.
- producer (D3D12) and consumer (Vulkan) **cannot share a device** — hand work across via the bridge.
- the flat path has **no EDRAM** (the whole point of the high cut; do not reintroduce EDRAM emulation).

---

## 4. How to work this (process, learned the hard way)

1. **Observe before theorizing.** The previous session's central failure was inferring the frame/render
   model from log statistics and guessing fixes — repeatedly wrong. **Look at actual rendered output**
   first (read the live window via a framebuffer-readback-to-PNG across a run, or attach RenderDoc to the
   plume Vulkan device), screen by screen, before designing anything.
2. **One change at a time, verify, then iterate.** Live tests are expensive (interactive F10). Make each
   run count: change one thing, predict the outcome, check it.
3. **Keep the disk-replay path (`_c5render`) as a regression oracle** for any consumer change.
4. **Match the game, don't invent.** Parity means reproducing what the guest does (clears, draw order,
   blends, frame boundaries) — derive these from the guest command stream / registers, not heuristics.

---

## 5. First steps

1. Revert (§1), confirm at the C-6 foundation, `_build_beta.bat` → `BUILD_EXIT=0`.
2. Stand up a way to **see** the live output per screen (framebuffer→PNG filmstrip across a run, or
   RenderDoc). This is the prerequisite for everything else.
3. With real images in hand, decide the rendering/frame model and the first concrete parity target
   (recommend the simplest broken screen, not gameplay).

---

## 6. Appendix — raw observations from a prior look (verify independently; delete for a pure clean slate)

These are *empirical observations of the game's behavior*, not approaches. They cost live-test cycles to
obtain; re-verify rather than trust, but they may save time:

- **Several screens already render correctly** through the live path: the **language-select** screen
  (clouds + Union Jack + "ENGLISH") and the **"NHL Legacy Edition" press-start splash** both looked
  right. So the pipeline/textures/full-frame render path works.
- **The main menu renders as a solid blue fill.** Its fullscreen background appears to come across as its
  own tiny (≈1-draw) frame that renders flat blue and hides the menu elements. Open question the next
  session should answer **first** by inspecting that one draw: is it a missing/blank background **asset**
  (e.g. an animated/streamed/RTT background that doesn't reproduce — possibly the same class as the video
  issue below), or a real texture being mis-rendered?
- **Green/red blocky corruption == the known prerendered-VIDEO bug** (VP6, predates the high cut; see
  `nhl12_vp6_green_video_fix_plan.md`). It is **unrelated** to live-takeover rendering — ignore it.
- **Frame rate tracks the monitor.** On a 165 Hz display the game presents ≈140–165 frames/sec; most
  commits are tiny (1–13 draws) with rare full redraws — i.e. the game uses **retained-mode / partial
  frame** updates (it relies on the previous frame's pixels persisting). The flat path clears/rebuilds per
  commit, so the retained content is not carried unless explicitly handled. (Whether to model retention,
  and how, is for the fresh session to decide from observation — the prior attempts at it did not work.)
- **`RB_COPY_CONTROL.color_clear_enable` was never observed set** during live takeover — the game does not
  signal color clears through that bit on this path. Don't rely on it as a frame-clear signal.
