#include <Editor/ReplayDraw.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include <Core/Draw.h>
#include <Core/StringUtils.h>
#include <Core/Text.h>
#include <Core/Utils.h>

#include <System/System.h>

#include <Managers/SimfileMan.h>
#include <Managers/StyleMan.h>
#include <Managers/TempoMan.h>

#include <Editor/Shortcuts.h>
#include <Editor/View.h>

namespace Vortex {

namespace {

// ================================================================================================
// Colors, as Simply Love shows them.

const uint32_t kJudgeColors[NUM_JUDGES] = {
    RGBAtoColor32(33, 204, 232, 255),   // Fantastic (blue)
    RGBAtoColor32(255, 255, 255, 255),  // Fantastic (white)
    RGBAtoColor32(226, 156, 24, 255),   // Excellent
    RGBAtoColor32(102, 201, 85, 255),   // Great
    RGBAtoColor32(180, 92, 255, 255),   // Decent
    RGBAtoColor32(201, 133, 94, 255),   // Way Off
    RGBAtoColor32(255, 48, 48, 255),    // Miss
};

const char* const kJudgeNames[NUM_JUDGES] = {
    "Fantastic+", "Fantastic", "Excellent", "Great",
    "Decent",     "Way Off",   "Miss",
};

const char* const kJudgeMarkup[NUM_JUDGES] = {
    "{tc:21CCE8}", "{tc:FFFFFF}", "{tc:E29C18}", "{tc:66C955}",
    "{tc:B45CFF}", "{tc:C9855E}", "{tc:FF3030}",
};

const uint32_t kStrayColor = RGBAtoColor32(255, 64, 200, 255);
const uint32_t kHoldPressColor = RGBAtoColor32(170, 170, 170, 255);
const uint32_t kHeldPanelColor = RGBAtoColor32(255, 255, 255, 72);
const uint32_t kOutlineColor = RGBAtoColor32(0, 0, 0, 200);
const uint32_t kLetGoColor = RGBAtoColor32(255, 144, 48, 255);
const uint32_t kViewRangeColor = RGBAtoColor32(255, 204, 102, 60);

struct Drawer {
    Drawer(const ReplayFile& file, const ReplayResult& result,
           const ReplayLayers& layers)
        : myFile(file),
          myNotes(result.notes),
          myPresses(result.presses),
          myHeld(result.held),
          myLetGos(result.letGos),
          myMines(result.mines),
          myStats(result.stats),
          myWarning(result.warning),
          myShowPresses(layers.presses),
          myShowLabels(layers.labels) {}

    const ReplayFile& myFile;
    const std::vector<ReplayNote>& myNotes;
    const std::vector<ReplayPress>& myPresses;
    const std::vector<ReplaySpan>& myHeld;
    const std::vector<ReplayMark>& myLetGos;
    const std::vector<ReplayMark>& myMines;
    const ReplayStats& myStats;
    const std::string& myWarning;
    const bool myShowPresses;
    const bool myShowLabels;

    struct Geometry {
        int colX[SIM_MAX_COLUMNS] = {};
        int colW = 64;
        int numCols = 0;
        double begin = 0.0, end = 0.0;
    };

    Geometry geometry() const {
        Geometry g;
        g.numCols =
            std::min(gStyle->getNumCols(), static_cast<int>(SIM_MAX_COLUMNS));
        for (int c = 0; c < g.numCols; ++c) g.colX[c] = gView->columnToX(c);
        g.colW = gSystem->applyScaleFactor(gView->applyZoom(64));
        if (g.numCols > 1) {
            g.colW = std::max(8, std::abs(g.colX[1] - g.colX[0]));
        }
        g.begin = gView->offsetToTime(gView->yToOffset(-64));
        g.end = gView->offsetToTime(gView->yToOffset(gView->getHeight() + 64));
        if (g.begin > g.end) std::swap(g.begin, g.end);
        return g;
    }

    template <typename T>
    static auto firstVisible(const std::vector<T>& list, double time) {
        return std::lower_bound(
            list.begin(), list.end(), time,
            [](const T& item, double v) { return item.time < v; });
    }

