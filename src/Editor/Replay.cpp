#include <Editor/Replay.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <Core/StringUtils.h>
#include <Core/Utils.h>
#include <Core/Xmr.h>

#include <System/File.h>
#include <System/System.h>

#include <Simfile/Chart.h>
#include <Simfile/Notes.h>
#include <Simfile/Simfile.h>

#include <Managers/ChartMan.h>
#include <Managers/NoteMan.h>
#include <Managers/SimfileMan.h>
#include <Managers/StyleMan.h>

#include <Editor/Common.h>
#include <Editor/Editor.h>
#include <Editor/Menubar.h>
#include <Editor/Music.h>
#include <Editor/ReplayDraw.h>
#include <Editor/ReplayFile.h>
#include <Editor/ReplayLocate.h>
#include <Editor/ReplayMatch.h>
#include <Editor/ReplayVideo.h>
#include <Editor/TextOverlay.h>
#include <Editor/View.h>

namespace Vortex {

namespace {

const char* const kReplayExtension = ".avreplay";

/// Kept next to a replay: the recording of the play and where the song
/// starts in it.
const char* const kVideoLinkExtension = ".avvideo";

// ================================================================================================
// ReplayImpl.
//
// Opening and closing replays and keeping them on the chart they were played
// on. Reading the file is in ReplayFile, finding the simfile in ReplayLocate,
// matching the replay to the chart in ReplayMatch, drawing in ReplayDraw and
// finding the play in a recording and showing it in ReplayVideo.

struct ReplayImpl : public Replay {
    ReplayFile myFile;
    fs::path myPath;
    bool myIsOpen = false;
    std::string myStatus;
    ReplayResult myResult;
    ReplayLibrary myLibrary;
    ReplayLayers myLayers;
    bool myIsDragging = false;

    /// The simfile and chart the replay is shown on, see OpenSimfileId. The
    /// chart is null while the chart that is open is not the played one.
    std::string myBoundSimfile;
    const Chart* myBoundChart = nullptr;

    /// The music speed before the replay changed it, and the speed it set.
    int mySpeedBefore = 100;
    int myReplaySpeed = 100;

    /// The recording of the play shown in mpv, and the recording seconds at
    /// music time 0.
    VideoPlayer myVideo;
    fs::path myVideoPath;
    double myVideoOffset = 0.0;
    std::string myMpvPath = "mpv";
    std::string myMpvArgs;

    /// Finding the play in a recording, on another thread.
    std::thread myVideoJob;
    std::atomic<bool> myVideoJobDone = false;
    std::atomic<bool> myVideoCancel = false;
    fs::path myVideoJobPath;
    RecordingSync myVideoJobResult;

    ~ReplayImpl() { stopVideo(); }

    // ================================================================================================
    // ReplayImpl :: settings.

    void loadSettings(XmrNode& settings) {
        XmrNode* node = settings.child("replay");
        if (!node) return;
        node->get("showPresses", &myLayers.presses);
        node->get("showHeldPanels", &myLayers.heldPanels);
        node->get("showLabels", &myLayers.labels);
        node->get("showPanel", &myLayers.panel);
        node->get("showTimeline", &myLayers.timeline);
        if (const char* mpv = node->get("mpvPath")) myMpvPath = mpv;
        if (const char* args = node->get("mpvArgs")) myMpvArgs = args;
        myLibrary.load(*node);
    }

    void saveSettings(XmrNode& settings) override {
        XmrNode* node = settings.addChild("replay");
        node->addAttrib("showPresses", myLayers.presses);
        node->addAttrib("showHeldPanels", myLayers.heldPanels);
        node->addAttrib("showLabels", myLayers.labels);
        node->addAttrib("showPanel", myLayers.panel);
        node->addAttrib("showTimeline", myLayers.timeline);
        node->addAttrib("mpvPath", myMpvPath.c_str());
        node->addAttrib("mpvArgs", myMpvArgs.c_str());
        myLibrary.save(*node);
    }

    // ================================================================================================
    // ReplayImpl :: opening and closing.

    bool openReplay() override {
        static SDL_DialogFileFilter filters[] = {
            {"ArrowReplay (*.avreplay)", "avreplay"},
            {"All files", "*"},
        };
        fs::path path =
            gSystem->openFileDlg("Open replay", filters, 2, fs::path());
        if (path.empty()) return false;
        return openReplay(path);
    }

