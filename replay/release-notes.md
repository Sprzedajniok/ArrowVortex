**Download** `ArrowReplay-v0.3.0-win64.zip` below: ArrowVortex with the Replay menu, the Simply Love module and an example replay. Needs the [Visual C++ 2015-2022 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).

The Simply Love module is the same as in v0.2.1.

**Changes since v0.2.1**

- **Video of the play (experimental):** if you record your sessions with OBS, **Replay > Open video of the play...** shows the recording next to the replay, following the cursor, in [mpv](https://mpv.io/) (on the `PATH`, or set `mpvPath` in the settings). The play is found from the song in the recording's audio, so a whole evening of recording works: the song preview looping in the song select menu and restarts are skipped, and OBS's file names (`2026-10-22 19-58-30.mkv`) pick the right play when a song was played twice. The video opens again with the replay.
  - `Shift+F7` / `Shift+F8` show the video 10 ms earlier / later.
  - Press `c` in the mpv window and drag over the camera or the pad pressure values to show only that part (again to zoom in further, `Shift+C` for the whole video). It is kept for the next videos.
- Fixed a crash at startup when the replay settings had an empty list of song folders or song files, which ArrowVortex saves after a session in which it did not have to look for a simfile (v0.2.0 and v0.2.1).

See the [README](https://github.com/Sprzedajniok/ArrowVortex/blob/arrowreplay-v0.3.0/README.md#video-of-the-play-experimental) for details.
