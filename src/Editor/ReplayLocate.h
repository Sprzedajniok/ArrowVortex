#pragma once

#include <string>
#include <utility>
#include <vector>

#include <Core/Xmr.h>

#include <Editor/ReplayFile.h>

namespace Vortex {

struct Chart;

/// How well a chart matches the chart of a replay: 0 if the steps type or
/// the difficulty differ, otherwise 1, plus 2 if the meter and 1 if the
/// description match too.
int ScoreReplayChart(const ReplayFile& file, const Chart& chart);

/// Finds the simfile a replay was played on, and remembers songs and the
/// folders that hold packs when they were found in another place than the
/// replay says.
struct ReplayLibrary {
    void load(XmrNode& node);
    void save(XmrNode& node) const;

    /// Finds the simfile the replay was played on, asking the user if needed.
    /// Sets useCurrent if it is the simfile that is open, and remember if the
    /// song was not where the replay says. Returns an empty path if the user
    /// cancelled.
    fs::path locateSimfile(const fs::path& replayPath, const ReplayFile& file,
                           bool& useCurrent, bool& remember);

    /// Remembers where the song of a replay was found.
    void rememberSongFile(const ReplayFile& file, const fs::path& simfile);

    /// Remembers the folder that holds the pack of a simfile.
    void rememberSongRoot(const fs::path& simfile, const ReplayFile& file);

    /// Folders that contain packs, learned from simfiles that were found.
    std::vector<std::string> mySongRoots;

    /// Replay song path -> simfile, for songs found in another place than
    /// the replay says. Most recent first.
    std::vector<std::pair<std::string, std::string>> mySongFiles;
};

};  // namespace Vortex
