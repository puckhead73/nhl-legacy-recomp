# NHL12 Equipment DB Color Notes

Date: 2026-06-15

This note documents the offline DB-color path used by `tools\nhl12_texture_proof.py`.

## Source Files

```text
extracted\cache_hdd\db\nhlng-meta.xml
extracted\cache_hdd\db\nhlng.db
```

`nhlng-meta.xml` describes `exhibitiongoalieequipment`, whose table shortname is `lVMf`.
The table contains 104 integer fields and 789 bits per row, so each fixed row occupies 99 bytes.
In `nhlng.db`, the `lVMf` directory entry points to offset `0x192F0`; the next table starts at
`0x279D0`, giving 597 decoded rows plus padding.

## Color Fields

The table contains DB-driven color zones for:

```text
padszone1color_r/g/b      ... padszone9color_r/g/b
blockerzone1color_r/g/b   ... blockerzone9color_r/g/b
trapperzone1color_r/g/b   ... trapperzone9color_r/g/b
stickzone1color_r/g/b     ... stickzone3color_r/g/b
```

The parser currently supports explicit `--db-bit-order msb` or `--db-bit-order lsb`. The current
proof commands use `msb`, matching the big-endian nature of the file and producing plausible in-range
equipment IDs and colors. Keep the bit order in every report until a runtime row lookup confirms it.

## Proof Commands

Representative DB-colored material proof:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --material-set goaliepad:0_11 --material-set glove:0_15 --out build\texture_material_db_colors --material-size 256 --no-images --db-file extracted\cache_hdd\db\nhlng.db --db-meta extracted\cache_hdd\db\nhlng-meta.xml --db-bit-order msb
```

Latest result:

```text
goaliepad:0_11 -> source=db, kind=pads, id=11, matched=1
glove:0_15     -> source=db, kind=trapper, id=15, matched=3
0 failures
```

Broader DB-colored discovery sample:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --discover-material-dir goaliepad --discover-material-dir glove --discover-material-limit 40 --out build\texture_material_db_colors_discovery --material-size 64 --no-images --db-file extracted\cache_hdd\db\nhlng.db --db-meta extracted\cache_hdd\db\nhlng-meta.xml --db-bit-order msb
```

Latest result:

```text
40 material stacks tested
27 exact DB equipment-color matches
13 DB global color fallbacks
0 synthetic fallbacks
0 failures
160 composite metrics
```

Some DB rows contain legitimate saturated green or purple equipment colors. The artifact metric
therefore treats saturated green/purple as a failure only when the selected DB tint palette does not
itself contain that color family.

Current goalie-equipment sample, including the user-visible blocker/trapper/pad problem families:

```powershell
python tools\nhl12_texture_proof.py --root extracted\cache_hdd\rendering --discover-material-dir goaliepad --discover-material-dir glove --discover-material-dir blocker --discover-material-dir trapper --discover-material-limit 40 --out build\texture_material_goalie_equipment_db_sample --material-size 64 --no-images --db-file extracted\cache_hdd\db\nhlng.db --db-meta extracted\cache_hdd\db\nhlng-meta.xml --db-bit-order msb
```

Latest result:

```text
40 material stacks tested
0 failures
6 exact pads DB matches
15 exact trapper/glove DB matches
9 exact blocker DB matches
10 DB global color fallbacks
```

The proof tool now interleaves limited discovery across requested directories, so a small sample
cannot accidentally test only `goaliepad` while skipping `blocker` and `trapper`.

## Remaining Caveat

This proves the static DB-color/material-sheet path offline. It still does not prove that the live
game binds the same DB rows and shader constants during normal gameplay. Use the runtime
`[NHL-TEX]` / `[NHL-SAMPLER]` log analyzer with the extracted texture catalog for that:

```powershell
python tools\nhl12_texture_proof.py --analyze-log build\nhl12_normalplay_equipment_sampler.log --catalog-report build\texture_proof_goalie_equipment_renderer_key\report.json --out build\nhl12_normalplay_equipment_sampler_analysis
```

Strict catalog matches prove the live texture binding agrees with extracted RX2 format, dimensions,
mip count, packed state, tiled state, and pitch. DB-generated recolor/material inputs may still be
unmatched; treat those as the next target for shader-constant or DB-row logging rather than as an
automatic RX2 decode failure.