    bool openReplay(const fs::path& path) override {
        ReplayFile file;
        std::string error;
        if (!ParseReplay(path, file, error)) {
            HudError("Could not open replay: %s.", error.c_str());
            return false;
        }
        if (file.version > kReplayVersion) {
            HudWarning(
                "The replay has a newer format (%d) than this ArrowVortex "
                "reads (%d), update it if the replay looks wrong.",
                file.version, kReplayVersion);
        }

        bool useCurrent = false, remember = false;
        fs::path simfile =
            myLibrary.locateSimfile(path, file, useCurrent, remember);
        if (!useCurrent) {
            // No simfile means the user cancelled choosing one.
            if (simfile.empty()) {
                HudNote("No simfile was chosen, the replay was not opened.");
                return false;
            }
            if (!gEditor->openSimfile(simfile)) return false;
            myLibrary.rememberSongRoot(simfile, file);
            if (remember) myLibrary.rememberSongFile(file, simfile);
        }

        if (!myIsOpen) mySpeedBefore = gMusic->getSpeed();
        stopVideo();
        myFile = std::move(file);
        myPath = path;
        myIsOpen = true;

        if (!selectChart()) {
            HudWarning("The chart \"%s %s\" was not found in the simfile.",
                       StripEnumPrefix(myFile.get("Difficulty")).c_str(),
                       myFile.get("Meter").c_str());
        }
        myBoundSimfile = OpenSimfileId();

        gMusic->setSpeed(static_cast<int>(std::lround(myFile.rate * 100.0)));
        myReplaySpeed = gMusic->getSpeed();

        rebuild();
        gMenubar->update(Menubar::REPLAY);
        if (!myResult.warning.empty()) {
            HudWarning("%s", myResult.warning.c_str());
        }
        HudInfo("Replay: %s, %s.", myFile.get("Title").c_str(),
                myFile.get("Date").c_str());
        if (loadVideoLink()) startVideo();
        return true;
    }

    static std::string OpenSimfileId() {
        if (gSimfile->isClosed()) return {};
        return gSimfile->getDir() + "/" + gSimfile->getFile();
    }

    void closeReplay() override {
        // Undo the speed change, unless the speed was changed since.
        if (myIsOpen && gMusic->getSpeed() == myReplaySpeed) {
            gMusic->setSpeed(mySpeedBefore);
        }
        stopVideo();
        myIsOpen = false;
        myBoundSimfile.clear();
        myBoundChart = nullptr;
        myFile = ReplayFile();
        myPath.clear();
        myResult = ReplayResult();
        gMenubar->update(Menubar::REPLAY);
    }

    bool isOpen() const override { return myIsOpen; }

    // Opens the chart that matches the steps type and difficulty of the
    // replay. Returns false if none was found.
    bool selectChart() {
        int best = -1, bestScore = 0;
        for (int i = 0; i < gSimfile->getNumCharts(); ++i) {
            int score = ScoreReplayChart(myFile, *gSimfile->getChart(i));
            if (score > bestScore) best = i, bestScore = score;
        }
        if (best < 0) return false;
        gSimfile->openChart(best);
        return true;
    }

    // ================================================================================================
    // ReplayImpl :: matching the replay against the chart.

    void onChanges(int changes) override {
        if (!myIsOpen) return;

        // Opening another simfile, or another chart while the replay is
        // shown, ends the replay. While the played chart is not open, any
        // chart can be opened to find it.
        if ((changes & (VCM_FILE_CHANGED | VCM_CHART_CHANGED)) &&
            (OpenSimfileId() != myBoundSimfile ||
             (myBoundChart && gChart->get() != myBoundChart))) {
            closeReplay();
            HudNote("Replay closed.");
            return;
        }

        if (changes &
            (VCM_CHART_CHANGED | VCM_CHART_PROPERTIES_CHANGED |
             VCM_NOTES_CHANGED | VCM_TEMPO_CHANGED | VCM_FILE_CHANGED)) {
            rebuild();
        }
    }

    // Returns true if the chart that is open is the one the replay was played
    // on.
    bool chartMatches() {
        const Chart* chart = gChart->get();
        return chart && ScoreReplayChart(myFile, *chart) > 0;
    }

    void rebuild() {
        myResult = ReplayResult();
        myStatus.clear();
        if (!myIsOpen) return;
        if (gChart->isClosed() || !chartMatches()) {
            myBoundChart = nullptr;
            myStatus = "Open the " + StripEnumPrefix(myFile.get("Difficulty")) +
                       " " + myFile.get("Meter") + " chart to see the replay.";
            return;
        }
        myBoundChart = gChart->get();
        const int numCols = gStyle->getNumCols();
        if (numCols != myFile.columns) {
            myStatus = "The replay has a different number of columns.";
            return;
        }

        // The notes that can be judged and the mines, the engine does not
        // judge warped and fake notes.
        std::vector<ReplayChartNote> chart;
        for (auto& note : *gNotes) {
            if (note.isWarped || note.isFake || note.type == NOTE_FAKE) {
                continue;
            }
            ReplayChartNote n;
            n.time = note.time;
            n.endTime = note.endtime;
            n.col = static_cast<int>(note.col);
            n.mine = note.type == NOTE_MINE;
            n.lift = note.type == NOTE_LIFT;
            n.hold = note.endrow > note.row;
            n.roll = note.type == NOTE_ROLL;
            chart.push_back(n);
        }
        myResult = MatchReplay(myFile, chart, numCols);
    }

