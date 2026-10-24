#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Vortex {

/// Newest replay format this viewer reads.
const int kReplayVersion = 1;

/// One line of the events section of a replay file.
struct ReplayEvent {
    char type = 0;      ///< P, T, E, H or M (see the recorder module).
    double time = 0.0;  ///< Engine music seconds.
    int col = 0;        ///< Zero-based column.
    bool down = false;
    std::string score;  ///< TapNoteScore or HoldNoteScore, without prefix.
    double offset = 0.0;
    std::string earlyScore;
    double earlyOffset = 0.0;
    bool heldMiss = false;
    std::string noteType;
};

struct ReplayFile {
    std::map<std::string, std::string> header;
    std::vector<ReplayEvent> events;
    double endTime = 0.0;
    bool failed = false;
    std::string nextScreen;  ///< Screen that followed gameplay.

    int version = 0;
    double rate = 1.0;
    double globalOffset = 0.0;
    /// The sync correction ITGmania applied to the song's pack: -0.009 for
    /// ITG synced packs, 0 for null synced ones. Not in older replays.
    double syncOffset = 0.0;
    bool hasSyncOffset = false;
    int columns = 0;
    bool faPlusMode = false;
    bool autoplay = false;

    /// Window sizes in seconds, as used by the engine (W0 is the FA+ window).
    double window[6] = {0.015, 0.023, 0.0445, 0.1035, 0.1365, 0.1815};
    double holdWindow = 0.3215;
    double rollWindow = 0.3515;
    double mineWindow = 0.0715;

    std::string get(const char* key) const {
        auto it = header.find(key);
        return it == header.end() ? std::string() : it->second;
    }
};

/// "TapNoteScore_W1" -> "W1".
std::string StripEnumPrefix(const std::string& s);

/// Reads a replay file. Returns false and sets error if it can not be used.
bool ParseReplay(const fs::path& path, ReplayFile& out, std::string& error);

};  // namespace Vortex
