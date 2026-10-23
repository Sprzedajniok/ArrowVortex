**Download** `ArrowReplay-v0.2.0-win64.zip` below: ArrowVortex with the Replay menu, the Simply Love module and an example replay. Needs the [Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).

**Update the module too:** copy the new `Simply Love module/ArrowReplay.lua` over the old one in the `Modules` folder of your theme and restart ITGmania.

**Changes since v0.1.1**

- "Record Replay" in the player options (second page, Gameplay Extras) turns recording on or off per player, the choice is saved in the profile
- Warning when the open simfile does not look like the one that was played (hits that do not line up, or a changed sync)
- Fixed early hits are labeled like `+68(-161)`
- When the played chart is not found, other charts can be opened, or the chart created, without the replay closing
- Closing a replay sets the music speed back
- Dropped rolls are marked at the right place, autoplay replays are marked, replays in a newer format warn
- Simfiles picked by hand no longer add unrelated folders to the song search
- The F7 / F8 hint shows the shortcuts that are actually bound, long charts are matched faster
- `ArrowVortex --replay-report REPLAY SIMFILE OUT` writes a replay summary without opening a window

See the [README](https://github.com/Sprzedajniok/ArrowVortex/blob/arrowreplay-v0.2.0/README.md#arrowvortex--arrowreplay) for details.
