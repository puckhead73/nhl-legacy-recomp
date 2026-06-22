# vp6conv — NHL Legacy / EA VP6 movie converter

Convert any video (`.mp4`, `.mov`, `.avi`, `.mkv`, …) into an **EA-container VP6
`.vp6` movie** that NHL Legacy — and other EA Sports titles that use the same
`MVhd` container (NHL 07–09, FIFA, NFS, …) — can play.

You run this yourself on your source clips; the resulting `.vp6` files are what
get swapped into the game (e.g. bundled by the recomp builder).

## Quick start

```
tools\vp6-converter\vp6conv.exe  myclip.mp4  -o  out.vp6
```

To replace an existing movie **and keep its original soundtrack**, point at the
movie you're replacing with `--match`:

```
vp6conv.exe  myclip.mp4  -o  attract_eng.vp6  --match  "...\fe\movies\ntsc\attract_eng.vp6"
```

`--match` copies the target's width/height/frame-rate **and** reuses its audio
track byte-for-byte (see *Audio* below). Without flags the output defaults to
NHL Legacy's **1280×720 @ 29.97, vp60**, silent.

## Options

| Option | Meaning |
|---|---|
| `-o, --output <path>` | Output `.vp6` (required). |
| `--match <ref.vp6>` | Match a target movie's size/fps and reuse its audio. |
| `--width / --height <px>` | Override size (rounded down to a multiple of 16 — VP6 requires it). |
| `--fps <num/den>` | Override frame rate, e.g. `30000/1001` or `25/1`. |
| `--version <vp60\|vp61\|vp62>` | VP6 variant. **NHL Legacy uses `vp60`** (default). |
| `--quant <0-63>` | Fixed quantiser; lower = better quality / bigger file (default: auto). |
| `--key-int <n>` | Keyframe interval in frames (default: auto). |
| `--no-audio` | Force a silent file even with `--match`. |
| `--ffmpeg / --encoder <path>` | Override the bundled tool locations. |
| `--keep-temp` | Keep the intermediate `.y4m` / video-only files. |

## Audio

The Xbox 360 EA audio stream (`SCHl`/`SCDl` … "SimpleX") uses a codec **no open
tool can re-encode** — ffmpeg can't even parse it. So vp6conv never re-encodes
audio. Your choices are:

- **`--match <ref.vp6>`** — graft the *original* movie's audio onto your new
  video, byte-for-byte (verified identical). Best when you're re-skinning a clip
  but are happy to keep its existing sound. If your video is a different length,
  frames are mapped 1:1 and the last frame is held / surplus dropped so the audio
  stays intact and in sync.
- **default (silent)** — no audio track. Fine for clips that don't need sound.

Custom *source* audio (your clip's own track) is not yet supported because it
would require an EA 360 audio encoder.

## How it works

1. **ffmpeg** decodes/scales/re-times your source to a raw `Y4M` (yuv420p,
   dimensions forced to a multiple of 16).
2. **nihav-encoder** ([NihAV](https://nihav.org), the only open-source VP6
   *encoder*) encodes that to a VP6 stream in the EA container
   (`MVhd` + `MV0K`/`MV0F` frames). Video only.
3. vp6conv **splices** the reference's audio chunks (`SCHl`/`SCCl`/`SCDl`/`SCEl`)
   back in at the container level when `--match` is used.

ffmpeg only *decodes* VP6, so the actual VP6 encode is done by NihAV; vp6conv
orchestrates the two and handles the EA container itself.

## Tools / building

The bundled helpers live in `bin/` next to `vp6conv.exe` (git-ignored — they're
large / third-party):

- `bin/ffmpeg.exe` — any recent ffmpeg build.
- `bin/nihav-encoder.exe` — built from NihAV (`nihav` + `nihav-encoder`,
  `cargo build --release`; the default features already include the VP6 encoder
  and the EA muxer).

Rebuild vp6conv itself with `cargo build --release` (no dependencies); the binary
locates `ffmpeg.exe`/`nihav-encoder.exe` in `bin/` next to it, then on `PATH`.

## Licensing

- **vp6conv** (this tool): MIT.
- **NihAV**: AGPLv3 (relicensing offered on request by upstream). Distributing
  `nihav-encoder.exe` carries the AGPL source-offer obligation; source is public
  at <https://nihav.org>.
- **ffmpeg**: LGPL/GPL depending on the build.
