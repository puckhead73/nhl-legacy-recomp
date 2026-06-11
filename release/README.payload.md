# NHL Legacy Recomp

A native PC port of NHL Legacy (Xbox 360), produced by static recompilation.
This package does **not** include any game assets - you must provide your own
legally dumped copy of the game.

## Requirements

- Windows 10/11, 64-bit
- A Direct3D 12 capable GPU
- ~10 GB free disk space for the install
- Your own NHL Legacy disc dump: either a raw `.iso` of the disc, or an
  already-extracted game folder containing `default.xex`

## Quick start

1. Extract this zip anywhere (keep the files together - the builder needs the
   `payload` folder next to it).
2. Double-click `nhl-legacy-builder.exe` and follow the prompts, **or** use a
   terminal:

   ```
   nhl-legacy-builder install --iso "C:\dumps\NHL Legacy.iso" --out "C:\Games\NHL Legacy"
   ```

   With an already-extracted folder instead of an ISO:

   ```
   nhl-legacy-builder install --from "C:\dumps\NHL Legacy" --out "C:\Games\NHL Legacy"
   ```

3. When it finishes, launch `nhllegacy.exe` from the install folder. No
   arguments or configuration needed.

To check a dump without installing (a few seconds instead of a full extract):

```
nhl-legacy-builder verify --iso "C:\dumps\NHL Legacy.iso"
```

## Troubleshooting

- **"default.xex does not match the supported build"** - the port is
  recompiled from one specific vanilla image of the game. A different region,
  a modified dump, or a dump with a title update applied will not pass
  validation.
- **"not a recognized Xbox 360 disc image"** - the file is not a raw XDVDFS
  dump. Re-rip the disc to a plain `.iso`.
- **Install interrupted** - just re-run the same install command; it will
  redo the extraction.

## Legal

You must own NHL Legacy and dump your own copy. This project is not
affiliated with or endorsed by EA or Microsoft. See THIRD-PARTY-NOTICES.txt
for bundled component licenses.
