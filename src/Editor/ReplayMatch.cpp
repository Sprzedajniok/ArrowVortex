#include <Editor/ReplayMatch.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace Vortex {

namespace {

template <typename T>
static bool LessByTime(const T& a, const T& b) {
    return a.time < b.time;
}

struct Matcher {
    explicit Matcher(const ReplayFile& file) : myFile(file) {}

    const ReplayFile& myFile;
    std::vector<ReplayNote> myNotes;
    std::vector<ReplayPress> myPresses;
    std::vector<ReplaySpan> myHeld;
    std::vector<ReplayMark> myLetGos;
    std::vector<ReplayMark> myMines;
    std::vector<double> myIssues;
    ReplayStats myStats;
    std::string myWarning;

    int toJudge(const std::string& tns, double offset) const {
        if (tns == "Miss") return J_MISS;
        if (tns.size() != 2 || tns[0] != 'W' || tns[1] < '1' || tns[1] > '5') {
            return -1;
        }
        int w = tns[1] - '0';
        if (myFile.faPlusMode) return J_W0 + w - 1;
        if (w == 1) return std::abs(offset) <= myFile.window[0] ? J_W0 : J_W1;
        return J_W0 + w;
    }

    struct Target {
        double time = 0.0, endTime = 0.0;
        bool lift = false, hold = false, roll = false;
        int mark = -1;
        bool holdJudged = false;
    };

    struct Press {
        double time = 0.0;
        bool down = false;
        int mark = -1;
    };

    // Returns the target closest to the given time that has no judgment yet,
    // or -1 if there is none within maxDist.
    static int nearestFree(const std::vector<Target>& targets, double time,
                           double maxDist) {
        auto it = std::lower_bound(
            targets.begin(), targets.end(), time,
            [](const Target& t, double v) { return t.time < v; });
        int center = static_cast<int>(it - targets.begin());
        int best = -1;
        double bestDist = maxDist;
        for (int i = center; i < static_cast<int>(targets.size()); ++i) {
            double d = targets[i].time - time;
            if (d > bestDist) break;
            if (targets[i].mark < 0 && d <= bestDist) best = i, bestDist = d;
        }
        for (int i = center - 1; i >= 0; --i) {
            double d = time - targets[i].time;
            if (d > bestDist) break;
            if (targets[i].mark < 0 && d < bestDist) best = i, bestDist = d;
        }
        return best;
    }

