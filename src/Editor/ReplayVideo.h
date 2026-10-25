#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#include <Editor/ReplaySync.h>

struct SDL_Process;

namespace Vortex {

class Sound;
struct ReplayFile;

/// What finding the song in a recording needs from the replay and the song,
/// gathered beforehand so the search can run on another thread.
struct VideoSyncInput {
    std::vector<float> song;  ///< Onset envelope of the song at the rate.
    std::vector<SongPart> parts;
    double rate = 1.0;
    double hint = 0.0;         ///< Expected offset, NaN if unknown.
    double songSeconds = 0.0;  ///< Recording seconds the song takes to play.
};

/// The hop of the onset envelopes used to find songs in recordings.
const double kVideoSyncHop = 0.005;

/// Prepares finding the song of a replay in a recording. The preview of the
/// song is left out of the parts to look for, the replay date and the time
/// in the recording's file name give the expected offset.
VideoSyncInput PrepareVideoSync(const ReplayFile& file, const Sound& song,
                                double previewStart, double previewLength,
                                const fs::path& recording);

/// Finds where song time 0 is in the recording (see FindSongInRecording).
RecordingSync SyncVideo(const VideoSyncInput& input, const fs::path& recording,
                        const std::atomic<bool>* cancel = nullptr);

/// Reads the audio of a media file, e.g. an OBS recording, into an onset
/// envelope (see OnsetEnvelope) with the given hop in seconds. Only the part
/// from begin to end seconds is read, or all of it if end is negative.
/// start is set to the recording time of the first envelope value. Returns
/// false and sets error if the audio can not be read or cancel is set.
bool ReadRecordingEnvelope(const fs::path& path, double hop, double begin,
                           double end, std::vector<float>& envelope,
                           double& start, std::string& error,
                           const std::atomic<bool>* cancel = nullptr);

/// The local time in the file name of an OBS recording ("2026-10-22
/// 20-58-30.mkv"), as seconds since 1970 in that time zone, or a negative
/// value if there is none.
double RecordingStartTime(const fs::path& path);

/// Seconds since 1970 of a "2026-10-22 21:03:07" date, or a negative value.
double ParseLocalDate(const std::string& date);

/// A video shown in mpv, which follows the replay over its IPC.
class VideoPlayer {
   public:
    ~VideoPlayer();

    /// Starts mpv with the video, paused and without sound (the editor plays
    /// the music). extraArgs are added to its command line, e.g.
    /// "--vf=crop=640:480:1280:0" to show the camera only.
    bool open(const std::string& mpvPath, const std::string& extraArgs,
              const fs::path& video, std::string& error);

    void close();
    bool isOpen() const { return myProcess != nullptr; }

    /// Shows the video at the given time, paused or playing at the given
    /// speed. Called every frame, it only talks to mpv when needed.
    void show(double time, bool playing, double speed);

   private:
    bool connect();
    bool send(const std::string& command);
    void drain();
    void disconnect();

    SDL_Process* myProcess = nullptr;
    std::string myPipeName;
#ifdef _WIN32
    void* myPipe = nullptr;
#else
    int mySocket = -1;
#endif
    std::string myInput;  ///< What mpv sent, up to an unfinished line.
    bool myLoaded = false;
    bool myPlaying = false;
    double myTime = -1e9;
    double mySpeed = 1.0;
    uint64_t myLastSeekTicks = 0;
    uint64_t myPlayStartTicks = 0;
    double myPlayStartTime = 0.0;
};

};  // namespace Vortex