    void drawBelowNotes() const {
        Geometry g = geometry();

        Renderer::resetColor();
        Renderer::bindShader(Renderer::SH_COLOR);
        auto batch = Renderer::batchC();
        int w = std::max(4, g.colW / 2);
        for (auto& span : myHeld) {
            if (span.begin > g.end) break;
            if (span.end < g.begin) continue;
            int y0 = gView->timeToY(span.begin), y1 = gView->timeToY(span.end);
            if (y0 > y1) std::swap(y0, y1);
            Draw::fill(&batch,
                       {g.colX[span.col] - w / 2, y0, w, std::max(1, y1 - y0)},
                       kHeldPanelColor);
        }
        batch.flush();
    }

    // Draws a horizontal bar with a dark outline.
    static void drawBar(QuadBatchC* batch, int x, int y, int w, int h,
                        uint32_t color) {
        Draw::fill(batch, {x - w / 2 - 1, y - h / 2 - 1, w + 2, h + 2},
                   kOutlineColor);
        Draw::fill(batch, {x - w / 2, y - h / 2, w, h}, color);
    }

    static void drawBox(QuadBatchC* batch, recti r, int t, uint32_t color) {
        Draw::fill(batch, {r.x, r.y, r.w, t}, color);
        Draw::fill(batch, {r.x, r.y + r.h - t, r.w, t}, color);
        Draw::fill(batch, {r.x, r.y, t, r.h}, color);
        Draw::fill(batch, {r.x + r.w - t, r.y, t, r.h}, color);
    }

    void drawAboveNotes() const {
        Geometry g = geometry();
        const int thick = std::max(2, gSystem->applyScaleFactor(3));
        const int line = std::max(1, gSystem->applyScaleFactor(2));
        const int box = g.colW * 9 / 10;

        Renderer::resetColor();
        Renderer::bindShader(Renderer::SH_COLOR);
        auto batch = Renderer::batchC();

        // Misses: a red box over the note.
        for (auto it = firstVisible(myNotes, g.begin);
             it != myNotes.end() && it->time <= g.end; ++it) {
            if (it->judge != J_MISS) continue;
            int x = g.colX[it->col], y = gView->timeToY(it->time);
            recti r = {x - box / 2, y - box / 2, box, box};
            Draw::fill(&batch, r, Color32a(kJudgeColors[J_MISS], 70));
            drawBox(&batch, r, line, kJudgeColors[J_MISS]);
        }

        // Hit mines.
        for (auto it = firstVisible(myMines, g.begin);
             it != myMines.end() && it->time <= g.end; ++it) {
            int x = g.colX[it->col], y = gView->timeToY(it->time);
            recti r = {x - box / 2, y - box / 2, box, box};
            drawBox(&batch, r, line, kJudgeColors[J_MISS]);
        }

        if (myShowPresses) {
            // Lines from each note to the press that hit it.
            for (auto it = firstVisible(myNotes, g.begin - 0.5);
                 it != myNotes.end() && it->time <= g.end + 0.5; ++it) {
                if (it->judge == J_MISS || !it->hasPress) continue;
                int x = g.colX[it->col];
                int y0 = gView->timeToY(it->time);
                int y1 = gView->timeToY(it->pressTime);
                if (y0 > y1) std::swap(y0, y1);
                Draw::fill(&batch, {x - line / 2, y0, line, y1 - y0 + 1},
                           Color32a(kJudgeColors[it->judge], 190));
            }

            // Dashed lines from rescored notes to their early press.
            const int dash = std::max(2, gSystem->applyScaleFactor(4));
            for (auto it = firstVisible(myNotes, g.begin - 0.5);
                 it != myNotes.end() && it->time <= g.end + 0.5; ++it) {
                if (it->earlyJudge < 0) continue;
                int x = g.colX[it->col] + line * 2;
                int y0 = gView->timeToY(it->time);
                int y1 =
                    gView->timeToY(it->time + it->earlyOffset * myFile.rate);
                if (y0 > y1) std::swap(y0, y1);
                for (int y = y0; y < y1; y += dash * 2) {
                    Draw::fill(&batch,
                               {x - line / 2, y, line, std::min(dash, y1 - y)},
                               kJudgeColors[it->earlyJudge]);
                }
            }

            // Every press.
            for (auto it = firstVisible(myPresses, g.begin);
                 it != myPresses.end() && it->time <= g.end; ++it) {
                int x = g.colX[it->col], y = gView->timeToY(it->time);
                switch (it->kind) {
                    case PRESS_HIT:
                        drawBar(&batch, x, y, g.colW * 8 / 10, thick,
                                kJudgeColors[it->judge]);
                        break;
                    case PRESS_EARLY:
                        drawBar(&batch, x, y, g.colW * 7 / 10, thick,
                                kJudgeColors[it->judge]);
                        break;
                    case PRESS_HOLD:
                        drawBar(&batch, x, y, g.colW * 5 / 10, thick,
                                kHoldPressColor);
                        break;
                    case PRESS_MINE:
                        drawBar(&batch, x, y, g.colW * 8 / 10, thick,
                                kJudgeColors[J_MISS]);
                        break;
                    default:
                        drawBar(&batch, x, y, g.colW * 9 / 10, thick + 2,
                                kStrayColor);
                        break;
                }
            }
        }

        // Dropped holds: a red bar where the foot came off.
        for (auto it = firstVisible(myLetGos, g.begin);
             it != myLetGos.end() && it->time <= g.end; ++it) {
            int x = g.colX[it->col], y = gView->timeToY(it->time);
            drawBar(&batch, x, y, g.colW * 9 / 10, thick + 2,
                    kJudgeColors[J_MISS]);
        }

        batch.flush();

        if (myShowLabels) drawLabels(g);
    }