    void run(const std::vector<ReplayChartNote>& chart, int numCols) {
        const double rate = myFile.rate;
        const double shift = rate * myFile.globalOffset;

        // Notes that can be judged and mines, per column.
        std::vector<std::vector<Target>> taps(numCols), mines(numCols);
        double firstNote = 1e9, lastNote = -1e9;
        for (auto& note : chart) {
            if (note.col < 0 || note.col >= numCols) continue;
            Target t;
            t.time = note.time;
            t.endTime = note.endTime;
            if (note.mine) {
                mines[note.col].push_back(t);
                continue;
            }
            t.lift = note.lift;
            t.hold = note.hold;
            t.roll = note.roll;
            taps[note.col].push_back(t);
            firstNote = std::min(firstNote, note.time);
            lastNote = std::max(lastNote, note.endTime);
        }
        for (auto& col : taps)
            std::stable_sort(col.begin(), col.end(), LessByTime<Target>);
        for (auto& col : mines)
            std::stable_sort(col.begin(), col.end(), LessByTime<Target>);

        // Panel presses and releases, per column.
        std::vector<std::vector<Press>> presses(numCols);
        for (auto& e : myFile.events) {
            if (e.type != 'P' || e.col < 0 || e.col >= numCols) continue;
            Press p;
            p.time = e.time + shift;
            p.down = e.down;
            presses[e.col].push_back(p);
        }
        for (auto& col : presses) {
            std::stable_sort(col.begin(), col.end(), LessByTime<Press>);
        }

        // Final tap judgments, in the order they happened.
        std::vector<const ReplayEvent*> judgments;
        for (auto& e : myFile.events) {
            if (e.type == 'T' && e.col >= 0 && e.col < numCols) {
                judgments.push_back(&e);
            }
        }
        std::stable_sort(judgments.begin(), judgments.end(),
                         [](const ReplayEvent* a, const ReplayEvent* b) {
                             return a->time < b->time;
                         });

        // The recorder timestamps presses slightly differently than the
        // engine does. Match once to measure that bias, then match again with
        // it removed and a tighter tolerance.
        double bias =
            matchHits(judgments, taps, presses, rate, shift, 0.0, 0.035, true);
        for (auto& col : taps) {
            for (auto& t : col) t.mark = -1;
        }
        for (auto& col : presses) {
            for (auto& p : col) p.mark = -1;
        }
        myNotes.clear();
        myPresses.clear();
        matchHits(judgments, taps, presses, rate, shift, bias, 0.02, false);

        matchMisses(judgments, taps, rate, shift);
        matchEarlyHits(presses, shift, bias);
        matchHolds(taps, presses, rate, shift);
        matchMines(mines, presses, rate, shift, bias);

        // Every press that is still unaccounted for.
        for (int c = 0; c < numCols; ++c) {
            for (auto& p : presses[c]) {
                if (!p.down || p.mark >= 0) continue;
                ReplayPress m;
                m.time = p.time + bias;
                m.col = c;
                m.kind = PRESS_STRAY;
                for (auto& t : taps[c]) {
                    if (t.hold && m.time >= t.time - 0.05 &&
                        m.time <= t.endTime + 0.05) {
                        m.kind = PRESS_HOLD;
                        break;
                    }
                }
                myPresses.push_back(m);
            }
        }

        // How long each panel was held down.
        for (int c = 0; c < numCols; ++c) {
            bool down = false;
            double begin = 0.0;
            for (auto& p : presses[c]) {
                if (p.down && !down) {
                    down = true, begin = p.time + bias;
                } else if (!p.down && down) {
                    down = false;
                    myHeld.push_back({begin, p.time + bias, c});
                }
            }
            if (down) {
                myHeld.push_back({begin, myFile.endTime + shift, c});
            }
        }

        std::stable_sort(myNotes.begin(), myNotes.end(),
                         [](const ReplayNote& a, const ReplayNote& b) {
                             return a.time != b.time ? a.time < b.time
                                                     : a.col < b.col;
                         });
        std::stable_sort(myPresses.begin(), myPresses.end(),
                         LessByTime<ReplayPress>);
        std::stable_sort(myLetGos.begin(), myLetGos.end(),
                         LessByTime<ReplayMark>);
        std::stable_sort(myMines.begin(), myMines.end(),
                         LessByTime<ReplayMark>);
        std::stable_sort(myHeld.begin(), myHeld.end(),
                         [](const ReplaySpan& a, const ReplaySpan& b) {
                             return a.begin < b.begin;
                         });

        // Notes the engine never judged, e.g. after failing.
        for (auto& col : taps) {
            for (auto& t : col) {
                if (t.mark < 0) ++myStats.unjudged;
            }
        }

        computeStats(firstNote - 0.5, lastNote + 0.5);

        // Signs that the simfile is not the version that was played: hits
        // that do not line up with any press, or all notes moved in time.
        // On the played version the presses line up within a millisecond.
        if (myStats.hits >= 20 && myStats.hitsMatched * 10 < myStats.hits * 9) {
            myWarning = std::format(
                "Only {}% of hits line up, another version of the simfile?",
                myStats.hitsMatched * 100 / myStats.hits);
        } else if (std::abs(bias) > 0.005) {
            myWarning = std::format(
                "Notes are {:.0f} ms {} than when played, sync changed?",
                std::abs(bias) * 1000.0, bias > 0.0 ? "later" : "earlier");
        }
    }

