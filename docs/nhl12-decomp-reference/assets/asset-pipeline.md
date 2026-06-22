# Asset Pipeline — Archives, Cache, and Streaming

How NHL 12 stores and loads its data. Combines the disc survey
([`../asset_formats.md`](../asset_formats.md), CONFIRMED magic-byte probe) with the
`cache:\` open-call log captured at runtime (`[RT]`) and the RenderWare/EA format
knowledge from the module map.

---

## 1. Two-stage storage model (CONFIRMED `[RT]`)

NHL 12 does **not** read assets from the disc image directly at runtime. It uses a
**360-HDD install** model:

```
Disc (.big archives)  ──install──▶  cache:\ (unpacked loose tree + .big copies)  ──▶  game
```

The game reads **everything** from the `cache:` VFS device and **never falls back to
`d:\`**. On a real 360, first boot copies/unpacks the disc into the HDD cache; the
game's `FileCopier`/`AssetStream` repopulates an empty cache. This is the **current
blocker**: the recompiled build stalls before the first frame because `cache:\` is
not populated and the game's own unpack step hasn't been made to run. `[RT]` See
[`../HANDOFF.md`](../HANDOFF.md).

## 2. Disc layout (CONFIRMED — game partition, ~6 GB / 17 files)

| File | Size | Format | Role |
|---|---|---|---|
| `default.xex` | 9.1 MB | XEX2 | the executable (`nhlzf.exe`) |
| `boot.big` | 9.8 MB | BigEB v3 | boot-flow assets (legal/splash/loading UI) |
| `audioboot.big` / `audioboot2.big` | 1.9 / 3.5 MB | BigEB v3 | boot audio |
| `cache.big` | 280 MB | BigEB v3 (~12,930 entries) | streamed-cacheable game data |
| `cacherender.big` | 1.25 GB | BigEB v3 | render assets (textures/models), cacheable |
| `nocache.big` | 2.45 GB | BigEB v3 | streamed-on-demand data (commentary audio likely) |
| `nocacherender.big` | 1.85 GB | BigEB v3 | render assets, streamed |
| `data0.big` | 23 MB | BigEB v3 | small data set (DB/rosters?) |
| `gamedata/aidata.big` | 17 KB | BigEB v3 | **AI tuning** |
| `gamedata/anim/faceposer.big` | 8.2 MB | BigEB v3 | **facial animation** |
| `gamedata/nis.big` | 133 MB | BigEB v3 | **NIS** = Non-Interactive Sequences (cinematics) |
| `gamedata/gps.big` | 1,031 B | BIG4 | tiny config archive |
| `options.viv` | 284 B | BIG4 | options/config seed |
| `nxeart`, `$SystemUpdate/*` | — | PIRS/XMNP | dashboard art / system update — **not served** |

## 3. Archive formats

### BigEB v3 — `45 42 00 03` ("EB", ver 3) — CONFIRMED magic, TOC partial
EA Canada's `.big` container of this era. Header (big-endian, observed):
`u16 "EB"` · `u16 version = 3` · `u32 file count` · `u32 = 0x400` (alignment) ·
`u32` (index size?). Observed file counts: `data0`=2, `boot`=0x228, `cache`=0x3282.
**Entry-table layout is UNKNOWN** (TBD) and **may carry refpack compression** on
entries. Writing `tools/unpack_big.py` for this format is the immediate next task to
populate `cache:\`.

### BIG4 — `42 49 47 34` ("BIG4") — CONFIRMED fully
Classic EA BIG: `magic` · `u32 LE total size` · `u32 BE count` · `u32 BE index end`.
Entries: `u32 BE offset` · `u32 BE size` · ASCIIZ name. Trivially parseable
(`gps.big`, `options.viv`). Good smoke test for an unpacker.

## 4. What `cache:\` must contain (CONFIRMED from `NtCreateFile` log `[RT]`)

The game opens these paths under `cache:\` (distinct list from the run log). They are
**not loose on disc** — they live *inside* the `.big`s and must be unpacked:

```
cache:\{data0,cache,nocache,audioboot}.big        ← .big copies (exist in extracted/)
cache:\rendering\boot\texlib_*.rx2                ← RenderWare texture libraries
cache:\rendering\skeleton_bindpose\*.rx2          ← skeleton bind poses
cache:\shaders\*.fxo                              ← compiled shaders
cache:\AttribDB\renddb.{bin,vlt}                  ← render/material attribute DB
cache:\audio\music\*.{csi,sbr,xml}                ← music banks/cues
cache:\audio\tuning\mixer_*                       ← audio mixer tuning
cache:\audio\reverb\*.irf                         ← reverb impulse responses
cache:\audio\NA_En_loadfile.xml                   ← audio locale manifest
cache:\fe\profile\default.png                     ← frontend profile art
cache:\scrape\{boot.scrape,scenedef.lua}          ← Lua scene/boot definition
cache:\debug\courbd.ttf                           ← debug font (Courier Bold)
cache:\467414.ver                                 ← version stamp
```

## 5. Interior asset formats (CONFIRMED extensions, INFERRED internals)

| Ext | What | Notes |
|---|---|---|
| `.rx2` | RenderWare 4 asset chunk (textures, skeletons, models) | `rendering/boot/texlib_*`, `skeleton_bindpose/*`. The RW chunked stream format. |
| `.fxo` | Compiled shader object | D3D shader microcode (the XDK `xgraphics\ucode` compiler produced these). Translated by RexGlue at runtime. |
| `renddb.{bin,vlt}` | Render/material **attribute database** | `.vlt` = "vault" (EA's hashed key→value store). Drives material/render attributes — relevant to the equipment-texture work in `graphics/`. |
| `.csi` / `.sbr` | Audio cue script / sound bank | RWAudioCore data; cues + banked samples. |
| `.irf` | Impulse-response reverb | Convolution reverb data. |
| `.scrape` / `scenedef.lua` | Scene/boot definition | **Lua** — the boot/scene flow is partly data/script-driven. |
| `.big` (gamedata) | sub-archives | `nis.big` (cinematics), `faceposer.big` (facial anim), `aidata.big` (AI tuning). |
| `.viv` | tiny EA config | BIG4 wrapper. |

## 6. Streaming & I/O (CONFIRMED relevance)

- **4+ GB lives in `nocache*`/`*render`** → heavy runtime streaming. The async file
  path (`Nt*` + overlapped IO via `AssetStream` threads) must be solid. `[RT]`
- The **AssetStream** pipeline is **load → unpack → translate** (the "translate" step
  is endian/format fixup into runtime form). `[RT]`
- For host performance, serve from **extracted files**, not the raw ISO (seek-heavy).

## 7. Open questions
- BigEB v3 entry-table layout + compression (blocks `tools/unpack_big.py`).
- Whether mounting `cache:\cache.big` directly is enough (game reads members
  internally) vs. needing the loose tree unpacked.
- The game's own cache-build trigger (`FileCopier::Thread` / `AssetStream::Unpack`) —
  making it run would self-populate `cache:\`.
- `.vlt` vault hashing scheme (needed to read `renddb`).

See [`save-load/serialization.md`](../save-load/serialization.md) for profile/roster
persistence and [`../unknowns/open-questions.md`](../unknowns/open-questions.md).
