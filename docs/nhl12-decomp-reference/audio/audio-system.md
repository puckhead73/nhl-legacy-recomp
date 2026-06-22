# Audio System

The audio engine is **RenderWare Audio Core** (`rw::audio::core`), backed by
**`basekit/audio/rwaudiocore 6.02`**. Namespaces/versions CONFIRMED `[RTTI][P4]`;
data formats CONFIRMED `[RT]`; mechanism INFERRED.

## 1. Engine: `rw::audio::core` (CONFIRMED symbols `[RTTI]`)
Recovered RTTI fragments reveal the runtime structure:
- **`System`** — the audio system (`IsCommandComplete`, `GetTime`) — a command-driven
  mixer (you submit commands; it reports completion).
- **`Voice`** — a playing instance (`IsExpelled`, `Release`, `GetDecayTime`) — voices
  are pooled and can be **expelled** (stolen) under voice-count pressure.
- **`Pan` / `HwPan`** — panning (software + hardware-assisted).
- **`StreamPool`** — streaming-audio buffer pool (for long assets: music, commentary).
- **`SamplePlayer`** — one-shot sample playback (SFX).

This is a classic **voice-pool + streaming** console audio engine.

## 2. Codecs (CONFIRMED)
- **XMA** (Xbox 360 hardware codec) — reached via the **17 `XMA*` kernel imports**
  `[IMP]`, overridden by RexGlue (FFmpeg-backed), not recompiled. Used for music and
  likely streamed commentary. The recompiled build's XMA worker **busy-spins on
  register `0601`** when its music streams are missing — a *content* gap (empty
  `cache:\audio\music`), not a code bug `[RT]`.
- **Speex** — `rwaudiocore/decoders/speex/libspeex/nb_celp.cpp` `[P4]` — CPU-decoded
  (narrowband CELP). Voice-grade; likely in-game voice/online VoIP and/or compact
  speech. Runs as recompiled code.

## 3. Audio data in `cache:\audio\` (CONFIRMED paths `[RT]`)
| Path | What |
|---|---|
| `audio\music\*.{csi,sbr,xml}` | Music banks/cues (`.sbr` = sound bank, `.csi` = cue script, `.xml` = manifest). |
| `audio\tuning\mixer_*` | **Mixer tuning** — bus levels, ducking, dynamic mix states. |
| `audio\reverb\*.irf` | **Reverb impulse responses** (convolution reverb — arena acoustics). |
| `audio\NA_En_loadfile.xml` | Locale/region audio manifest (North America, English). |

`nocache.big` (2.45 GB) is the inferred home of **commentary** (large, streamed,
play-by-play + color). `[INF]`

## 4. The audio categories (INFERRED for hockey)
- **Commentary** — play-by-play + color (the largest, most stateful; driven by game
  events from the sim, sequenced by the `storytelling` layer `[P4]`).
- **Crowd** — reactive ambience (cheers on goals/hits, boos, chants) — state-machine
  driven by score/momentum.
- **On-ice SFX** — skates, stick/puck contact, boards, glass, posts, goal horn.
- **Music** — menu + stinger music (XMA streams).
- **PA / arena** — goal announcements, horn.

These map onto **mixer states** (`audio\tuning\mixer_*`) that duck/blend buses
(e.g. crowd swells under commentary on a goal). CONFIRMED tuning data; specific states
UNKNOWN until unpacked.

## 5. Trigger flow (INFERRED)
```
sim event (goal, hit, save, whistle)  [nhlgameplay]
      │
 storytelling/presentation sequences the beat (commentary/crowd)  [P4 storytelling]
      │
 rw::audio::core::System  ── submit voice/stream commands ──▶ mixer
      │            (Voice pool; expel under pressure; Pan)
 mixer state (audio/tuning/mixer_*)  ── ducking/blend ──▶ output
      │
 XMA / Speex decode  ──▶  XAudio/host mixer (6 XAudio imports, overridden)
```

## 6. Recomp notes (CONFIRMED relevance)
- XMA + the 6 `XAudio*` imports are **overrides** (host mixer). `[IMP]`
- Speex decodes in recompiled code — a good correctness check (CPU codec, deterministic
  output to compare against Xenia).
- Audio threading: the **XMA audio worker** thread (see
  [`../architecture/threading.md`](../architecture/threading.md)) must be fed or it
  busy-spins.

## Open questions
- `.sbr`/`.csi` bank/cue formats (needs `.big` unpack).
- Commentary location + event→line mapping.
- Mixer state list; reverb per-arena selection.
- Exact use of Speex (VoIP vs. in-game speech).

See [`../assets/asset-pipeline.md`](../assets/asset-pipeline.md) and
[`../unknowns/open-questions.md`](../unknowns/open-questions.md).
