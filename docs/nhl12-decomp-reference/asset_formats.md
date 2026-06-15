# NHL 12 Disc Asset Formats

*Survey from `docs/disc_inventory.csv` + magic-byte probe of every file.
VFS policy (plan §Phase 3.4): serve container files byte-identically;
game code parses its own archives. We do NOT need to unpack these for MVP.*

## Disc layout (game partition, 6.03 GB in 17 files)

| File | Size | Format | Role (hypothesis) |
|---|---|---|---|
| `default.xex` | 9.1 MB | XEX2 | the executable (`nhlzf.exe`) |
| `boot.big` | 9.8 MB | **BigEB v3** | boot-flow assets (legal/splash/loading UI) |
| `audioboot.big` / `audioboot2.big` | 1.9 / 3.5 MB | BigEB v3 | boot audio (menu music stingers?) |
| `cache.big` | 280 MB | BigEB v3 (~12,930 entries) | streamed-cacheable game data |
| `cacherender.big` | 1.25 GB | BigEB v3 | render assets (textures/models), cacheable |
| `nocache.big` | 2.45 GB | BigEB v3 | streamed-on-demand data (audio: commentary likely here) |
| `nocacherender.big` | 1.85 GB | BigEB v3 | render assets, streamed |
| `data0.big` | 23 MB | BigEB v3 | small data set (DB/rosters?) |
| `gamedata/aidata.big` | 17 KB | BigEB v3 | AI tuning |
| `gamedata/anim/faceposer.big` | 8.2 MB | BigEB v3 | facial animation |
| `gamedata/nis.big` | 133 MB | BigEB v3 | NIS = Non-Interactive Sequences (cinematics) |
| `gamedata/gps.big` | 1,031 B | **BIG4** (11 entries) | tiny config archive |
| `options.viv` | 284 B | **BIG4** (1 entry) | options/config seed |
| `nxeart` | 1.4 MB | PIRS | dashboard art — irrelevant |
| `$SystemUpdate/*` | 11.6 MB | PIRS/XMNP | system update — irrelevant, do not serve |

## Format notes

### BigEB v3 (`45 42 00 03` = "EB", ver 3)
EA Canada's big-file container of this era. Header (big-endian, observed):
`magic u16 "EB"` · `version u16 = 3` · `u32 file count` · `u32 = 0x400` (alignment?)
· `u32` (index size?). Entry table format TBD — only needed post-MVP (modding,
asset dumping). The interesting near-term use: extracting Xenos shader blobs for
Phase 4 shader-translation bring-up *before* live captures exist.

### BIG4 (`42 49 47 34`)
Classic EA BIG: `magic` · `u32 LE total size` (verified: matches file sizes) ·
`u32 BE count` · `u32 BE index end`. Entries: `u32 BE offset` · `u32 BE size` ·
ASCIIZ name. Trivially parseable — do `gps.big`/`options.viv` first as a smoke test
if we ever need archive contents.

### Expected interior formats (from EA Canada 2011 titles; unverified)
- **Audio:** EA Audiocore SNR/SNS ("SPS") streams; codecs EA-XAS and **EA-XMA**
  (Xbox 360). XMA decode path in Phase 5 must handle XMA inside EA wrappers.
- **Textures/models:** EA RenderWare-descendant / proprietary EAGL-era chunks; in
  `*render.big`. Not needed for MVP (game code does the parsing).
- **Cinematics:** `nis.big` — EA NIS animation/camera data. Audio/video for intro
  movies probably VP6/Bink inside a .big (no loose .vp6/.bik on disc — note: EA
  typically packs them).

## Implications for the recomp

1. **VFS is simple for MVP**: 15 game files + directory structure under `game:\`.
   No loose-file sprawl; byte-identical serving is easy to verify with hashes.
2. **I/O pattern risk**: 4.3 GB in `nocache*`/`*render` implies heavy streaming —
   the Nt file APIs + async I/O (XOverlapped) path must be solid, and seek-heavy
   reads from the user's dump should come from extracted files (not the raw ISO)
   for performance.
3. **No loose video files** — boot-flow legal/intro movies are packed; first-frame
   milestone may hit the game's movie player early. Watch for XMV vs VP6 usage in
   import list (XMV would show as XMA+Vd usage; VP6 is pure CPU).