    // Draws the timing of each note (and other events) to the right of the
    // notefield.
    void drawLabels(const Geometry& g) const {
        struct Label {
            double time;
            int col;
            std::string text;
            uint32_t color;
            std::string extra;
            uint32_t extraColor = 0;
        };
        std::vector<Label> labels;

        for (auto it = firstVisible(myNotes, g.begin);
             it != myNotes.end() && it->time <= g.end; ++it) {
            std::string text, extra;
            uint32_t extraColor = 0;
            if (it->judge == J_MISS) {
                text = it->heldMiss ? "MISS (held)" : "MISS";
            } else {
                text = std::format("{:+.0f}", it->offset * 1000.0);
                if (it->earlyJudge >= 0) {
                    extra = std::format("fixed from {} {:+.0f}",
                                        kJudgeNames[it->earlyJudge],
                                        it->earlyOffset * 1000.0);
                    extraColor = kJudgeColors[it->earlyJudge];
                }
            }
            labels.push_back({it->time, it->col, text, kJudgeColors[it->judge],
                              extra, extraColor});
        }
        for (auto it = firstVisible(myPresses, g.begin);
             it != myPresses.end() && it->time <= g.end; ++it) {
            if (it->kind == PRESS_STRAY && myShowPresses) {
                labels.push_back({it->time, it->col, "stray", kStrayColor});
            }
        }
        for (auto it = firstVisible(myLetGos, g.begin);
             it != myLetGos.end() && it->time <= g.end; ++it) {
            labels.push_back(
                {it->time, it->col, "LET GO", kJudgeColors[J_MISS]});
        }
        for (auto it = firstVisible(myMines, g.begin);
             it != myMines.end() && it->time <= g.end; ++it) {
            labels.push_back({it->time, it->col, "MINE", kJudgeColors[J_MISS]});
        }
        std::stable_sort(
            labels.begin(), labels.end(), [](const Label& a, const Label& b) {
                return a.time != b.time ? a.time < b.time : a.col < b.col;
            });

        static const char* const kArrows[4] = {"\xE2\x86\x90", "\xE2\x86\x93",
                                               "\xE2\x86\x91", "\xE2\x86\x92"};
        const bool arrows = g.numCols == 4 || g.numCols == 8;

        TextStyle style;
        style.fontSize = gView->getZoomLevel() >= 4 ? 11 : 10;
        const int left =
            gView->getNotefieldCoords().xr + gSystem->applyScaleFactor(6);
        const int minGap = style.fontSize + 1;
        int lastY = -100000;

        for (size_t i = 0; i < labels.size();) {
            size_t j = i;
            while (j < labels.size() &&
                   std::abs(labels[j].time - labels[i].time) < 1e-4) {
                ++j;
            }
            int y = gView->timeToY(labels[i].time);
            if (std::abs(y - lastY) >= minGap) {
                lastY = y;
                int x = left;
                for (size_t k = i; k < j; ++k) {
                    std::string text = labels[k].text;
                    if (j - i > 1 && arrows) {
                        text = std::string(kArrows[labels[k].col % 4]) + text;
                    }
                    style.textColor = labels[k].color;
                    Text::arrange(Text::ML, style, text.c_str());
                    Text::draw(vec2i{x, y});
                    x += Text::getSize().x + 8;
                    if (!labels[k].extra.empty()) {
                        style.textColor = labels[k].extraColor;
                        Text::arrange(Text::ML, style, labels[k].extra.c_str());
                        Text::draw(vec2i{x - 4, y});
                        x += Text::getSize().x + 4;
                    }
                }
            }
            i = j;
        }
    }