    // ================================================================================================
    // ReplayImpl :: navigation.

    void jumpToIssue(bool forward) override {
        const std::vector<double>& issues = myResult.issues;
        if (!myIsOpen || issues.empty()) {
            HudNote("No replay issues to jump to.");
            return;
        }
        const double cursor = gView->getCursorTime();
        const double eps = 0.002;
        if (forward) {
            auto it =
                std::upper_bound(issues.begin(), issues.end(), cursor + eps);
            if (it == issues.end()) {
                HudNote("No more issues after the cursor.");
                return;
            }
            gView->setCursorTime(*it);
        } else {
            auto it =
                std::lower_bound(issues.begin(), issues.end(), cursor - eps);
            if (it == issues.begin()) {
                HudNote("No more issues before the cursor.");
                return;
            }
            gView->setCursorTime(*(it - 1));
        }
    }

    // ================================================================================================
    // ReplayImpl :: drawing.

    bool visible() const {
        return myIsOpen && myStatus.empty() && gChart->isOpen() &&
               gStyle->getNumCols() == myFile.columns;
    }

    bool timelineActive() const {
        return myLayers.timeline && visible() && !gTextOverlay->isOpen();
    }

    void drawBelowNotes() override {
        if (!visible() || !myLayers.heldPanels) return;
        DrawReplayBelowNotes(myFile, myResult, myLayers);
    }

    void drawAboveNotes() override {
        if (!visible()) return;
        DrawReplayAboveNotes(myFile, myResult, myLayers);
    }

    void drawPanel() override {
        if (!myIsOpen) return;
        if (myLayers.panel) {
            DrawReplaySummary(myFile, myResult, myLayers, myStatus);
        }
        if (timelineActive()) DrawReplayTimeline(myFile, myResult, myLayers);
    }

    void onMousePress(MousePress& evt) override {
        if (evt.button != Mouse::LMB || evt.handled || !timelineActive()) {
            return;
        }
        if (IsInside(ReplayTimelineFrame(), evt.x, evt.y)) {
            SeekReplayTimeline(evt.y);
            myIsDragging = true;
            evt.handled = true;
        }
    }

    void onMouseRelease(MouseRelease& evt) override {
        if (evt.button == Mouse::LMB && myIsDragging) {
            SeekReplayTimeline(evt.y);
            myIsDragging = false;
        }
    }

    void tick() override {
        if (myIsDragging) SeekReplayTimeline(gSystem->getMousePos().y);
        if (myVideoJob.joinable() && myVideoJobDone) finishVideoSync();
        if (myVideo.isOpen()) {
            // The editor plays at the replay rate, the recording at 1x.
            const double rate = myFile.rate;
            myVideo.show(myVideoOffset + gView->getCursorTime() / rate,
                         !gMusic->isPaused(),
                         gMusic->getSpeed() / (100.0 * rate));
        }
    }

    // ================================================================================================
    // ReplayImpl :: video.

    void openVideo() override {
        if (!myIsOpen) {
            HudNote("Open a replay first.");
            return;
        }
        if (myVideoJob.joinable()) {
            HudNote("Still looking for the play in the recording.");
            return;
        }
        if (!gMusic->getSamples().isCompleted() ||
            gMusic->getSamples().getNumFrames() == 0) {
            HudNote("Wait for the music to finish loading.");
            return;
        }
        static SDL_DialogFileFilter filters[] = {
            {"Recordings (*.mkv, *.mp4, *.mov, *.flv, *.ts, *.webm)",
             "mkv;mp4;mov;flv;ts;webm"},
            {"All files", "*"},
        };
        fs::path path = gSystem->openFileDlg("Open the recording of the play",
                                             filters, 2, myVideoPath);
        if (path.empty()) return;
        if (path == myVideoPath) {
            // Keeps the offset, including nudges.
            startVideo();
            return;
        }

        const Simfile* sim = gSimfile->get();
        VideoSyncInput input =
            PrepareVideoSync(myFile, gMusic->getSamples(), sim->previewStart,
                             sim->previewLength, path);
        myVideoJobPath = path;
        myVideoJobDone = false;
        myVideoCancel = false;
        myVideoJob = std::thread([this, input = std::move(input), path] {
            myVideoJobResult = SyncVideo(input, path, &myVideoCancel);
            myVideoJobDone = true;
        });
        HudInfo("Looking for the play in the recording...");
    }

