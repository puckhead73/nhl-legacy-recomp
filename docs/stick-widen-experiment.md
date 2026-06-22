# Stick-widen experiment — does the engine read the `stick` field as 8 bits?

**Goal.** Close the one open viability gate ("Gate B") for expanding hockey
sticks past 128: does the game read the `stick` equipment index *generically*
from the self-describing TDB descriptor (so widening the field is honored with
no code patch), or does it mask the value to 7 bits somewhere downstream?

**Method.** Widen the `stick` field from 7→8 bits in the per-player equipment
table, set every player's stick to **200**, and observe which asset the game
requests for that player:

- requests index **200** (`stick200.big` / `texlib_200.rx2`) → **generic read,
  Gate B is FREE** — widening the schema is enough.
- requests index **72** (`stick72.big` / `texlib_72.rx2`, since `200 & 0x7F =
  72`) → a hardcoded 7-bit mask exists → **Gate B needs a recomp patch**, now
  precisely located by the call site.

## What was built / changed

- **`tools/stick_widen/`** — Rust tool (depends on the studio's `tdb-core`):
  - `inspect <db> [table|field|--list]` — dump tables / a table's fields.
  - `widen <in> <out> [table=xmja] [field=wXFx] [value=200] [limit=all]` —
    endian-aware bit-insert that opens one free bit at the field, widens the
    descriptor 7→8, shifts later fields +1, bumps `record_length_bits`, sets
    the test value, and reseals all CRCs via `tdb_core::write_tdb`. Verifies the
    round-trip (record0 reads back the value, CRC `invalid_count == 0`).
  - `cmprec <a> <b> <table> <rec>` — proves the bit-insert preserved every
    other field (used to confirm only `wXFx` changed; the `wBIz` player key was
    byte-preserved).
- **Findings about the live DB** (`H:\…\NHL Legacy - Vanilla\_compiled\db\nhlng.db`,
  big-endian, 134 tables):
  - per-player equipment table is **`xmja`** (community name `ajmx` reversed;
    5066 records = all players), stick field **`wXFx`** (= `xFXw` reversed) at
    **bit offset 45, width 7**, record 12 bytes / 95 bits → exactly **1 pad
    bit**, so +1 fits with no stride growth / table relocation.
- **Deployed to the Vanilla `_compiled` tree** (reversible — see Restore):
  - `db\nhlng.db` → widened copy (every player's stick = 200). Original backed
    up to `db\nhlng.db.bak_stickexp`.
  - staged `rendering\playerstick\texlib_200.rx2` (copy of `texlib_72.rx2`) and
    `fe\ion\artassets\createplayer\sticks\stick200.big` (copy of `stick72.big`)
    so a 200-request *resolves* and is logged. The masked targets
    `texlib_72.rx2` / `stick72.big` already exist.

> The experiment runs on the **Vanilla** root because the asset-open diagnostic
> (`NHL_LOG_ASSET_OPENS`, in `LooseTreeEntry::Open`) only fires for files served
> from the `_compiled` loose overlay. The Modded/plain roots serve loose at the
> root via `game:` and would not log.

## Run it

1. Launch the game against the Vanilla root with the asset-open log on, e.g.:
   ```powershell
   $env:NHL_LOG_ASSET_OPENS = "1"
   & "E:\Repositories\nhl-legacy-recomp\out\build\<your-build>\nhllegacy.exe" `
       --game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"
   ```
   (or edit `tools/drive.ps1`, which already points at the Vanilla root, and set
   the env var before it launches).
2. The log lands in `out\build\<build>\logs\nhllegacy_NNN.log`. Grep it:
   ```powershell
   Select-String -Path "out\build\<build>\logs\nhllegacy_*.log" `
       -Pattern "texlib_200|texlib_72|stick200|stick72"
   ```

### Surfaces to try (in order of signal cleanliness)

Every player now has stick = 200, so any surface that loads a player's stick
should request the 200-or-72 asset.

1. **Edit/Create Player → a roster player → equipment → stick.** The picker and
   the 3D preview load the *equipped* stick on demand for one player — the least
   ambiguous trigger.
2. **Start an Exhibition game** and watch a player's stick on the ice.
3. Any roster/line screen that previews equipment.

## Reading the result (decision tree)

- **`stick200.big` or `texlib_200.rx2` appears** → the read preserved 200 →
  **generic 8-bit read confirmed, Gate B is FREE.** This is the decisive
  *positive*: index 200 cannot appear unless the full value survived.
- **Only `…_72` appears in a per-player context (never 200)** → the value was
  masked to 7 bits → **Gate B needs a recomp patch.** Caveat: the in-game
  renderer may bulk-preload `texlib_18..112` at boot, so a `texlib_72` hit *at
  startup* is not the masked read. Judge by what is requested **when you open
  the stick surface**, and prefer the thumbnail signal (`stick72.big` is **not**
  in the picker's normal 23-set, so its appearance while viewing the 200-player
  is a real masked-read tell).
- **Neither 200 nor 72 appears for sticks** → inconclusive (the stick asset may
  be served from `.big` rather than `_compiled`, or opened via `Entry::GetChild`
  on a cached dir handle rather than the logged `Open`). Escalation (plan B):
  add a `GetChild`-miss logger in `LooseTreeEntry` so the exact requested leaf
  is captured hit-or-miss, regardless of staging. Not built yet — only needed if
  this run is inconclusive.

## Restore (undo everything)

```powershell
& "E:\Repositories\nhl-legacy-recomp\tools\stick_widen\restore.ps1"
```
Restores `nhlng.db` from the backup and removes the two staged assets.
