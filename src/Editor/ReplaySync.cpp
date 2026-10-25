#include <Editor/ReplaySync.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace Vortex {

namespace {

/// Envelopes are first matched downsampled by this factor, then refined.
const int kCoarse = 4;

/// The lowest correlation that counts as a match of a song part.
const double kMinCorrelation = 0.3;

/// Matches of a part at least this many seconds apart are different matches.
const double kPeakDistance = 0.5;

/// The most matches kept for each part, e.g. several restarted attempts.
const size_t kMaxPeaks = 8;

/// Offsets of different parts this close are the same attempt.
const double kSameOffset = 0.03;

/// How far from the expected offset the song can start, the replay date is
/// when gameplay started and the music starts a few seconds later.
const double kHintRange = 20.0;

/// A rise in log energy above this is clipped, so the start of the music
/// after silence does not outweigh everything else.
const double kMaxRise = 4.0;

/// A part of the song, ready to be correlated.
struct Pattern {
    int first = 0;              ///< First song envelope frame.
    std::vector<float> fine;    ///< Zero mean, unit length.
    std::vector<float> coarse;  ///< Downsampled, zero mean, unit length.
};

std::vector<float> Downsample(const std::vector<float>& values, int factor) {
    std::vector<float> out(values.size() / factor);
    for (size_t i = 0; i < out.size(); ++i) {
        float sum = 0.0f;
        for (int j = 0; j < factor; ++j) sum += values[i * factor + j];
        out[i] = sum;
    }
    return out;
}

// Makes the values zero mean and unit length. Returns false for silence.
bool Normalize(std::vector<float>& values) {
    if (values.empty()) return false;
    double mean = 0.0;
    for (float v : values) mean += v;
    mean /= static_cast<double>(values.size());
    double length = 0.0;
    for (float& v : values) {
        v = static_cast<float>(v - mean);
        length += static_cast<double>(v) * v;
    }
    length = std::sqrt(length);
    if (length < 1e-6) return false;
    for (float& v : values) v = static_cast<float>(v / length);
    return true;
}

// Prefix sums of a signal and its square, for the mean and spread of any
// window.
struct Sums {
    explicit Sums(const std::vector<float>& signal)
        : sum(signal.size() + 1, 0.0), squares(signal.size() + 1, 0.0) {
        for (size_t i = 0; i < signal.size(); ++i) {
            sum[i + 1] = sum[i] + signal[i];
            squares[i + 1] =
                squares[i] + static_cast<double>(signal[i]) * signal[i];
        }
    }

    // The length of the window minus its mean.
    double spread(size_t begin, size_t count) const {
        double s = sum[begin + count] - sum[begin];
        double q = squares[begin + count] - squares[begin];
        return std::sqrt(std::max(0.0, q - s * s / static_cast<double>(count)));
    }

