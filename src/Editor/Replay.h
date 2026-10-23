#pragma once

#include <Core/Core.h>
#include <Core/Input.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace Vortex {

/// Shows an ITGmania play recorded by the ArrowReplay Simply Love module on
/// top of the notefield: every judged note with its timing offset, misses,
/// dropped holds, hit mines and every panel press, including presses that did
/// not hit any note.
struct Replay : public InputHandler {
    static void create(XmrNode& settings);
    static void destroy();

    /// Returns true if the path has the replay file extension.
    static bool isReplayFile(const fs::path& path);

    /// Matches a replay to the chart it was played on in the given simfile and
    /// writes the summary to a text file, without opening the editor. Used by
    /// "ArrowVortex --replay-report REPLAY SIMFILE OUT" and the CI tests.
    /// Returns false (and writes the error) if the replay can not be shown.
    static bool writeReport(const fs::path& replay, const fs::path& simfile,
                            const fs::path& out);

    virtual void saveSettings(XmrNode& settings) = 0;

    /// Called by the editor when changes were made to the simfile.
    virtual void onChanges(int changes) = 0;

    /// Prompts the user to select a replay file and opens it.
    virtual bool openReplay() = 0;

    /// Loads a replay file, opens the simfile and chart it was played on and
    /// shows it on the notefield.
    virtual bool openReplay(const fs::path& path) = 0;

    virtual void closeReplay() = 0;
    virtual bool isOpen() const = 0;

    /// Moves the cursor to the next or previous miss, stray press, decent,
    /// way off, dropped hold or hit mine.
    virtual void jumpToIssue(bool forward) = 0;

    /// Draws the panel hold durations, under the notes.
    virtual void drawBelowNotes() = 0;

    /// Draws presses, hit offsets, misses and labels, over the notes.
    virtual void drawAboveNotes() = 0;

    /// Draws the summary panel and the timeline next to the minimap.
    virtual void drawPanel() = 0;

    /// Scrolls the view while the timeline is dragged.
    virtual void tick() = 0;

    virtual void toggleShowPresses() = 0;
    virtual void toggleShowHeldPanels() = 0;
    virtual void toggleShowLabels() = 0;
    virtual void toggleShowPanel() = 0;
    virtual void toggleShowTimeline() = 0;

    virtual bool hasShowPresses() const = 0;
    virtual bool hasShowHeldPanels() const = 0;
    virtual bool hasShowLabels() const = 0;
    virtual bool hasShowPanel() const = 0;
    virtual bool hasShowTimeline() const = 0;
};

extern Replay* gReplay;

};  // namespace Vortex