    void drawSummary(const std::string& status) const {
        std::string text = "{tc:888}REPLAY{tc}  " + myFile.get("Title");
        text += "\n{tc:AAA}" + StripEnumPrefix(myFile.get("Difficulty")) + " " +
                myFile.get("Meter") + "  " + myFile.get("Player") + " " +
                myFile.get("Profile") + std::format("  {:.2f}x", myFile.rate) +
                "{tc}";
        text += "\n{tc:777}" + myFile.get("Date") + "{tc}";
        if (myFile.failed) text += "  {tc:FF3030}FAILED{tc}";
        if (myFile.autoplay) text += "  {tc:FF8040}autoplay{tc}";
        if (myFile.nextScreen == "ScreenGameplay") {
            text += "  {tc:FF8040}restarted{tc}";
        } else if (!myFile.nextScreen.empty() &&
                   myFile.nextScreen.rfind("ScreenEvaluation", 0) != 0 &&
                   myFile.nextScreen != "NextCourseSong") {
            text += "  {tc:FF8040}quit{tc}";
        }

        if (!status.empty()) {
            text += "\n\n{tc:FF8040}" + status + "{tc}";
        } else {
            if (!myWarning.empty()) {
                text += "\n\n{tc:FF8040}" + myWarning + "{tc}";
            }
            text += "\n";
            for (int j = 0; j < NUM_JUDGES; ++j) {
                if (myFile.faPlusMode && j == J_W5) continue;
                text += std::format("\n{}{:<11}{{tc}} {}", kJudgeMarkup[j],
                                    kJudgeNames[j], myStats.judges[j]);
            }
            int holds =
                myStats.holdsHeld + myStats.holdsLetGo + myStats.holdsMissed;
            text +=
                std::format("\n\nHolds       {}/{}", myStats.holdsHeld, holds);
            if (myStats.holdsLetGo > 0) {
                text += std::format("  {{tc:FF3030}}let go {}{{tc}}",
                                    myStats.holdsLetGo);
            }
            text += std::format("\nMines hit   {}", myStats.minesHit);
            text += std::format("\n{{tc:FF40C8}}Stray taps{{tc}}  {}",
                                myStats.strays);
            if (myStats.earlyHits > 0) {
                text += std::format(
                    "\n{{tc:B45CFF}}Early hits{{tc}}  {}  "
                    "{{tc:21CCE8}}fixed {}{{tc}}",
                    myStats.earlyHits, myStats.rescored);
            }
            if (myStats.numOffsets > 0) {
                text += std::format("\nMean {:+.1f} ms   SD {:.1f} ms",
                                    myStats.meanMs, myStats.sdMs);
            }
            if (myStats.unjudged > 0) {
                text += std::format("\n{{tc:888}}Not judged  {}{{tc}}",
                                    myStats.unjudged);
            }
            std::string prev =
                gShortcuts->getNotation(Action::REPLAY_PREVIOUS_ISSUE, false);
            std::string next =
                gShortcuts->getNotation(Action::REPLAY_NEXT_ISSUE, false);
            if (!prev.empty() && !next.empty()) {
                text += std::format(
                    "\n\n{{tc:777}}{} / {}: previous / next issue{{tc}}", prev,
                    next);
            } else if (!prev.empty() || !next.empty()) {
                text += std::format("\n\n{{tc:777}}{}: {} issue{{tc}}",
                                    prev.empty() ? next : prev,
                                    prev.empty() ? "next" : "previous");
            }
        }

        TextStyle style;
        style.textFlags = Text::MARKUP;
        Text::arrange(Text::TL, style, text.c_str());
        vec2i size = Text::getSize();

        recti view = gView->getRect();
        int pad = gSystem->applyScaleFactor(8);
        int x = view.x + pad;
        int y = view.y + std::max(pad * 4, (view.h - size.y) / 2);
        Draw::fill({x - pad / 2, y - pad / 2, size.x + pad, size.y + pad},
                   Color32(0, 160));
        Text::draw(vec2i{x, y});
    }