    std::vector<double> sum, squares;
};

// Correlation of a normalized pattern with the signal window at lag. The
// pattern has zero mean, so the window's mean drops out of the product.
double CorrelateAt(const std::vector<float>& pattern,
                   const std::vector<float>& signal, const Sums& sums,
                   long lag) {
    if (lag < 0 || lag + pattern.size() > signal.size()) return 0.0;
    double spread = sums.spread(static_cast<size_t>(lag), pattern.size());
    if (spread < 1e-9) return 0.0;
    const float* window = signal.data() + lag;
    float dot = 0.0f;
    for (size_t j = 0; j < pattern.size(); ++j) dot += pattern[j] * window[j];
    return dot / spread;
}

// The best matches of a pattern: local correlation maxima above the minimum,
// at least minDistance lags apart, best first.
std::vector<std::pair<double, long>> FindPeaks(
    const std::vector<float>& pattern, const std::vector<float>& signal,
    long minDistance) {
    std::vector<std::pair<double, long>> peaks;
    if (pattern.empty() || signal.size() < pattern.size()) return peaks;
    Sums sums(signal);
    long numLags = static_cast<long>(signal.size() - pattern.size()) + 1;
    std::vector<double> corr(numLags);
    for (long lag = 0; lag < numLags; ++lag) {
        corr[lag] = CorrelateAt(pattern, signal, sums, lag);
    }
    for (long lag = 0; lag < numLags; ++lag) {
        if (corr[lag] < kMinCorrelation) continue;
        if (lag > 0 && corr[lag - 1] > corr[lag]) continue;
        if (lag + 1 < numLags && corr[lag + 1] > corr[lag]) continue;
        peaks.emplace_back(corr[lag], lag);
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::pair<double, long>> kept;
    for (auto& peak : peaks) {
        bool near = false;
        for (auto& k : kept) {
            near = near || std::abs(k.second - peak.second) < minDistance;
        }
        if (near) continue;
        kept.push_back(peak);
        if (kept.size() == kMaxPeaks) break;
    }
    return kept;
}

};  // anonymous namespace

// ================================================================================================
// OnsetEnvelope.

OnsetEnvelope::OnsetEnvelope(double samplesPerHop)
    : mySamplesPerHop(std::max(1.0, samplesPerHop)),
      myNextBoundary(mySamplesPerHop) {}

void OnsetEnvelope::add(const float* samples, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        // A first order high-pass makes drums and other attacks stand out.
        float x = samples[i];
        float y = x - 0.97f * myPrevSample;
        myPrevSample = x;
        myEnergy += static_cast<double>(y) * y;
        ++myCount;
        ++myIndex;
        if (static_cast<double>(myIndex) < myNextBoundary) continue;

        double logEnergy = std::log(1e-10 + myEnergy / myCount);
        double rise = myHasPrevLog ? logEnergy - myPrevLog : 0.0;
        myValues.push_back(static_cast<float>(std::clamp(rise, 0.0, kMaxRise)));
        myPrevLog = logEnergy;
        myHasPrevLog = true;
        myEnergy = 0.0;
        myCount = 0;
        myNextBoundary += mySamplesPerHop;
    }
}

// ================================================================================================
// Finding the song.

std::vector<SongPart> PickSongParts(double begin, double end,
                                    double previewBegin, double previewEnd,
                                    int count, double length) {
    // The song before and after the preview, and how many parts fit in each.
    struct Span {
        double begin, end;
        int fit, parts = 0;
    };
    std::vector<Span> spans;
    for (auto [b, e] : {std::pair{begin, std::min(end, previewBegin)},
                        std::pair{std::max(begin, previewEnd), end}}) {
        int fit = e > b ? static_cast<int>((e - b) / length) : 0;
        if (fit > 0) spans.push_back({b, e, fit});
    }
    // Hand out the parts one by one to the span with the most room left per
    // part, so they spread over the whole song.
    for (int i = 0; i < count; ++i) {
        Span* best = nullptr;
        for (auto& span : spans) {
            if (span.parts == span.fit) continue;
            double room = (span.end - span.begin) / (span.parts + 1);
            if (!best || room > (best->end - best->begin) / (best->parts + 1)) {
                best = &span;
            }
        }
        if (!best) break;
        ++best->parts;
    }
    std::vector<SongPart> parts;
    for (auto& span : spans) {
        double gap =
            (span.end - span.begin - span.parts * length) / (span.parts + 1);
        for (int i = 0; i < span.parts; ++i) {
            double b = span.begin + gap + i * (length + gap);
            parts.push_back({b, b + length});
        }
    }
    return parts;
}

