# ArrowReplay internals

Installing and using ArrowReplay is described in the
[main README](../README.md#arrowvortex--arrowreplay). This page covers the
recorder, the replay file format and the tests.

## Files

- `ArrowReplay.lua`: the Simply Love module that records replays.
- `tests/example - Into You (SRPG10).avreplay`: a real replay of *[11] Into You
  (N-Vision Remix Edit)* (Challenge 11) from Stamina RPG 10 (SRPG10), to try the viewer
  without recording anything.
- `tests/test_recorder.py`: runs the module against a mocked ITGmania Lua API.
- `tests/make_test_replay.py`: writes a synthetic replay for a `.sm` chart.
- `tests/test_chart.sm`: a generated chart (taps, jumps, holds, rolls, mines,
  a BPM change and a stop) for the viewer test.
- `tests/check_report.py`: compares the viewer's replay report with the
  judgments `make_test_replay.py` generated.
- `screenshots/`: images used by the main README.
- The viewer itself is in `src/Editor/`: `Replay.cpp` (opening replays and
  keeping them on their chart), `ReplayFile.cpp` (reading the file),
  `ReplayLocate.cpp` (finding the simfile), `ReplayMatch.cpp` (matching the
  replay to the chart) and `ReplayDraw.cpp` (notefield marks, summary panel
  and timeline).

## Recorder

Simply Love loads every `.lua` file in its `Modules` folder and runs the
returned actors on the screens they are registered for. `ArrowReplay.lua`
registers one actor on `ScreenGameplay` that:

- adds an input callback and writes every first press and release of a
  gameplay button, at `music seconds - DeviceInput.ago * rate` (the same clock
  the engine uses to judge a step);
- listens to `Judgment` messages (final tap results, hold results, hit mines)
  and ITGmania's `EarlyHit` messages (early decents/way offs that may still be
  rescored);
- writes the file when the screen changes, or before the next song of a course.

Simply Love has no way for modules to add options, so the "Record Replay"
choice is added to its Gameplay Extras row (`GameplayExtrasB`) by wrapping the
theme's global `CustomOptionRow()`. The choice is saved per profile in
`ArrowReplay.ini` in the profile folder (guests keep it until the game is
closed). In a theme without `CustomOptionRow()` every play is recorded.

## File format

UTF-8 text. A header of `Key<TAB>Value` lines (song path, steps type,
difficulty, meter, rate, global offset, timing windows, date, ...), a `---`
line, then one event per line. Fields are tab separated, times are engine music
seconds (without the global offset), columns start at 1 and offsets are the
engine's `TapNoteOffset` in real seconds (negative is early).

```
P    time  col  down                  press (1) or release (0)
T    time  col  tns  offset  earlyTns  earlyOffset  heldMiss  noteType
E    time  col  tns  offset           early hit that may get rescored
H    time  col  hns                   hold result (Held, LetGo, Missed)
M    time  col  tns                   hit mine
END  time  failed  nextScreen         nextScreen is ScreenGameplay on a restart
```

## Matching

Chart time is `music seconds + rate * GlobalOffset + SyncOffset`, where
`SyncOffset` is the correction ITGmania applies to ITG synced packs (-0.009,
see `Pack.ini` and the DefaultSyncOffset preference) or 0 for null synced
ones. For every `T` record with a hit, the viewer looks for a press in the
same column (a release for lifts) and a not yet judged note such that
`press = note + offset * rate`, within 35 ms.
The median difference between those presses and the recorded ones is the
recorder's clock bias; matching then runs again with the bias removed and a
20 ms tolerance. On real recordings the difference is under a millisecond.

Misses go to the nearest unjudged note one timing window before the miss was
reported. Hold results go to the latest hold that started before them, and a
dropped hold is marked at the last release inside it. Presses that nothing
claims are stray taps, or hold re-presses when they fall inside a hold or roll.

## Tests

```sh
pip install lupa
python tests/test_recorder.py ArrowReplay.lua
python tests/make_test_replay.py SONG.sm "/Songs/Pack/Song/song.sm" out.avreplay --rate 1.5
```

`make_test_replay.py` prints the judgment counts it generated, which should
match the viewer's summary panel. Opening `tests/example - Into You (SRPG10).avreplay`
checks the viewer on a real recording.

The viewer can also write its summary without opening a window, which the CI
does after every Windows build:

```sh
python tests/make_test_replay.py tests/test_chart.sm "/Songs/Tests/Test/test_chart.sm" test.avreplay --truth truth.json
ArrowVortex --replay-report test.avreplay tests/test_chart.sm report.txt
python tests/check_report.py report.txt truth.json
```

## Releases

Put the release notes in `release-notes.md`, then run the ArrowReplay workflow
in the Actions tab ("Run workflow") with the version, e.g. `v0.2.0`. It builds
and tests ArrowVortex, creates the tag `arrowreplay-v0.2.0` and publishes
`ArrowReplay-v0.2.0-win64.zip` as a release. Pushing a tag `arrowreplay-v0.2.0`
does the same.

## Limitations

- Doubles and courses are supported but not tested yet.
- The viewer does not compare the chart with the `ChartHash` of the replay
  yet. It warns when fewer than 90% of the hits line up with a press or the
  notes are more than 5 ms off, which is what a different version of a
  simfile looks like, but smaller edits go unnoticed.
- When the `HOME` environment variable is set (e.g. when started from Git
  Bash), ArrowVortex keeps its settings in `$HOME/.config/arrowvortex/`. That
  is upstream ArrowVortex behavior.
