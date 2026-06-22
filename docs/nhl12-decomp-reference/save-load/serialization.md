# Save / Load & Serialization

**Status: ⬜ mostly UNKNOWN.** No save flow has been exercised (the build stalls
pre-frontend). This doc records the confirmed building blocks and what to resolve.

## 1. Platform storage primitives (CONFIRMED `[IMP]`)
Saves on 360 go through **content packages**:
- `XamContent*` (6 imports) — content enumeration/create/open. Stubbed to "empty" for
  the recomp MVP (no saves). `[IMP]`
- `XamUser*` (11) — the signed-in profile (saves are per-profile). Stubbed to a fake
  signed-in user.
- `XeCrypt*`/`XeKeys*` (15) — EA **signs/verifies** content (rosters, saves, DLC).
  These must be correct or load silently fails. `[IMP]`

So the *transport* is known; the *payload formats* are not.

## 2. What gets saved (INFERRED from modes)
- **Profile / settings** — controller config, sliders, audio/video options
  (`options.viv` is the on-disc options *seed*; user options persist to a profile save).
- **Rosters** — team/player data (editable rosters; downloadable roster updates via
  OSDK). The on-disc roster DB is likely in `data0.big`. `[INF]`
- **Franchise / GM Mode** — the `leaguelogic/gmmodedata` model serialized (standings,
  schedule, contracts, trades). See [`../ui/frontend.md`](../ui/frontend.md).
- **Be A Pro** — single-player career progress (INFERRED; mode not yet confirmed by
  name).

## 3. Serialization style (INFERRED)
EA titles of this era serialize via:
- **Versioned binary blobs** (a header with a version int; the classic "version
  assumption" that breaks across patches). The recomp must preserve **big-endian**
  field layout exactly (see [`../architecture/memory.md`](../architecture/memory.md)).
- EA's **`.vlt` "vault"** hashed key→value store appears for `renddb` `[RT]` and may be
  reused for data-heavy saves.
- EASTL containers serialized field-by-field.

## 4. Recomp implications (CONFIRMED relevance)
- **Endianness:** save blobs are BE; a host that writes LE saves would be incompatible
  with the original and internally inconsistent. Keep BE.
- **Signing:** `XeCrypt*` must reproduce EA's expected hashes/signatures or loads are
  rejected. A known silent-failure trap. `[IMP]`
- **Out of MVP scope:** save/load of franchise data is explicitly out of the recomp
  MVP — content imports are stubbed empty.

## Open questions (all UNKNOWN)
- Profile/settings save format + version.
- Roster DB format (likely `data0.big`) and update mechanism.
- GM/franchise save schema (serialized `gmmodedata`).
- The `.vlt` vault format.
- EA's content signing scheme specifics (`XeCrypt*` expectations).

Resolving these requires reaching the frontend + a real save round-trip, then reading
the serializers. See [`../unknowns/open-questions.md`](../unknowns/open-questions.md)
and [`../assets/asset-pipeline.md`](../assets/asset-pipeline.md).
