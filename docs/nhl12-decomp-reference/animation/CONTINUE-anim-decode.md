# Continuation prompt — NHL Legacy animation decode (headless-invoke)

Paste the block below to resume. Everything needed is in this repo; the full
technical spec is in [`cba-format.md`](cba-format.md) §6 and the `animation-assets-located`
auto-memory.

---

## Prompt

> Continue the NHL Legacy animation-extraction work. Goal: get usable per-bone
> keyframe tracks (quaternions + translations) out of `gamedata/anim/anim.cba`
> (3,577 `DctAnimationAsset` clips). Read
> `docs/nhl12-decomp-reference/animation/cba-format.md` (esp. §6) first — it has
> the full reverse-engineered decode spec.
>
> **State of play:** The `.cba` container + record format is fully decoded
> (`tools/cba_parse.py`). The runtime-hook route is DEAD (the statically-traced
> chain `sub_82CA9B20/9618/7BD0` never fires in real gameplay — proven via the
> self-announcing `gpu/hooks/anim_capture.cpp`; 1104 `[highcut]` control hits vs 0
> `[anim]`). We pivoted to **headless-invoking the real recompiled decoder** —
> running the game's own self-contained decode functions offline.
>
> **Decided architecture:** add a gated `NHL_ANIM_DECODE=<out_path>` mode INSIDE
> `nhllegacy` (it already links all 366 recomp TUs; a standalone harness would have
> to recompile them). Hook it in `NhllegacyApp::OnPostSetup` (see `src/nhllegacy_app.h`):
> when the env var is set, run the decode pipeline instead of the game, dump, exit.
>
> **The decode pipeline (per clip), from cba-format.md §6:**
> 1. Build a controller struct in guest RAM: `{+0 → descriptor D, +4 → {outerCount}, +12 → asset}`.
> 2. `sub_82CA8C60(controller, asset)` — primes/unpacks into 64-byte key records (allocates → needs the guest heap, which `Runtime::Setup` provides).
> 3. seek (`sub_82CE3548`).
> 4. `sub_82CA7BD0(controller, out4, out5)` ×3.
> - **asset** = the GD.DATA `DctAnimationAsset` record copied into guest RAM with each array `(count,cap,offset,0)` descriptor's offset rewritten to an absolute guest pointer (inline scalars unchanged; `(u16)[asset+2]`=block-quant, `(u8)[asset+4]`=subblock; array ptrs at +28/+40/+52/+76/+88).
> - **descriptor D** = `{+0 base→64-byte records, +4 rowCount, +8 rowStride, +12 NumKeys}`; `sub_82CE18E0(D,row,col)` = `base + ((stride*row+col)<<6)`.
> - **out4** = `NumKeys*64` bytes; each 64-byte record = 8 quantized int16 quaternions.
> - **out5** = 12-byte `{float maxErr (init huge-negative), u32, u32}`.
> - int16 quat → float = `v/32767`; components are delta-coded across keys (prefix-sum; see `sub_82CE1900`).
>
> **First hurdle to solve:** calling a guest function (`sub_82CA7BD0` etc.) from host
> code needs a valid guest `PPCContext` + guest stack (r1). Find how the SDK/kernel
> starts the guest XEX entry thread and reuse that path (or `rex::ppc` helpers) to
> set up a context + stack, rather than hand-rolling it. The decode functions are
> confirmed self-contained (no kernel calls), so once a context+stack exist and the
> asset is built in guest RAM, the calls are pure.
>
> **Function anchors** (`generated/default/`): `sub_82CA7BD0` @ recomp.70.cpp:60381,
> `sub_82CA7330` @ 59142, `sub_82CA7390` @ 59198, `sub_82CA8C60` @ 62940;
> `sub_82CE18E0` @ recomp.73.cpp:13344, `sub_82CE1900` @ 13362, `sub_82CE19F0` @ 13490.
> Constants: cosine basis table @ `0x83A413B0`, scalar floats near `0x8208xxxx`.
>
> **Build/run:** game = `out/build/win-amd64-vk-ffx`; build via
> `scripts/_game_ffx_build.bat` (or `cmake --build ... --target nhllegacy` inside a
> vcvars64 shell — clang needs `INCLUDE` set, so run the build inside `cmd` that
> sourced vcvars). Run: `nhllegacy.exe --game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"`.
> The `Runtime::Setup` headless recipe template is `tools/replay/src/trace_replay.cpp`.
>
> **Validation plan:** decode ONE known clip first (e.g. `CHKP04_SP_SHFBF_3a`),
> confirm decoded quaternions are unit-length and translations are sane, before
> scaling to all 3,577. Optionally cross-check against a GPU skinning-matrix capture.
>
> Start with the host→guest call setup + a single-clip proof, then scale + export
> (glTF/FBX or a simple JSON of per-bone tracks) and map to the bind-pose skeletons
> (`cache:\rendering\skeleton_bindpose\*.rx2`) using FPS/Distance from the
> `ClipControllerAsset` (`0x75e9ea49`).

---

## Side note (not animation)
Two SDK source fixes were staged and rebuilt this session — VP6-PROBE/DUMP removal
(`texture_cache.cpp`) + Vulkan discrete-GPU preference (`vulkan_provider.cpp`). If the
game wasn't relinked against the fresh SDK install yet, do that
(`scripts/_game_ffx_build.bat`). See the `perf-report-gpu-selection` memory.
