# ArrowReplay

A replay viewer for ITGmania, built on ArrowVortex. Play a song, then scroll
through the chart and see exactly what you did: every panel press, the timing
of every note, misses, presses that did not hit anything, dropped holds and hit
mines.

It has two parts:

- **`ArrowReplay.lua`**: a Simply Love module that records your presses and the
  engine's judgments while you play and saves them as an `.avreplay` file.
- **ArrowVortex with a Replay menu** (this fork) that opens those files and
  draws them on the notefield.

## Recording

1. Copy `ArrowReplay.lua` into the `Modules` folder of your Simply Love theme,
   e.g. `ITGmania/Themes/Simply Love/Modules/`. No other theme files change.
2. Restart ITGmania.

After every song (also when you restart or quit it) a replay is written to
`Save/ArrowReplays/YYYYMMDD-HHMMSS_P1_Title.avreplay` in your ITGmania data
folder (`%APPDATA%\ITGmania` on a regular Windows install, the game folder on a
portable one). Both players are recorded, courses get one file per song.

Requires ITGmania and Simply Love (upstream or a fork) with module support.

## Viewing

Open a replay with **Replay > Open replay...** (`Ctrl+Shift+O`), File > Open,
by dropping it on the window, or by passing it on the command line.
ArrowVortex opens the simfile and the chart that was played and sets the music
speed to the rate you played at.

The replay stores the song's ITGmania path (`/Songs/Pack/Song/...`). ArrowVortex
looks for it in the ITGmania folder the replay is in, in
`%APPDATA%\ITGmania\Songs` and in `C:\Games\ITGmania\Songs`. If it is somewhere
else, you are asked to pick the simfile once and its `Songs` folder is
remembered.

### On the notefield

| Mark | Meaning |
| --- | --- |
| Colored bar with a line to the note | The press that hit the note, in the judgment color (blue FA+, white Fantastic, gold Excellent, green Great, purple Decent, brown Way Off) |
| Shorter, faded bar | An early decent/way off that a later press rescored |
| Thick pink bar, `stray` | A press that did not activate anything |
| Gray bar | A press during a hold or roll |
| Red box, `MISS` | A miss (`MISS (held)` if the panel was held down) |
| Red bar, `LET GO` | Where the foot left a dropped hold |
| Red box on a mine, `MINE` | A hit mine |
| Light strips behind the notes | How long each panel was held down |

To the right of the notefield is each note's offset in milliseconds (negative
is early). Jumps show one value per arrow.

### Summary and timeline

The panel on the left counts judgments, holds, hit mines and stray taps and
shows the mean offset and its standard deviation.

The timeline next to the minimap covers the whole chart at the minimap's scale:

- each note is a dot at its offset, early on the left and late on the right,
  across the widest timing window;
- thin vertical lines are window edges, the white trail is the mean of the last
  24 hits, so drifting early or late is easy to spot;
- red lines are misses, orange lines dropped holds, short red marks on the right
  hit mines and pink marks on the left stray taps;
- the yellow box is the part of the chart on screen. Click or drag to jump.

`F7` / `F8` jump to the previous / next problem (miss, decent, way off, stray
tap, dropped hold, hit mine). Every layer can be turned off in the Replay menu,
and all actions can be rebound in `settings/shortcuts.txt`.

## How it works

ITGmania sends every judgment to Lua with the exact offset the engine used, and
the input callback receives every press with its timestamp. For each judgment
the viewer finds the note and the press that line up with
`note time + offset * rate`, so hits are drawn exactly where the engine judged
them. On real recordings the recorded press time matches the engine's to well
under a millisecond. Presses without a judgment are stray taps. Times are
converted to chart time with the global offset and the music rate.

### File format

UTF-8 text. A header of `Key<TAB>Value` lines (song path, steps type,
difficulty, rate, global offset, timing windows, ...), a `---` line, then one
event per line, all tab separated, times in engine music seconds and columns
starting at 1:

```
P  time  col  down                  press (1) or release (0)
T  time  col  tns  offset  earlyTns  earlyOffset  heldMiss  noteType
E  time  col  tns  offset           early hit that may get rescored
H  time  col  hns                   hold result
M  time  col  tns                   hit mine
END  time  failed  nextScreen
```

## Building

See [BUILDING.md](../BUILDING.md). The viewer is `src/Editor/Replay.cpp`.

Tests:

- `tools/test_recorder.py ../ArrowReplay.lua` runs the module against a mocked
  ITGmania Lua API (`pip install lupa`).
- `tools/make_test_replay.py` writes a synthetic replay for any `.sm` chart.

## Limitations

- Doubles and courses are supported but have not been tested yet.
- The viewer does not check that the chart matches the one the replay was
  recorded on. A replay played on a different version of a simfile will be
  drawn wrong.
- When the `HOME` environment variable is set (e.g. starting from Git Bash),
  ArrowVortex keeps its settings in `$HOME/.config/arrowvortex/`. That is how
  upstream ArrowVortex behaves.