    // Matches the judgments that came from a press to a note and to that
    // press. Returns the median difference between the press time the engine
    // used and the recorded one.
    double matchHits(const std::vector<const ReplayEvent*>& judgments,
                     std::vector<std::vector<Target>>& taps,
                     std::vector<std::vector<Press>>& presses, double rate,
                     double shift, double bias, double tolerance,
                     bool measure) {
        std::vector<double> deltas;
        const double lookback = 0.5 + std::max(1.0, rate);
        for (const ReplayEvent* e : judgments) {
            int judge = toJudge(e->score, e->offset);
            if (judge < 0 || judge == J_MISS) continue;

            auto& targets = taps[e->col];
            auto& colPresses = presses[e->col];
            const double t = e->time + shift;
            const double off = e->offset * rate;
            const bool lift = e->noteType == "Lift";

            // The press the engine judged is the one at note time + offset.
            int bestTarget = -1, bestPress = -1;
            double bestErr = tolerance;
            auto first = std::lower_bound(
                colPresses.begin(), colPresses.end(), t - lookback,
                [](const Press& p, double v) { return p.time < v; });
            for (int i = static_cast<int>(first - colPresses.begin());
                 i < static_cast<int>(colPresses.size()); ++i) {
                const Press& p = colPresses[i];
                if (p.time > t + 0.05) break;
                if (p.down == lift || p.mark >= 0) continue;
                int target = nearestFree(targets, p.time + bias - off, bestErr);
                if (target < 0) continue;
                double err =
                    std::abs(targets[target].time - (p.time + bias - off));
                if (err <= bestErr) {
                    bestErr = err, bestTarget = target, bestPress = i;
                }
            }

            if (!measure) {
                ++myStats.hits;
                if (bestPress >= 0) ++myStats.hitsMatched;
            }

            // No recorded press, fall back to the time of the judgment.
            if (bestTarget < 0) {
                bestTarget = nearestFree(targets, t - off, 0.5);
                if (bestTarget < 0) continue;
            }

            Target& target = targets[bestTarget];
            target.mark = static_cast<int>(myNotes.size());

            ReplayNote n;
            n.time = target.time;
            n.col = e->col;
            n.judge = judge;
            n.offset = e->offset;
            n.hasPress = true;
            n.pressTime = target.time + off;
            myNotes.push_back(n);

            if (bestPress >= 0) {
                Press& p = colPresses[bestPress];
                p.mark = target.mark;
                deltas.push_back(n.pressTime - p.time);
                ReplayPress m;
                m.time = n.pressTime;
                m.col = e->col;
                m.kind = PRESS_HIT;
                m.judge = judge;
                myPresses.push_back(m);
            }

            // An early decent/way off that this hit replaced.
            int earlyJudge = toJudge(e->earlyScore, e->earlyOffset);
            if (!measure && earlyJudge >= 0 && earlyJudge != J_MISS &&
                std::abs(e->earlyOffset - e->offset) > 1e-5) {
                ReplayNote& note = myNotes.back();
                note.earlyJudge = earlyJudge;
                note.earlyOffset = e->earlyOffset;
                double earlyTime = target.time + e->earlyOffset * rate;
                linkPress(colPresses, earlyTime - bias, 0.02, !lift, e->col,
                          PRESS_EARLY, earlyJudge, earlyTime);
            }
        }

        if (deltas.empty()) return 0.0;
        std::nth_element(deltas.begin(), deltas.begin() + deltas.size() / 2,
                         deltas.end());
        return deltas[deltas.size() / 2];
    }

    // Marks the unused press closest to the given time, if there is one within
    // maxDist. Returns true if a press was marked.
    bool linkPress(std::vector<Press>& colPresses, double time, double maxDist,
                   bool down, int col, int kind, int judge, double drawTime) {
        int best = -1;
        double bestDist = maxDist;
        for (int i = 0; i < static_cast<int>(colPresses.size()); ++i) {
            const Press& p = colPresses[i];
            if (p.down != down || p.mark >= 0) continue;
            double d = std::abs(p.time - time);
            if (d <= bestDist) best = i, bestDist = d;
        }
        if (best < 0) return false;
        colPresses[best].mark = 0;
        ReplayPress m;
        m.time = drawTime;
        m.col = col;
        m.kind = kind;
        m.judge = judge;
        myPresses.push_back(m);
        return true;
    }

    void matchMisses(const std::vector<const ReplayEvent*>& judgments,
                     std::vector<std::vector<Target>>& taps, double rate,
                     double shift) {
        // A miss is reported once the note left the largest timing window.
        double window = 0.0;
        for (double w : myFile.window) window = std::max(window, w);
        const double delay = window * rate;
        for (const ReplayEvent* e : judgments) {
            if (e->score != "Miss") continue;
            const double t = e->time + shift;
            int target = nearestFree(taps[e->col], t - delay, 0.4);
            if (target < 0) continue;
            Target& tgt = taps[e->col][target];
            tgt.mark = static_cast<int>(myNotes.size());
            ReplayNote n;
            n.time = tgt.time;
            n.col = e->col;
            n.judge = J_MISS;
            n.heldMiss = e->heldMiss;
            myNotes.push_back(n);
        }
    }

    // Counts the early hits, and marks the presses of early hits that never
    // got a final judgment in the replay (e.g. because the song ended first).
    void matchEarlyHits(std::vector<std::vector<Press>>& presses, double shift,
                        double bias) {
        for (auto& e : myFile.events) {
            if (e.type != 'E' || e.col < 0 ||
                e.col >= static_cast<int>(presses.size())) {
                continue;
            }
            int judge = toJudge(e.score, e.offset);
            if (judge < 0) continue;
            ++myStats.earlyHits;

            bool judged = false;
            for (auto& t : myFile.events) {
                if (t.type == 'T' && t.col == e.col &&
                    std::abs(t.earlyOffset - e.offset) < 1e-5) {
                    judged = true;
                    break;
                }
            }
            if (judged) continue;
            const double t = e.time + shift;
            linkPress(presses[e.col], t, 0.03, true, e.col, PRESS_EARLY, judge,
                      t + bias);
        }
    }