    // ================================================================================================
    // Timeline.
    //
    // A strip left of the minimap that covers the whole chart with the same
    // vertical scale as the minimap: every judged note as a dot at its offset
    // (early on the left, late on the right), a running mean, misses and
    // dropped holds as lines across, and stray taps in a lane on the left.

    // The plot area, matching the minimap's chart area vertically.
    static recti timelineRect() {
        vec2i size = gSystem->getWindowSize();
        int minimapW = gSystem->applyScaleFactor(24);
        int w = gSystem->applyScaleFactor(110);
        int x = size.x - 8 - minimapW - gSystem->applyScaleFactor(6) - w;
        return {x, 8 + 16, w, size.y - 16 - 32};
    }

    static recti timelineFrame() {
        recti r = timelineRect();
        return {r.x - 2, r.y - 16, r.w + 4, r.h + 32};
    }

    // The chart range shown by the strip, as view offsets (same as the
    // minimap).
    static void timelineRange(double& begin, double& end) {
        int endRow = gSimfile->getEndRow();
        if (gView->isTimeBased()) {
            begin = gTempo->rowToTime(0);
            end = gTempo->rowToTime(endRow);
        } else {
            begin = 0.0;
            end = static_cast<double>(endRow);
        }
    }

    static int timelineY(const recti& r, double ofs, double begin, double end) {
        double t = (ofs - begin) / (end - begin);
        if (gView->hasReverseScroll()) t = 1.0 - t;
        return r.y + static_cast<int>(t * r.h + 0.5);
    }

    static void seekTimeline(int y) {
        recti r = timelineRect();
        double begin = 0.0, end = 0.0;
        timelineRange(begin, end);
        if (end <= begin || r.h <= 0) return;
        double t = std::clamp(
            static_cast<double>(y - r.y) / static_cast<double>(r.h), 0.0, 1.0);
        if (gView->hasReverseScroll()) t = 1.0 - t;
        gView->setCursorOffset(begin + (end - begin) * t);
    }