    void finishVideoSync() {
        myVideoJob.join();
        const RecordingSync& sync = myVideoJobResult;
        if (!sync.found) {
            HudError("Could not find the play in the recording: %s.",
                     sync.error.c_str());
            return;
        }
        myVideoPath = myVideoJobPath;
        myVideoOffset = sync.offset;
        saveVideoLink();
        HudInfo(
            "The song starts at %s in the recording (%d of %d parts of "
            "the song match, %.0f%%).",
            Str::formatTime(sync.offset).c_str(), sync.votes, sync.parts,
            sync.score * 100.0);
        startVideo();
    }

    void startVideo() {
        std::string error;
        if (!myVideo.open(myMpvPath, myMpvArgs, myVideoPath, error)) {
            HudError(
                "Could not start mpv (%s), set mpvPath in the replay settings.",
                error.c_str());
        }
    }

    void stopVideo() {
        if (myVideoJob.joinable()) {
            myVideoCancel = true;
            myVideoJob.join();
        }
        myVideo.close();
        myVideoPath.clear();
        myVideoOffset = 0.0;
    }

    void nudgeVideo(double seconds) override {
        if (myVideoPath.empty()) {
            HudNote("Open the recording of the play first.");
            return;
        }
        myVideoOffset -= seconds;
        saveVideoLink();
        HudInfo("Video %s by %.0f ms.", seconds > 0.0 ? "later" : "earlier",
                std::abs(seconds) * 1000.0);
    }

    fs::path videoLinkPath() const {
        fs::path path = myPath;
        return path.replace_extension(kVideoLinkExtension);
    }

    void saveVideoLink() {
        std::ofstream out(videoLinkPath(), std::ios::binary);
        out << "Video\t" << pathToUtf8(myVideoPath) << "\n";
        out << std::format("Offset\t{:.6f}\n", myVideoOffset);
        if (!out.good()) HudWarning("Could not save the video offset.");
    }

    bool loadVideoLink() {
        std::ifstream in(videoLinkPath(), std::ios::binary);
        std::string line, video;
        double offset = 0.0;
        bool hasOffset = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("Video\t")) video = line.substr(6);
            if (line.starts_with("Offset\t")) {
                hasOffset = Str::read(line.substr(7), &offset);
            }
        }
        if (video.empty() || !hasOffset) return false;
        fs::path path = utf8ToPath(video);
        std::error_code error;
        if (!fs::exists(path, error)) {
            HudNote("The recording of this replay was not found: %s.",
                    video.c_str());
            return false;
        }
        myVideoPath = path;
        myVideoOffset = offset;
        return true;
    }

    // ================================================================================================
    // ReplayImpl :: toggles.

    void toggleShowPresses() override {
        myLayers.presses = !myLayers.presses;
        gMenubar->update(Menubar::REPLAY);
    }

    void toggleShowHeldPanels() override {
        myLayers.heldPanels = !myLayers.heldPanels;
        gMenubar->update(Menubar::REPLAY);
    }

    void toggleShowLabels() override {
        myLayers.labels = !myLayers.labels;
        gMenubar->update(Menubar::REPLAY);
    }

    void toggleShowPanel() override {
        myLayers.panel = !myLayers.panel;
        gMenubar->update(Menubar::REPLAY);
    }

    void toggleShowTimeline() override {
        myLayers.timeline = !myLayers.timeline;
        gMenubar->update(Menubar::REPLAY);
    }

    bool hasShowPresses() const override { return myLayers.presses; }
    bool hasShowHeldPanels() const override { return myLayers.heldPanels; }
    bool hasShowLabels() const override { return myLayers.labels; }
    bool hasShowPanel() const override { return myLayers.panel; }
    bool hasShowTimeline() const override { return myLayers.timeline; }
};

};  // anonymous namespace

// ================================================================================================
// Replay API.

Replay* gReplay = nullptr;

void Replay::create(XmrNode& settings) {
    gReplay = new ReplayImpl;
    static_cast<ReplayImpl*>(gReplay)->loadSettings(settings);
}

void Replay::destroy() {
    delete static_cast<ReplayImpl*>(gReplay);
    gReplay = nullptr;
}

bool Replay::isReplayFile(const fs::path& path) {
    return Str::iequal(pathToUtf8(path.extension()), kReplayExtension);
}

};  // namespace Vortex
