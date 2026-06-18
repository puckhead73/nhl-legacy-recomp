# NHL Legacy Recomp

A native **PC port of NHL Legacy** (Xbox 360), produced by static recompilation —
the original game's code is translated into a native Windows executable via RexGlue rather than
emulated, so it runs as a real PC application with modern rendering on top.

> ⚠️ **You must provide your own legally dumped copy of the game.** This project
> ships **no game content** of any kind. **Piracy is not condoned** — do not
> ask for, share, or use illegally obtained game files. This project is not
> affiliated with or endorsed by EA or Microsoft.

## What you get

- **Native performance** — runs as a real Windows program (no emulator), at
  real-time framerates well above the original console.
- **Modern Vulkan renderer** with **AMD FidelityFX** (FSR upscaling / CAS
  sharpening), optional **supersampling**, and a live **color-grade** pass
  (exposure / contrast / saturation / white balance / tone mapping).
- **In-game overlay** — a settings panel, performance HUD, and display options
  (fullscreen / borderless, monitor select), toggled with a button on screen.
- **Loose-file friendly** — the installer unpacks the game's archives into a
  browsable file tree, so the community can inspect and replace assets.

## Requirements

- Windows 10 / 11, 64-bit
- A **Vulkan-capable GPU**
- The [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)
- ~10 GB free disk space
- **Your own dumped copy of NHL Legacy** — either a raw `.iso` of your disc, or an
  already-extracted game folder containing `default.xex`

## Getting started

1. Download the latest release zip from the
   [**Releases**](https://github.com/puckhead73/nhl-legacy-recomp/releases) page and
   extract it anywhere (keep the files together).
2. Run the builder with your own dump — either double-click
   `nhl-legacy-builder.exe` for prompts, or from a terminal:

   ```
   nhl-legacy-builder install --iso "C:\dumps\NHL Legacy.iso" --out "C:\Games\NHL Legacy"
   ```

   (Use `--from "C:\path\to\extracted folder"` instead of `--iso` if you already
   have an extracted game folder.)
3. Launch `nhllegacy.exe` from the output folder. No arguments needed.

To check that a dump is supported without doing a full install:

```
nhl-legacy-builder verify --iso "C:\dumps\NHL Legacy.iso"
```

The build is recompiled from one specific vanilla image of the game, so the builder
verifies your `default.xex` by hash. A different region, a modified dump, or one with
a title update applied will not pass validation.

## Legal

You must own NHL Legacy and dump your own copy. No game assets are included or
distributed with this project, and none will be provided. This is a fan
preservation/portability effort, not affiliated with or endorsed by EA or Microsoft.
See `THIRD-PARTY-NOTICES.txt` in the release for bundled component licenses.

## For developers

Building the port from source, the recompilation pipeline, the release process, and
architecture notes live in **[DEV-README.md](DEV-README.md)** and
**[docs/current-status.md](docs/current-status.md)**.