    void drawTimeline() const {
        recti r = timelineRect();
        double begin = 0.0, end = 0.0;
        timelineRange(begin, end);
        if (r.w < 24 || r.h < 48 || end <= begin) return;

        const int line = std::max(1, gSystem->applyScaleFactor(1));
        const int mark = std::max(2, gSystem->applyScaleFactor(2));
        const int lane = gSystem->applyScaleFactor(6);
        const int plotL = r.x + lane + 2, plotR = r.x + r.w;
        const int center = (plotL + plotR) / 2, half = (plotR - plotL) / 2;

        // The widest window spans the whole plot.
        double range = 0.0;
        for (double w : myFile.window) range = std::max(range, w);
        auto xOf = [&](double offset) {
            double f = std::clamp(offset / range, -1.0, 1.0);
            return center + static_cast<int>(f * half);
        };
        auto yOf = [&](double time) {
            return timelineY(r, gView->timeToOffset(time), begin, end);
        };

        recti frame = timelineFrame();
        Draw::fill(frame, Color32(0, 180));
        Renderer::pushScissorRect(frame);

        Renderer::resetColor();
        Renderer::bindShader(Renderer::SH_COLOR);
        auto batch = Renderer::batchC();

        drawBox(&batch, frame, 1, Color32(128, 255));
        Draw::fill(&batch, {r.x, r.y, lane, r.h}, Color32(40, 255));

        // The part of the chart that is on screen.
        int vy0 = timelineY(r, gView->yToOffset(0), begin, end);
        int vy1 =
            timelineY(r, gView->yToOffset(gView->getHeight()), begin, end);
        if (vy0 > vy1) std::swap(vy0, vy1);
        Draw::fill(&batch, {r.x, vy0, r.w, std::max(1, vy1 - vy0)},
                   kViewRangeColor);

        // Timing window edges, colored by the judgment inside them.
        for (int k = 0; k < 5; ++k) {
            double w =
                myFile.faPlusMode ? myFile.window[k + 1] : myFile.window[k];
            if (w >= range) break;
            uint32_t color = Color32a(kJudgeColors[J_W0 + k], 70);
            Draw::fill(&batch, {xOf(-w), r.y, 1, r.h}, color);
            Draw::fill(&batch, {xOf(w), r.y, 1, r.h}, color);
        }
        Draw::fill(&batch, {center, r.y, 1, r.h}, Color32(255, 90));

        // Judged notes, and a running mean of the last hits.
        const int kMeanCount = 24;
        double sum = 0.0;
        std::vector<double> recent;
        for (auto& n : myNotes) {
            if (n.judge == J_MISS) continue;
            int y = yOf(n.time);
            if (n.earlyJudge >= 0) {
                int xe = xOf(n.earlyOffset), xf = xOf(n.offset);
                uint32_t color = kJudgeColors[n.earlyJudge];
                Draw::fill(&batch, {std::min(xe, xf), y, std::abs(xf - xe), 1},
                           Color32a(color, 200));
                Draw::fill(&batch, {xe - mark / 2, y - mark / 2, mark, mark},
                           color);
            }
            Draw::fill(&batch,
                       {xOf(n.offset) - mark / 2, y - mark / 2, mark, mark},
                       Color32a(kJudgeColors[n.judge], 220));
            recent.push_back(n.offset);
            sum += n.offset;
            if (static_cast<int>(recent.size()) > kMeanCount) {
                sum -= recent[recent.size() - kMeanCount - 1];
            }
            int count = std::min(static_cast<int>(recent.size()), kMeanCount);
            Draw::fill(&batch, {xOf(sum / count) - 1, y, 2, mark},
                       Color32(255, 170));
        }

        // Stray taps, in the lane on the left.
        for (auto& p : myPresses) {
            if (p.kind != PRESS_STRAY) continue;
            Draw::fill(&batch, {r.x, yOf(p.time), lane, line}, kStrayColor);
        }

        // Misses, dropped holds and hit mines across the plot.
        for (auto& n : myNotes) {
            if (n.judge != J_MISS) continue;
            Draw::fill(&batch, {plotL, yOf(n.time), plotR - plotL, mark},
                       kJudgeColors[J_MISS]);
        }
        for (auto& m : myLetGos) {
            Draw::fill(&batch, {plotL, yOf(m.time), plotR - plotL, mark},
                       kLetGoColor);
        }
        for (auto& m : myMines) {
            Draw::fill(&batch, {plotR - lane * 2, yOf(m.time), lane * 2, mark},
                       kJudgeColors[J_MISS]);
        }
        batch.flush();

        TextStyle style;
        style.fontSize = 9;
        style.textColor = Color32(150, 255);
        Text::arrange(Text::ML, style, "early");
        Text::draw(vec2i{plotL, frame.y + 8});
        Text::arrange(Text::MR, style, "late");
        Text::draw(vec2i{plotR, frame.y + 8});
        std::string scale = std::format("{:.0f} ms", range * 1000.0);
        Text::arrange(Text::MC, style, scale.c_str());
        Text::draw(vec2i{center, frame.y + frame.h - 8});

        Renderer::popScissorRect();
    }
};

};  // anonymous namespace

void DrawReplayBelowNotes(const ReplayFile& file, const ReplayResult& result,
                          const ReplayLayers& layers) {
    Drawer(file, result, layers).drawBelowNotes();
}

void DrawReplayAboveNotes(const ReplayFile& file, const ReplayResult& result,
                          const ReplayLayers& layers) {
    Drawer(file, result, layers).drawAboveNotes();
}

void DrawReplaySummary(const ReplayFile& file, const ReplayResult& result,
                       const ReplayLayers& layers, const std::string& status) {
    Drawer(file, result, layers).drawSummary(status);
}

void DrawReplayTimeline(const ReplayFile& file, const ReplayResult& result,
                        const ReplayLayers& layers) {
    Drawer(file, result, layers).drawTimeline();
}

recti ReplayTimelineFrame() { return Drawer::timelineFrame(); }

void SeekReplayTimeline(int y) { Drawer::seekTimeline(y); }

};  // namespace Vortex
