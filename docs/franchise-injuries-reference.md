# NHL Legacy — injury system reference

Reverse-engineered from `default.decompressed.exe` (string/tweak analysis) and the
recomp. Code lives in `nhlgameplay/ai/injury.cpp` (in-game,
`C:\p4\NHL\Common\nhl\gameplay\...\ai\injury.cpp`) + `nhlfrontend\leaguelogic\
InjuryGen.cpp` (season/franchise generation, seed `sub_82B7BE58`) + `InjuryBrain.cpp`
(decisioning, seeds `sub_82B8AE70`, `sub_82B8EA38`).

## How many injuries

Injuries are **body-part type × severity tier**, with separate skater and goalie sets.

**Skater body parts (19):** Abs, Ankle, Arm, Back, Concussion, Elbow, Flu, Foot,
Groin, Hand, Hip, Jaw, Knee, Leg, Neck, Nose, Ribs, Shoulder, Wrist.

**Goalie body parts (15):** Ankle, Back, Collarbone, Concussion, Finger, Foot,
Groin, Hamstring, Hand, Hip, Knee, Shoulder, Thumb, Toe, Wrist.

(24 distinct body parts overall; 10 shared between skater/goalie.)

**Severity tiers (4):** `Game` (day-to-day, misses the current game), `Minor`,
`Major`, `Brutal` — confirmed by `gInjuryDistribution_{region}{Game|Minor|Major|
Brutal}`. The trainer-recommendation UI exposes Game/Minor/Major.

So the type space is ~ (19 skater + 15 goalie) body parts × 4 severities, with each
concrete injury also carrying a display string (`injurystring`).

## How long they last (duration model)

Duration is **generated from a distribution keyed by body-region × severity**, in
DAYS, then converted to games-missed:

- Distribution tweaks (category `HUT/Injury`): `gInjuryDistribution_{Torso|Arm|Leg}
  {Game|Minor|Major|Brutal}` — 3 regions × 4 severities = 12 buckets. (Static image
  default reads as 12 for all; the real per-bucket day ranges are resolved from data
  at mode load, not baked in the image — see "exact numbers" below.)
- Days→games conversion: `gMudInjuryDaysToGamesRate` / "Injury Days to Games Rate"
  (`MUD` = franchise/GM mode). Franchise tracks injuries in days; this rate maps days
  to games missed for scheduling.
- Display buckets (UI): Day-to-Day, Week(s), "in N games", and Long
  (`XSustainedAnInjuryDayToDay`, `XHasSustainedAnInjuryWeek`,
  `XHasSustainedAnInjuryInGames`, `...InjuryLong`). `InjuryLengthUnknown` exists for
  not-yet-evaluated.
- Runtime/save fields: `injuryOrigDays`/`injuryoriginallength`, `injuryDaysLeft`/
  `injurydayslefttoheal`, `dayssinceinjury`, `injuryseverity`, `injurytypepregame`,
  `mangameslost`, `gamesplayedhurt`, `wiNhlGamesMissedToInjury{Last,Current}Season`.
- In-PLAY temporary injuries (attribute debuffs during a game, not roster injuries):
  `gMinTempInjurySeconds` / `gMaxTempInjurySeconds`, `gInjuryRumbleDuration`,
  `gTempInjuryEffectFadeTime`.

## Frequency / occurrence tuning (all editable tweaks)

- `gTweakByGameStyleInjuryOccurrenceLevel` / `...CPU` (AI/Tweaks; difficulty-indexed,
  =13 in dump) — master occurrence rate. Also "Injury occurrence level" config.
- `gInjuryRateSlider` (AI/Injuries/AttributeMod) — the in-game injury slider.
- Fighting: `gFightingInjuryMinorChance`, `gFightingInjuryMajorChance`,
  `gMaxHealthForFacialInjury`, `gMaxOpponentHealthForHandInjury`.
- Shot-block: `gInjuryShotBlockPuckSpeedThreshold`,
  `gInjuryShotBlock{PuckHeightStanding,PuckHeightBlocking,Distance}Threshold`.
- Re-injury / situational: `gInjuryExistingInjMinIncrease`/`MaxIncrease`,
  `gIntimidationThresholdForIncreasedInjuryChance`,
  `gIntimidationIncreasedInjuryChance`, `gHitInjuryChanceMultiplier`.
- Goalie cover: `gInjuryDelay`, `gInjuryDelayLong`.

## Modding entry points

- **Frequency:** edit the AI/Injuries + AI/Tweaks injury tweaks via the engine-tunable
  overlay ([[engine-settings-editor]]) — these are in the captured 0x83Cxxxxx pool.
- **Durations:** `gInjuryDistribution_*` (HUT/Injury) + `gMudInjuryDaysToGamesRate`.
  Their live day-range values resolve from data at mode load, so the static defaults
  in the image are not the real ranges.
- **Exact per-type day numbers — TODO:** two ways to get them: (a) decode
  `InjuryGen.cpp` (`sub_82B7BE58` neighborhood) to read how it maps distribution +
  body part → days; (b) enable `gDumpGmInjuryTrackingData` (dumps CSV
  `team,position,injurytype,severity,injurystring,occurrence,gamesplayedhurt,
  mangameslost`) and observe a simmed season. Runtime is the faster authoritative path.