    void matchHolds(std::vector<std::vector<Target>>& taps,
                    std::vector<std::vector<Press>>& presses, double rate,
                    double shift) {
        for (auto& e : myFile.events) {
            if (e.type != 'H' || e.col < 0 ||
                e.col >= static_cast<int>(taps.size())) {
                continue;
            }
            const double t = e.time + shift;

            // The latest hold that started before the result came in.
            auto& targets = taps[e.col];
            auto it = std::upper_bound(
                targets.begin(), targets.end(), t + 0.005,
                [](double v, const Target& target) { return v < target.time; });
            Target* hold = nullptr;
            while (it != targets.begin() && !hold) {
                --it;
                if (it->hold && !it->holdJudged) hold = &*it;
            }
            if (!hold) continue;
            hold->holdJudged = true;

            if (e.score == "Held") {
                ++myStats.holdsHeld;
            } else if (e.score == "LetGo") {
                ++myStats.holdsLetGo;
                // The foot came off at the last release inside the hold. A
                // dropped hold is reported a hold window after the release,
                // a dropped roll a roll window after its last tap.
                double window =
                    hold->roll ? myFile.rollWindow : myFile.holdWindow;
                double drop = t - window * rate;
                for (auto& p : presses[e.col]) {
                    if (p.time > t) break;
                    if (!p.down && p.time >= hold->time) drop = p.time;
                }
                myLetGos.push_back({drop, e.col});
            } else {
                ++myStats.holdsMissed;
            }
        }
    }

    void matchMines(std::vector<std::vector<Target>>& mines,
                    std::vector<std::vector<Press>>& presses, double rate,
                    double shift, double bias) {
        for (auto& e : myFile.events) {
            if (e.type != 'M' || e.col < 0 ||
                e.col >= static_cast<int>(mines.size())) {
                continue;
            }
            const double t = e.time + shift;
            int mine = nearestFree(mines[e.col], t, 0.25);
            if (mine < 0) continue;
            Target& m = mines[e.col][mine];
            m.mark = 0;
            ++myStats.minesHit;
            myMines.push_back({m.time, e.col});
            linkPress(presses[e.col], m.time - bias,
                      myFile.mineWindow * rate + 0.02, true, e.col, PRESS_MINE,
                      -1, m.time);
        }
    }

    void computeStats(double chartBegin, double chartEnd) {
        double sum = 0.0, sumSq = 0.0;
        for (auto& n : myNotes) {
            ++myStats.judges[n.judge];
            if (n.judge == J_MISS) {
                myIssues.push_back(n.time);
                continue;
            }
            if (n.judge >= J_W4) myIssues.push_back(n.time);
            if (n.earlyJudge >= 0) {
                ++myStats.rescored;
                if (n.judge < J_W4) myIssues.push_back(n.time);
            }
            double ms = n.offset * 1000.0;
            sum += ms, sumSq += ms * ms;
            ++myStats.numOffsets;
        }
        if (myStats.numOffsets > 0) {
            double count = static_cast<double>(myStats.numOffsets);
            myStats.meanMs = sum / count;
            myStats.sdMs = std::sqrt(
                std::max(0.0, sumSq / count - myStats.meanMs * myStats.meanMs));
        }
        for (auto& p : myPresses) {
            if (p.kind != PRESS_STRAY) continue;
            if (p.time < chartBegin || p.time > chartEnd) continue;
            ++myStats.strays;
            myIssues.push_back(p.time);
        }
        for (auto& m : myLetGos) myIssues.push_back(m.time);
        for (auto& m : myMines) myIssues.push_back(m.time);
        std::sort(myIssues.begin(), myIssues.end());
    }
};

};  // anonymous namespace

ReplayResult MatchReplay(const ReplayFile& file,
                         const std::vector<ReplayChartNote>& chart,
                         int numCols) {
    Matcher matcher(file);
    matcher.run(chart, numCols);
    ReplayResult result;
    result.notes = std::move(matcher.myNotes);
    result.presses = std::move(matcher.myPresses);
    result.held = std::move(matcher.myHeld);
    result.letGos = std::move(matcher.myLetGos);
    result.mines = std::move(matcher.myMines);
    result.issues = std::move(matcher.myIssues);
    result.stats = matcher.myStats;
    result.warning = std::move(matcher.myWarning);
    return result;
}

};  // namespace Vortex
