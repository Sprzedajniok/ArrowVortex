**Download** `ArrowReplay-v0.2.1-win64.zip` below: ArrowVortex with the Replay menu, the Simply Love module and an example replay. Needs the [Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).

**Update the module too:** copy the new `Simply Love module/ArrowReplay.lua` over the old one in the `Modules` folder of your theme and restart ITGmania.

**Changes since v0.2.0**

- Songs from ITG synced packs no longer show "Notes are 9 ms earlier than when played, sync changed?". ITGmania plays them 9 ms later than their simfiles say (`SyncOffset=ITG` in `Pack.ini`, or the DefaultSyncOffset preference, see [nine-or-null](https://github.com/telperion/nine-or-null)). The module now records that correction and ArrowVortex applies it.
- Replays recorded with an older module show "likely ITGmania's 9 ms ITG sync correction" instead of the sync warning.

See the [README](https://github.com/Sprzedajniok/ArrowVortex/blob/arrowreplay-v0.2.1/README.md#arrowvortex--arrowreplay) for details.
