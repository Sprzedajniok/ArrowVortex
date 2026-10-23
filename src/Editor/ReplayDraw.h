#pragma once

#include <string>

#include <Core/Core.h>

#include <Editor/ReplayFile.h>
#include <Editor/ReplayMatch.h>

namespace Vortex {

/// The parts of a replay that are shown, see the Replay menu.
struct ReplayLayers {
    bool presses = true;
    bool heldPanels = true;
    bool labels = true;
    bool panel = true;
    bool timeline = true;
};

/// Draws the panel hold durations, under the notes.
void DrawReplayBelowNotes(const ReplayFile& file, const ReplayResult& result,
                          const ReplayLayers& layers);

/// Draws presses, hit offsets, misses and labels, over the notes.
void DrawReplayAboveNotes(const ReplayFile& file, const ReplayResult& result,
                          const ReplayLayers& layers);

/// Draws the summary panel, with the status instead of the statistics if
/// there is one.
void DrawReplaySummary(const ReplayFile& file, const ReplayResult& result,
                       const ReplayLayers& layers, const std::string& status);

/// Draws the timeline next to the minimap.
void DrawReplayTimeline(const ReplayFile& file, const ReplayResult& result,
                        const ReplayLayers& layers);

/// The area of the timeline, for mouse input.
recti ReplayTimelineFrame();

/// Moves the cursor to the part of the chart at y on the timeline.
void SeekReplayTimeline(int y);

};  // namespace Vortex
