# Audio: music moods and ambience

The engine supplies the machinery: buses, zones, music moods, ambient layers and
parameters. The game decides what is happening, such as a fight starting or the
player reaching the camp, and supplies the sounds.

## Buses
Every sound plays through a named bus (`SFX`, `Ambient`, `Weather`, `Music`,
`UI`, or any new name). Each bus has a volume and a low-pass filter.
`AudioZone`s change the buses they list while the listener is inside. Savanna's
huts muffle `Weather` and `Ambient`, but not `Music`.

## Music (`ve/audio/Music.hpp`)
A **mood** is a playlist: tracks, shuffle, a random silent gap between tracks,
a volume and a crossfade time. One mood plays at a time:
- **The base mood:** `setMood("Explore")`, or a `MusicMood` with `playOnStart`.
- **Pushed moods override the base:** `pushMood("Combat", 10)` /
  `popMood("Combat")`.
  - The highest priority wins; on ties, the latest push wins.
  - Pushes are counted, so two zones plus a fight don't fight each other.
  - Popping crossfades back to the mood underneath. If that mood's track
    stopped less than about 20 s ago, it resumes where it left off.
- **Stingers:** `stinger("audio/victory.ogg")` plays over the music, which
  ducks under it.

```cpp
// Game code: a fight.
void onCombatStarted() { ve::Engine::get().audio().music().pushMood("Combat", 10, 1.0f); }
void onCombatEnded()   { ve::Engine::get().audio().music().popMood("Combat", 4.0f); }
```

In scenes:
- **`MusicMood` component:** the object's name is the mood name.
  `tracks = "audio/a.ogg; audio/b.ogg"`.
- **`MusicZone` component:** pushes `mood` while the listener is within
  `radius`. With `height > 0` the zone is a vertical cylinder. `exitMargin`
  stops it flapping at the edge.

Tracks are streamed from disk. The debug panel (F1, Audio) shows the current
mood and pushes, and has buttons to switch mood or skip a track.

## Ambience (`ve/audio/Ambience.hpp`)
An `AmbientLayer` is a looping bed and/or random one-shots around the listener.
Its level is `time × biome × parameters`, smoothed over `fadeTime`:

| Field | Meaning |
|---|---|
| `fromHour`, `toHour`, `hourFade` | Plays between these hours (wrapping past midnight), fading over `hourFade` hours. Uses the `timeOfDay` parameter. |
| `biomes`, `biomeRadius` | The Terrain's biome weights around the listener, with the same syntax as vegetation: `"Savannah:1, Swamp:-1"`. |
| `parameters` | `"rain:-1, wind:-0.5"` gives `1 + Σ weight × value`. `"*:0, danger:1"` plays only when `danger` is set. |
| `loop` | A 2D streamed loop at `volume × level`. |
| `oneShots`, `interval`, `distance` | Random clips placed `distance` m from the listener (3D). Lower levels make them rarer, not just quieter. |

Layers play on the `Ambient` bus by default, so zones muffle them indoors.

## Parameters
`audio().setParameter(name, value)` / `parameter(name)`: named floats.
- **Effects module:** `DayNightCycle` sets `timeOfDay` (hours) and `night`
  (0..1). `Weather` sets `rain`, `storm` and `wind` (0..1).
- **Game code:** sets its own, e.g. `danger`.

You can view and edit them in the debug panel.

## Savanna
- **Music:**
  - Moods `Explore` (base), `Camp` (a `MusicZone` 20 m around the campfire)
    and `Combat`.
  - Press M to start or stop a stand-in fight.
  - `Combat` reuses `ambient_01.wav` until there's a real fight track.
- **Ambience:**
  - Crickets at night.
  - Frogs in the swamp from dusk to morning.
  - Daytime insects.
  - Birds at dawn and during the day (quiet in the rain).
  - Distant hyenas at night.
- **Placeholder sounds:** these are synthesized by
  `tools/generate_ambience.py` into `assets/audio/ambience/`. Replace the files,
  or point the layers at other files in the editor.
- **Autotest:** logs the mood and every layer's level at the camp, at the far
  corner, and in a hut at 22:00 with combat pushed.
