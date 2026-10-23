#pragma once

#include <string>
#include <vector>

#include <Editor/ReplayFile.h>

namespace Vortex {

// ================================================================================================
// Judgments, as Simply Love shows them.

/// Fantastic is split into the FA+ (blue) and regular (white) window.
enum Judge { J_W0, J_W1, J_W2, J_W3, J_W4, J_W5, J_MISS, NUM_JUDGES };

/// What a panel press ended up doing.
enum PressKind {
    PRESS_HIT,    ///< Judged a note.
    PRESS_EARLY,  ///< Early decent/way off that a later press rescored.
    PRESS_HOLD,   ///< Re-pressed inside a hold or roll body.
    PRESS_MINE,   ///< Stepped on a mine.
    PRESS_STRAY,  ///< Did not activate anything.
};

// ================================================================================================
// Results of matching the replay against the chart. All times are chart
// times, the same clock the notes and the music use in the editor.

struct ReplayNote {
    double time = 0.0;
    int col = 0;
    int judge = J_MISS;
    double offset = 0.0;  ///< Engine offset in real seconds, negative is early.
    bool hasPress = false;
    double pressTime = 0.0;
    int earlyJudge = -1;  ///< Early hit that was rescored, -1 if none.
    double earlyOffset = 0.0;
    bool heldMiss = false;
};

struct ReplayPress {
    double time = 0.0;
    int col = 0;
    int kind = PRESS_STRAY;
    int judge = -1;
};

struct ReplaySpan {
    double begin = 0.0, end = 0.0;
    int col = 0;
};

struct ReplayMark {
    double time = 0.0;
    int col = 0;
};

struct ReplayStats {
    int judges[NUM_JUDGES] = {};
    int holdsHeld = 0, holdsLetGo = 0, holdsMissed = 0;
    int minesHit = 0;
    int strays = 0;
    int earlyHits = 0;  ///< Early decents/way offs, rescored or not.
    int rescored = 0;   ///< Early hits that a later press improved.
    int unjudged = 0;
    int hits = 0;         ///< Hits, judgments that are not a miss.
    int hitsMatched = 0;  ///< Hits that line up with a recorded press.
    int numOffsets = 0;
    double meanMs = 0.0, sdMs = 0.0;
};

/// A note of the chart, as the matching needs it.
struct ReplayChartNote {
    double time = 0.0, endTime = 0.0;
    int col = 0;
    bool mine = false, lift = false, hold = false, roll = false;
};

struct ReplayResult {
    std::vector<ReplayNote> notes;     ///< Judged notes, by time and column.
    std::vector<ReplayPress> presses;  ///< Every press, by time.
    std::vector<ReplaySpan> held;      ///< Panels held down, by start.
    std::vector<ReplayMark> letGos;    ///< Dropped holds, by time.
    std::vector<ReplayMark> mines;     ///< Hit mines, by time.
    std::vector<double> issues;        ///< Times F7/F8 stop at, sorted.
    ReplayStats stats;
    std::string warning;  ///< Signs of a different version of the chart.
};

/// Matches the events of a replay to the notes of the chart it was played
/// on. The chart notes do not need to be sorted, warped and fake notes
/// should be left out.
ReplayResult MatchReplay(const ReplayFile& file,
                         const std::vector<ReplayChartNote>& chart,
                         int numCols);

};  // namespace Vortex
