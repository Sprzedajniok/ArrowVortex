#include <Editor/Replay.h>

#include <format>
#include <fstream>
#include <string>
#include <vector>

#include <Simfile/Chart.h>
#include <Simfile/Notes.h>
#include <Simfile/Parsing.h>
#include <Simfile/Simfile.h>
#include <Simfile/TimingData.h>

#include <Managers/StyleMan.h>

#include <Editor/ReplayFile.h>
#include <Editor/ReplayLocate.h>
#include <Editor/ReplayMatch.h>
#include <Editor/ReplayVideo.h>
#include <Editor/Sound.h>

#include <System/File.h>

namespace Vortex {

namespace {

// The notes of a chart as the replay matching needs them. Times, warped and
// fake notes are worked out the same way NoteMan does for the open chart.
std::vector<ReplayChartNote> ChartNotes(const Simfile& sim,
                                        const Chart& chart) {
    TimingData timing;
    timing.update(chart.getTempo(&sim));

    std::vector<const Note*> notes;
    for (const Note& note : chart.notes) notes.push_back(&note);

    // Notes after a warp (an event without seconds per row), up to the next
    // event.
    std::vector<bool> warped(notes.size(), false);
    size_t n = 0;
    bool insideWarp = false;
    for (auto& event : timing.events) {
        for (; n < notes.size() && (insideWarp ? notes[n]->row < event.row
                                               : notes[n]->row <= event.row);
             ++n) {
            warped[n] = insideWarp;
        }
        insideWarp = event.spr == 0.0;
    }

    // Like NoteMan, only the first fake segment is applied.
    std::vector<bool> fake(notes.size(), false);
    for (size_t i = 0; i < notes.size(); ++i) {
        fake[i] = notes[i]->type == NOTE_FAKE;
        if (!timing.fakes.empty()) {
            auto& segment = timing.fakes.front();
            fake[i] =
                fake[i] || (notes[i]->row >= segment.row &&
                            notes[i]->row <= segment.row + segment.length);
        }
    }

    std::vector<ReplayChartNote> out;
    TempoTimeTracker tracker(timing);
    for (size_t i = 0; i < notes.size(); ++i) {
        const Note& note = *notes[i];
        double time = tracker.advance(note.row);
        if (warped[i] || fake[i]) continue;
        ReplayChartNote c;
        c.time = time;
        c.endTime =
            note.endrow == note.row ? time : timing.rowToTime(note.endrow);
        c.col = static_cast<int>(note.col);
        c.mine = note.type == NOTE_MINE;
        c.lift = note.type == NOTE_LIFT;
        c.hold = note.endrow > note.row;
        c.roll = note.type == NOTE_ROLL;
        out.push_back(c);
    }
    return out;
}

std::string FormatReport(const ReplayFile& file, const Chart& chart,
                         const ReplayResult& result) {
    static const char* const kKeys[NUM_JUDGES] = {
        "FantasticPlus", "Fantastic", "Excellent", "Great",
        "Decent",        "WayOff",    "Miss",
    };
    const ReplayStats& s = result.stats;
    std::string out;
    out += "Title\t" + file.get("Title") + "\n";
    out += std::format("Chart\t{} {}\n", GetDifficultyName(chart.difficulty),
                       chart.meter);
    out += std::format("Rate\t{:.2f}\n", file.rate);
    for (int j = 0; j < NUM_JUDGES; ++j) {
        out += std::format("{}\t{}\n", kKeys[j], s.judges[j]);
    }
    out += std::format("HoldsHeld\t{}\n", s.holdsHeld);
    out += std::format("HoldsLetGo\t{}\n", s.holdsLetGo);
    out += std::format("HoldsMissed\t{}\n", s.holdsMissed);
    out += std::format("MinesHit\t{}\n", s.minesHit);
    out += std::format("StrayTaps\t{}\n", s.strays);
    out += std::format("EarlyHits\t{}\n", s.earlyHits);
    out += std::format("EarlyHitsFixed\t{}\n", s.rescored);
    out += std::format("NotJudged\t{}\n", s.unjudged);
    out += std::format("Hits\t{}\n", s.hits);
    out += std::format("HitsMatched\t{}\n", s.hitsMatched);
    out += std::format("MeanMs\t{:.2f}\n", s.meanMs);
    out += std::format("SdMs\t{:.2f}\n", s.sdMs);
    out += std::format("Issues\t{}\n", result.issues.size());
    out += "Warning\t" + result.warning + "\n";
    return out;
}

std::string MakeVideoSync(const fs::path& replay, const fs::path& simfile,
                          const fs::path& video, bool& success) {
    success = false;
    ReplayFile file;
    std::string error;
    if (!ParseReplay(replay, file, error)) {
        return "Error\tcould not open the replay: " + error + "\n";
    }
    Simfile sim;
    if (!LoadSimfile(sim, simfile)) {
        return "Error\tcould not open the simfile\n";
    }
    fs::path music = utf8ToPath(sim.dir);
    music.append(stringToUtf8(sim.music));
    Sound song;
    std::string title, artist;
    if (sim.music.empty() || !song.load(music, false, title, artist) ||
        song.getFrequency() <= 0) {
        return "Error\tcould not load the music\n";
    }

    VideoSyncInput input = PrepareVideoSync(file, song, sim.previewStart,
                                            sim.previewLength, video);
    RecordingSync sync = SyncVideo(input, video);
    if (!sync.found) return "Error\t" + sync.error + "\n";
    success = true;
    return std::format("Offset\t{:.6f}\nVotes\t{}\nParts\t{}\nScore\t{:.3f}\n",
                       sync.offset, sync.votes, sync.parts, sync.score);
}

std::string MakeReport(const fs::path& replay, const fs::path& simfile,
                       bool& success) {
    success = false;
    ReplayFile file;
    std::string error;
    if (!ParseReplay(replay, file, error)) {
        return "Error\tcould not open the replay: " + error + "\n";
    }
    Simfile sim;
    if (!LoadSimfile(sim, simfile)) {
        return "Error\tcould not open the simfile\n";
    }
    sim.sanitize();

    // The chart that was played, picked like when opening the replay.
    const Chart* chart = nullptr;
    int bestScore = 0;
    for (const Chart* c : sim.charts) {
        int score = ScoreReplayChart(file, *c);
        if (score > bestScore) chart = c, bestScore = score;
    }
    if (!chart) {
        return "Error\tthe simfile has no " +
               StripEnumPrefix(file.get("Difficulty")) + " chart\n";
    }
    if (chart->style->numCols != file.columns) {
        return "Error\tthe replay has a different number of columns\n";
    }

    ReplayResult result =
        MatchReplay(file, ChartNotes(sim, *chart), file.columns);
    success = true;
    return FormatReport(file, *chart, result);
}

};  // anonymous namespace

bool Replay::writeReport(const fs::path& replay, const fs::path& simfile,
                         const fs::path& out) {
    // Loading a simfile looks up the styles of its charts.
    const bool createStyles = gStyle == nullptr;
    if (createStyles) StyleMan::create();
    bool success = false;
    std::string report = MakeReport(replay, simfile, success);
    if (createStyles) StyleMan::destroy();

    std::ofstream file(out, std::ios::binary);
    file << report;
    return success && file.good();
}

bool Replay::writeVideoSync(const fs::path& replay, const fs::path& simfile,
                            const fs::path& video, const fs::path& out) {
    const bool createStyles = gStyle == nullptr;
    if (createStyles) StyleMan::create();
    bool success = false;
    std::string sync = MakeVideoSync(replay, simfile, video, success);
    if (createStyles) StyleMan::destroy();

    std::ofstream file(out, std::ios::binary);
    file << sync;
    return success && file.good();
}

};  // namespace Vortex
