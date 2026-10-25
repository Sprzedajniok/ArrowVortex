#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Vortex {

/// How much the loudness of an audio signal rises, one value per hop. Two
/// recordings of the same music have similar envelopes even when their
/// volume, mixing or pitch differ, so they can be lined up by correlation.
class OnsetEnvelope {
   public:
    /// The hop length in samples, it can be fractional.
    explicit OnsetEnvelope(double samplesPerHop);

    /// Adds mono samples.
    void add(const float* samples, size_t count);

    const std::vector<float>& values() const { return myValues; }

   private:
    double mySamplesPerHop;
    double myNextBoundary;
    size_t myIndex = 0;
    double myEnergy = 0.0;
    int myCount = 0;
    float myPrevSample = 0.0f;
    double myPrevLog = 0.0;
    bool myHasPrevLog = false;
    std::vector<float> myValues;
};

/// A part of the song to look for in a recording, in song seconds.
struct SongPart {
    double begin = 0.0, end = 0.0;
};

/// Picks count parts of the given length between begin and end, fewer if
/// they do not fit, spread evenly and leaving out the song preview, which
/// also plays (looped) in the song select menu and would match there.
std::vector<SongPart> PickSongParts(double begin, double end,
                                    double previewBegin, double previewEnd,
                                    int count, double length);

/// Where song time 0 is in a recording of the song being played.
struct RecordingSync {
    bool found = false;
    double offset = 0.0;  ///< Recording seconds at song time 0.
    int votes = 0;        ///< Parts of the song that matched at the offset.
    double score = 0.0;   ///< Their mean correlation, from 0 to 1.
    int parts = 0;        ///< Parts of the song that were looked for.
    std::string error;
};

/// Finds where the song was played in a recording. Both envelopes have the
/// same hop in recording seconds, so the song's hop is hop * rate song
/// seconds. Each part of the song is matched on its own and the offset most
/// parts agree on wins, more than half of them, which leaves out a preview
/// looping in a menu and a restart that ended early. When attempts tie, the
/// best match near hint (the expected offset, NaN if unknown) is taken.
RecordingSync FindSongInRecording(const std::vector<float>& song,
                                  const std::vector<float>& recording,
                                  double hop, double rate,
                                  const std::vector<SongPart>& parts,
                                  double hint);

};  // namespace Vortex