RecordingSync FindSongInRecording(const std::vector<float>& song,
                                  const std::vector<float>& recording,
                                  double hop, double rate,
                                  const std::vector<SongPart>& parts,
                                  double hint) {
    RecordingSync result;
    const double songHop = hop * rate;

    std::vector<Pattern> patterns;
    for (auto& part : parts) {
        int first = static_cast<int>(std::ceil(part.begin / songHop));
        int last = static_cast<int>(std::floor(part.end / songHop));
        if (first < 0 || last > static_cast<int>(song.size()) ||
            last - first < kCoarse * 16) {
            continue;
        }
        Pattern p;
        p.first = first;
        p.fine.assign(song.begin() + first, song.begin() + last);
        p.coarse = Downsample(p.fine, kCoarse);
        if (!Normalize(p.fine) || !Normalize(p.coarse)) continue;  // Silence.
        patterns.push_back(std::move(p));
    }
    result.parts = static_cast<int>(patterns.size());
    if (patterns.empty()) {
        result.error = "the song has no parts to look for";
        return result;
    }

    // Where each part matches, as offsets of song time 0.
    struct Match {
        double offset;
        int part;
        double score;
    };
    std::vector<Match> matches;
    const std::vector<float> coarse = Downsample(recording, kCoarse);
    const Sums fineSums(recording);
    const long minDistance =
        std::max(1L, std::lround(kPeakDistance / (hop * kCoarse)));
    for (int i = 0; i < static_cast<int>(patterns.size()); ++i) {
        const Pattern& p = patterns[i];
        for (auto& [score, lag] : FindPeaks(p.coarse, coarse, minDistance)) {
            // Refine at full resolution around the coarse match.
            long best = lag * kCoarse;
            double bestScore = -1.0;
            for (long l = (lag - 1) * kCoarse; l <= (lag + 1) * kCoarse; ++l) {
                double s = CorrelateAt(p.fine, recording, fineSums, l);
                if (s > bestScore) best = l, bestScore = s;
            }
            matches.push_back({(best - p.first) * hop, i, bestScore});
        }
    }
    if (matches.empty()) {
        result.error = "the song was not found in the recording";
        return result;
    }

    // Matches of different parts at the same offset are the same attempt.
    struct Attempt {
        double offset = 0.0, score = 0.0;
        int votes = 0;
    };
    std::vector<Attempt> attempts;
    std::sort(
        matches.begin(), matches.end(),
        [](const Match& a, const Match& b) { return a.offset < b.offset; });
    for (size_t i = 0; i < matches.size();) {
        size_t j = i + 1;
        while (j < matches.size() &&
               matches[j].offset - matches[j - 1].offset <= kSameOffset) {
            ++j;
        }
        std::vector<double> best(patterns.size(), 0.0);
        double sum = 0.0;
        for (size_t k = i; k < j; ++k) {
            best[matches[k].part] =
                std::max(best[matches[k].part], matches[k].score);
            sum += matches[k].offset;
        }
        Attempt a;
        a.offset = sum / static_cast<double>(j - i);
        for (double b : best) {
            a.votes += b > 0.0 ? 1 : 0;
            a.score += b;
        }
        attempts.push_back(a);
        i = j;
    }

    int maxVotes = 0;
    for (auto& a : attempts) maxVotes = std::max(maxVotes, a.votes);
    // The parts are from the part of the song that was played, so most of
    // them are found in the play. Fewer is e.g. a restart before it.
    const int needed =
        std::max(std::min(2, result.parts), result.parts / 2 + 1);
    if (maxVotes < needed) {
        result.error = std::format("only {} of {} parts of the song matched",
                                   maxVotes, result.parts);
        return result;
    }
    // Attempts with the most parts found, e.g. the same song played twice
    // or one bar off in repetitive music. Near the expected offset the best
    // match wins, an expected offset from another computer's clock can be
    // further off.
    const Attempt* chosen = nullptr;
    auto better = [&](const Attempt& a, const Attempt& b) {
        if (std::isnan(hint)) return a.score > b.score;
        bool nearA = std::abs(a.offset - hint) <= kHintRange;
        bool nearB = std::abs(b.offset - hint) <= kHintRange;
        if (nearA != nearB) return nearA;
        if (nearA) return a.score > b.score;
        return std::abs(a.offset - hint) < std::abs(b.offset - hint);
    };
    for (auto& a : attempts) {
        if (a.votes == maxVotes && (!chosen || better(a, *chosen))) {
            chosen = &a;
        }
    }

    // The best lag of all parts together around the attempt.
    const long center = std::lround(chosen->offset / hop);
    long lag = center;
    double bestScore = -1.0;
    for (long l = center - 2; l <= center + 2; ++l) {
        double s = 0.0;
        for (auto& p : patterns) {
            s += CorrelateAt(p.fine, recording, fineSums, l + p.first);
        }
        if (s > bestScore) lag = l, bestScore = s;
    }

    result.found = true;
    result.offset = static_cast<double>(lag) * hop;
    result.votes = chosen->votes;
    result.score = chosen->score / chosen->votes;
    return result;
}

};  // namespace Vortex
