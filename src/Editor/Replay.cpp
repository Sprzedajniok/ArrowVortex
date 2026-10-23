#include <Editor/Replay.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <Core/Draw.h>
#include <Core/StringUtils.h>
#include <Core/Text.h>
#include <Core/Utils.h>
#include <Core/Xmr.h>

#include <System/System.h>
#include <System/File.h>

#include <Simfile/Chart.h>
#include <Simfile/Notes.h>
#include <Simfile/Simfile.h>

#include <Managers/ChartMan.h>
#include <Managers/NoteMan.h>
#include <Managers/SimfileMan.h>
#include <Managers/StyleMan.h>
#include <Managers/TempoMan.h>

#include <Editor/Common.h>
#include <Editor/Editor.h>
#include <Editor/Menubar.h>
#include <Editor/Music.h>
#include <Editor/Shortcuts.h>
#include <Editor/TextOverlay.h>
#include <Editor/View.h>

namespace Vortex {

namespace {

// ================================================================================================
// Judgments and colors, as Simply Love shows them.

/// Fantastic is split into the FA+ (blue) and regular (white) window.
enum Judge { J_W0, J_W1, J_W2, J_W3, J_W4, J_W5, J_MISS, NUM_JUDGES };

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

/// What a panel press ended up doing.
enum PressKind {
    PRESS_HIT,    ///< Judged a note.
    PRESS_EARLY,  ///< Early decent/way off that a later press rescored.
    PRESS_HOLD,   ///< Re-pressed inside a hold or roll body.
    PRESS_MINE,   ///< Stepped on a mine.
    PRESS_STRAY,  ///< Did not activate anything.
};

const uint32_t kStrayColor = RGBAtoColor32(255, 64, 200, 255);
const uint32_t kHoldPressColor = RGBAtoColor32(170, 170, 170, 255);
const uint32_t kHeldPanelColor = RGBAtoColor32(255, 255, 255, 72);
const uint32_t kOutlineColor = RGBAtoColor32(0, 0, 0, 200);
const uint32_t kLetGoColor = RGBAtoColor32(255, 144, 48, 255);
const uint32_t kViewRangeColor = RGBAtoColor32(255, 204, 102, 60);

const char* const kReplayExtension = ".avreplay";

/// Newest replay format this viewer reads.
const int kReplayVersion = 1;

// ================================================================================================
// Replay file.

struct RawEvent {
    char type = 0;      ///< P, T, E, H or M (see the recorder module).
    double time = 0.0;  ///< Engine music seconds.
    int col = 0;        ///< Zero-based column.
    bool down = false;
    std::string score;  ///< TapNoteScore or HoldNoteScore, without prefix.
    double offset = 0.0;
    std::string earlyScore;
    double earlyOffset = 0.0;
    bool heldMiss = false;
    std::string noteType;
};

struct ReplayFile {
    std::map<std::string, std::string> header;
    std::vector<RawEvent> events;
    double endTime = 0.0;
    bool failed = false;
    std::string nextScreen;  ///< Screen that followed gameplay.

    int version = 0;
    double rate = 1.0;
    double globalOffset = 0.0;
    int columns = 0;
    bool faPlusMode = false;
    bool autoplay = false;

    /// Window sizes in seconds, as used by the engine (W0 is the FA+ window).
    double window[6] = {0.015, 0.023, 0.0445, 0.1035, 0.1365, 0.1815};
    double holdWindow = 0.3215;
    double rollWindow = 0.3515;
    double mineWindow = 0.0715;

    std::string get(const char* key) const {
        auto it = header.find(key);
        return it == header.end() ? std::string() : it->second;
    }
};

static std::string StripEnumPrefix(const std::string& s) {
    auto pos = s.find('_');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

static bool ParseReplay(const fs::path& path, ReplayFile& out,
                        std::string& error) {
    bool success = false;
    auto lines = File::getLines(path, &success);
    if (!success) {
        error = "could not read the file";
        return false;
    }
    if (lines.empty() || lines[0].rfind("ArrowReplay", 0) != 0) {
        error = "not an ArrowReplay file";
        return false;
    }

    bool inEvents = false;
    for (auto& line : lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!inEvents) {
            if (line == "---") {
                inEvents = true;
                continue;
            }
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            out.header[line.substr(0, tab)] = line.substr(tab + 1);
            continue;
        }

        auto f = Str::split(line, "\t", false, false);
        if (f.size() < 2) continue;
        if (f[0] == "END") {
            out.endTime = Str::readDouble(f[1]);
            out.failed = f.size() > 2 && f[2] == "1";
            if (f.size() > 3) out.nextScreen = f[3];
            continue;
        }
        if (f[0].size() != 1 || f.size() < 3) continue;

        RawEvent e;
        e.type = f[0][0];
        e.time = Str::readDouble(f[1]);
        e.col = Str::readInt(f[2]) - 1;
        switch (e.type) {
            case 'P':
                if (f.size() < 4) continue;
                e.down = f[3] == "1";
                break;
            case 'T':
                if (f.size() < 9) continue;
                e.score = f[3];
                e.offset = Str::readDouble(f[4]);
                e.earlyScore = f[5];
                e.earlyOffset = Str::readDouble(f[6]);
                e.heldMiss = f[7] == "1";
                e.noteType = f[8];
                break;
            case 'E':
                if (f.size() < 5) continue;
                e.score = f[3];
                e.offset = Str::readDouble(f[4]);
                break;
            case 'H':
            case 'M':
                if (f.size() < 4) continue;
                e.score = f[3];
                break;
            default:
                continue;
        }
        out.events.push_back(e);
    }

    out.rate = Str::readDouble(out.get("Rate"), 1.0);
    if (out.rate <= 0.0) out.rate = 1.0;
    out.globalOffset = Str::readDouble(out.get("GlobalOffset"), 0.0);
    out.columns = Str::readInt(out.get("Columns"), 0);
    out.faPlusMode = out.get("GameMode") == "FA+";
    out.version = Str::readInt(out.get("ArrowReplay"), 0);
    out.autoplay = out.get("Autoplay") == "1";

    for (auto& item : Str::split(out.get("Windows"), " ")) {
        auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string key = item.substr(0, eq);
        double value = Str::readDouble(item.substr(eq + 1), -1.0);
        if (value < 0.0) continue;
        if (key.size() == 2 && key[0] == 'W' && key[1] >= '0' &&
            key[1] <= '5') {
            out.window[key[1] - '0'] = value;
        } else if (key == "Hold") {
            out.holdWindow = value;
        } else if (key == "Roll") {
            out.rollWindow = value;
        } else if (key == "Mine") {
            out.mineWindow = value;
        }
    }

    if (out.columns <= 0) {
        error = "the replay has no column count";
        return false;
    }
    return true;
}

// ================================================================================================
// Finding the simfile of a replay.

/// Settings values cannot hold some characters, so paths are percent-encoded.
static std::string EscapeSetting(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c != 0 && std::strchr("%{};,=|>\r\n", c)) {
            out += std::format("%{:02X}", static_cast<unsigned char>(c));
        } else {
            out += c;
        }
    }
    return out;
}

static std::string UnescapeSetting(const std::string& s) {
    auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 &&
            hex(s[i + 2]) >= 0) {
            out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

/// Lowercase letters and digits only, so "Into You (N-Vision Remix)" and
/// "into you n vision remix" compare equal. Non-ASCII characters are kept.
static std::string NormalizeName(const std::string& s) {
    std::string out;
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || std::isalnum(u)) {
            out += static_cast<char>(u < 0x80 ? std::tolower(u) : u);
        }
    }
    return out;
}

struct SongInfo {
    std::string title, titleTr, subtitle, artist, artistTr;
};

/// Returns the value of "#NAME:value;" in a simfile, or an empty string.
static std::string ReadTag(const std::string& text, const char* name) {
    std::string key = std::string("#") + name + ":";
    size_t pos = text.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    size_t end = text.find(';', pos);
    std::string value = text.substr(
        pos, end == std::string::npos ? std::string::npos : end - pos);
    Str::trim(value);
    return value;
}

/// Reads the title and artist tags from the start of a .sm/.ssc file.
static SongInfo ReadSongInfo(const fs::path& path) {
    SongInfo info;
    std::ifstream in(path, std::ios::binary);
    if (!in) return info;
    std::string text(32768, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(in.gcount()));
    info.title = ReadTag(text, "TITLE");
    info.titleTr = ReadTag(text, "TITLETRANSLIT");
    info.subtitle = ReadTag(text, "SUBTITLE");
    info.artist = ReadTag(text, "ARTIST");
    info.artistTr = ReadTag(text, "ARTISTTRANSLIT");
    return info;
}

/// How well the charts of a simfile match the chart of a replay: 0 if none
/// has the same steps type and difficulty, 1 if one does, more if its meter
/// (+2) and description (+1) match too.
static int ChartScore(const fs::path& path, const ReplayFile& file) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

    std::string type = StripEnumPrefix(file.get("StepsType"));
    Str::toLower(type);
    std::replace(type.begin(), type.end(), '_', '-');
    const std::string diff = StripEnumPrefix(file.get("Difficulty"));
    const std::string meter = file.get("Meter");
    const std::string desc = file.get("Description");

    int best = 0;
    auto consider = [&](std::string t, std::string d, std::string m,
                        std::string ds) {
        Str::trim(t);
        Str::trim(d);
        Str::trim(m);
        Str::trim(ds);
        if (!Str::iequal(t, type) || !Str::iequal(d, diff)) return;
        int score = 1 + (m == meter ? 2 : 0) + (ds == desc ? 1 : 0);
        best = std::max(best, score);
    };

    if (Str::iequal(pathToUtf8(path.extension()), ".ssc")) {
        const std::string key = "#NOTEDATA:";
        for (size_t pos = text.find(key); pos != std::string::npos;) {
            size_t next = text.find(key, pos + key.size());
            std::string chart =
                text.substr(pos, next == std::string::npos ? std::string::npos
                                                           : next - pos);
            consider(ReadTag(chart, "STEPSTYPE"), ReadTag(chart, "DIFFICULTY"),
                     ReadTag(chart, "METER"), ReadTag(chart, "DESCRIPTION"));
            pos = next;
        }
    } else {
        // #NOTES:type:description:difficulty:meter:radar:notes;
        const std::string key = "#NOTES:";
        for (size_t pos = text.find(key); pos != std::string::npos;
             pos = text.find(key, pos + key.size())) {
            std::vector<std::string> fields;
            size_t begin = pos + key.size();
            for (int i = 0; i < 4; ++i) {
                size_t colon = text.find(':', begin);
                if (colon == std::string::npos) break;
                fields.push_back(text.substr(begin, colon - begin));
                begin = colon + 1;
            }
            if (fields.size() == 4) {
                consider(fields[0], fields[2], fields[3], fields[1]);
            }
        }
    }
    return best;
}

/// Returns the .ssc (preferred) or .sm file in a song folder, or the file
/// with the given name if it exists.
static fs::path FindSimfileIn(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    if (!name.empty() && fs::is_regular_file(dir / utf8ToPath(name), ec)) {
        return dir / utf8ToPath(name);
    }
    fs::path best;
    fs::directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = pathToUtf8(it->path().extension());
        if (Str::iequal(ext, ".ssc")) return it->path();
        if (Str::iequal(ext, ".sm")) best = it->path();
    }
    return best;
}

/// Calls fn for every song folder (root/pack/song) until it returns true.
/// Returns the folder it stopped at, or an empty path.
template <typename Fn>
static fs::path FindSongDir(const std::vector<fs::path>& roots, Fn fn) {
    const int kMaxFolders = 50000;
    int visited = 0;
    for (auto& root : roots) {
        std::error_code ec;
        fs::directory_iterator packIt(root, ec), end;
        for (; !ec && packIt != end; packIt.increment(ec)) {
            if (!packIt->is_directory(ec)) continue;
            std::error_code ec2;
            fs::directory_iterator songIt(packIt->path(), ec2);
            for (; !ec2 && songIt != end; songIt.increment(ec2)) {
                if (++visited > kMaxFolders) return {};
                if (songIt->is_directory(ec2) && fn(songIt->path())) {
                    return songIt->path();
                }
            }
        }
    }
    return {};
}

// ================================================================================================
// Results of matching the replay against the chart. All times are chart
// times, the same clock the notes and the music use in the editor.

struct NoteMark {
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

struct PressMark {
    double time = 0.0;
    int col = 0;
    int kind = PRESS_STRAY;
    int judge = -1;
};

struct Span {
    double begin = 0.0, end = 0.0;
    int col = 0;
};

struct Mark {
    double time = 0.0;
    int col = 0;
};

struct Stats {
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

template <typename T>
static bool LessByTime(const T& a, const T& b) {
    return a.time < b.time;
}

// ================================================================================================
// ReplayImpl.

struct ReplayImpl : public Replay {
    ReplayFile myFile;
    fs::path myPath;
    bool myIsOpen = false;
    std::string myStatus;
    std::string myWarning;  ///< Signs of a different version of the chart.

    std::vector<NoteMark> myNotes;
    std::vector<PressMark> myPresses;
    std::vector<Span> myHeld;
    std::vector<Mark> myLetGos;
    std::vector<Mark> myMines;
    std::vector<double> myIssues;
    Stats myStats;

    /// Folders that contain packs, learned from simfiles that were found.
    std::vector<std::string> mySongRoots;

    /// Replay song path -> simfile, for songs found in another place than
    /// the replay says. Most recent first.
    std::vector<std::pair<std::string, std::string>> mySongFiles;

    bool myShowPresses = true;
    bool myShowHeldPanels = true;
    bool myShowLabels = true;
    bool myShowPanel = true;
    bool myShowTimeline = true;
    bool myIsDragging = false;

    /// The simfile and chart the replay is shown on, see OpenSimfileId. The
    /// chart is null while the chart that is open is not the played one.
    std::string myBoundSimfile;
    const Chart* myBoundChart = nullptr;

    /// The music speed before the replay changed it, and the speed it set.
    int mySpeedBefore = 100;
    int myReplaySpeed = 100;

    // ================================================================================================
    // ReplayImpl :: settings.

    void loadSettings(XmrNode& settings) {
        XmrNode* node = settings.child("replay");
        if (!node) return;
        node->get("showPresses", &myShowPresses);
        node->get("showHeldPanels", &myShowHeldPanels);
        node->get("showLabels", &myShowLabels);
        node->get("showPanel", &myShowPanel);
        node->get("showTimeline", &myShowTimeline);
        if (const char* roots = node->get("songRoots")) {
            for (auto& root : Str::split(roots, "|", false, true)) {
                mySongRoots.push_back(UnescapeSetting(root));
            }
        }
        if (const char* files = node->get("songFiles")) {
            for (auto& entry : Str::split(files, "|", false, true)) {
                auto sep = entry.find('>');
                if (sep == std::string::npos) continue;
                mySongFiles.emplace_back(
                    UnescapeSetting(entry.substr(0, sep)),
                    UnescapeSetting(entry.substr(sep + 1)));
            }
        }
    }

    void saveSettings(XmrNode& settings) override {
        XmrNode* node = settings.addChild("replay");
        node->addAttrib("showPresses", myShowPresses);
        node->addAttrib("showHeldPanels", myShowHeldPanels);
        node->addAttrib("showLabels", myShowLabels);
        node->addAttrib("showPanel", myShowPanel);
        node->addAttrib("showTimeline", myShowTimeline);
        std::vector<std::string> roots, files;
        for (auto& root : mySongRoots) roots.push_back(EscapeSetting(root));
        for (auto& [song, file] : mySongFiles) {
            files.push_back(EscapeSetting(song) + ">" + EscapeSetting(file));
        }
        node->addAttrib("songRoots", Str::join(roots, "|").c_str());
        node->addAttrib("songFiles", Str::join(files, "|").c_str());
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
        fs::path simfile = locateSimfile(path, file, useCurrent, remember);
        if (!useCurrent) {
            // No simfile means the user cancelled choosing one.
            if (simfile.empty()) {
                HudNote("No simfile was chosen, the replay was not opened.");
                return false;
            }
            if (!gEditor->openSimfile(simfile)) return false;
            rememberSongRoot(simfile, file);
            if (remember) rememberSongFile(SongKey(file), simfile);
        }

        if (!myIsOpen) mySpeedBefore = gMusic->getSpeed();
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
        if (!myWarning.empty()) HudWarning("%s", myWarning.c_str());
        HudInfo("Replay: %s, %s.", myFile.get("Title").c_str(),
                myFile.get("Date").c_str());
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
        myIsOpen = false;
        myBoundSimfile.clear();
        myBoundChart = nullptr;
        myFile = ReplayFile();
        myPath.clear();
        clearMarks();
        gMenubar->update(Menubar::REPLAY);
    }

    bool isOpen() const override { return myIsOpen; }

    // The replay's song path, used as the key for remembered songs.
    static std::string SongKey(const ReplayFile& file) {
        std::string key = file.get("SimfilePath");
        return key.empty() ? file.get("SongDir") : key;
    }

    // The replay's song path split into folders ("Songs", pack, song folder)
    // and the simfile name, which is empty if the replay has no simfile path.
    static std::vector<std::string> SongPathParts(const ReplayFile& file) {
        auto parts = Str::split(file.get("SimfilePath"), "/", false, true);
        if (parts.size() < 4) {
            parts = Str::split(file.get("SongDir"), "/", false, true);
            parts.emplace_back();
        }
        return parts;
    }

    // Returns true if the title and artist match the song of the replay.
    static bool IsSameSong(const SongInfo& info, const ReplayFile& file) {
        std::string title = NormalizeName(file.get("Title"));
        if (title.empty()) return false;
        std::string full =
            NormalizeName(file.get("Title") + file.get("Subtitle"));
        bool titleMatches = false;
        for (auto* t : {&info.title, &info.titleTr}) {
            std::string n = NormalizeName(*t);
            std::string ns = NormalizeName(*t + info.subtitle);
            if (!n.empty() && (n == title || n == full || ns == full)) {
                titleMatches = true;
            }
        }
        if (!titleMatches) return false;
        std::string artist = NormalizeName(file.get("Artist"));
        std::string a = NormalizeName(info.artist);
        std::string at = NormalizeName(info.artistTr);
        return artist.empty() || (a.empty() && at.empty()) || a == artist ||
               at == artist;
    }

    // Finds the simfile the replay was played on. The replay stores the
    // ITGmania path ("/Songs/Pack/Song/song.ssc"). In order, this tries: the
    // simfile that is already open (useCurrent is set), songs that were found
    // somewhere else before, the same pack and song folder in every folder
    // that holds packs, the song folder in any pack, any simfile with the same
    // title and artist, and finally asks the user. Sets remember when the song
    // was not where the replay says.
    fs::path locateSimfile(const fs::path& replayPath, const ReplayFile& file,
                           bool& useCurrent, bool& remember) {
        auto parts = SongPathParts(file);
        std::string pack, song, name;
        size_t n = parts.size();
        if (n >= 3)
            pack = parts[n - 3], song = parts[n - 2], name = parts[n - 1];

        // The simfile that is open already.
        if (gSimfile->isOpen()) {
            fs::path dir = utf8ToPath(gSimfile->getDir());
            if (dir.filename().empty()) dir = dir.parent_path();
            const Simfile* sim = gSimfile->get();
            SongInfo info = {sim->title, sim->titleTr, sim->subtitle,
                             sim->artist, sim->artistTr};
            bool sameDir =
                !song.empty() &&
                Str::iequal(pathToUtf8(dir.filename()), song) &&
                Str::iequal(pathToUtf8(dir.parent_path().filename()), pack);
            // By title only if it also has the played chart, the same song
            // is often in several packs with different charts.
            if (sameDir ||
                (IsSameSong(info, file) && OpenChartScore(file) >= 3)) {
                useCurrent = true;
                return {};
            }
        }

        // A song that was found somewhere else before.
        std::error_code ec;
        for (auto& [key, path] : mySongFiles) {
            if (key == SongKey(file) &&
                fs::is_regular_file(utf8ToPath(path), ec)) {
                return utf8ToPath(path);
            }
        }

        // Folders that might contain the pack: learned ones, the ITGmania
        // folder the replay is in, and the default places.
        std::vector<fs::path> candidates;
        for (auto& root : mySongRoots) candidates.push_back(utf8ToPath(root));
        std::string top = n >= 4 ? parts[n - 4] : std::string("Songs");
        for (fs::path p = replayPath.parent_path(); !p.empty();
             p = p.parent_path()) {
            candidates.push_back(p / utf8ToPath(top));
            candidates.push_back(p / "Songs");
            if (p == p.parent_path()) break;
        }
        if (const char* appdata = std::getenv("APPDATA")) {
            candidates.push_back(utf8ToPath(appdata) / "ITGmania" / "Songs");
        }
        // Default folder of the ITGmania installer.
        candidates.emplace_back("C:/Games/ITGmania/Songs");

        std::vector<fs::path> roots;
        for (auto& root : candidates) {
            if (!fs::is_directory(root, ec)) continue;
            if (root == root.root_path()) continue;
            bool known = false;
            for (auto& r : roots) known = known || fs::equivalent(r, root, ec);
            if (!known) roots.push_back(root);
        }

        // The same pack and song folder.
        if (!song.empty()) {
            for (auto& root : roots) {
                fs::path dir = root / utf8ToPath(pack) / utf8ToPath(song);
                if (!fs::is_directory(dir, ec)) continue;
                fs::path found = FindSimfileIn(dir, name);
                if (!found.empty()) return found;
            }
        }

        remember = true;

        // Elsewhere: the song folder in another pack, or any simfile with the
        // same title and artist.
        struct Candidate {
            fs::path simfile;
            int score = 0, chart = 0;
        };
        std::vector<Candidate> found;
        FindSongDir(roots, [&](const fs::path& d) {
            bool sameFolder =
                !song.empty() && Str::iequal(pathToUtf8(d.filename()), song);
            fs::path sim = FindSimfileIn(d, sameFolder ? name : std::string());
            if (sim.empty()) return false;
            bool sameSong = IsSameSong(ReadSongInfo(sim), file);
            if (!sameSong && !sameFolder) return false;
            int chart = ChartScore(sim, file);
            if (!sameSong && chart == 0) return false;
            int score = (sameSong ? 10 : 0) + (sameFolder ? 5 : 0) + chart * 3;
            found.push_back({sim, score, chart});
            return false;
        });
        std::stable_sort(found.begin(), found.end(),
                         [](const Candidate& a, const Candidate& b) {
                             return a.score > b.score;
                         });
        auto packAndSong = [](const fs::path& sim) {
            return pathToUtf8(sim.parent_path().parent_path().filename()) +
                   " / " + pathToUtf8(sim.parent_path().filename());
        };
        if (found.size() == 1) {
            HudNote("Found \"%s\" in \"%s\".", file.get("Title").c_str(),
                    packAndSong(found[0].simfile).c_str());
            return found[0].simfile;
        }

        // The same song is often in several packs, let the user choose. The
        // best match is listed first and is the default button.
        if (found.size() > 1) {
            const size_t kMaxChoices = 6;
            const std::string diff = StripEnumPrefix(file.get("Difficulty"));
            std::string text = std::format(
                "Several simfiles match \"{}\" ({} {}).\n"
                "The replay was recorded on {}\n\n",
                file.get("Title"), diff, file.get("Meter"), SongKey(file));
            std::vector<std::string> buttons;
            size_t shown = std::min(found.size(), kMaxChoices);
            for (size_t i = 0; i < shown; ++i) {
                const Candidate& c = found[i];
                std::string chart =
                    c.chart >= 3  ? "has the played chart"
                    : c.chart > 0 ? "has a " + diff + " chart, other meter"
                                  : "no " + diff + " chart";
                text += std::format("{}. {}  ({})\n", i + 1,
                                    packAndSong(c.simfile), chart);
                buttons.push_back(std::to_string(i + 1));
            }
            if (found.size() > shown) {
                text += std::format("and {} more\n", found.size() - shown);
            }
            text += "\nWhich one was played?";
            buttons.emplace_back("Browse...");
            buttons.emplace_back("Cancel");
            int choice =
                gSystem->showChoiceDlg("Choose the simfile", text, buttons);
            if (choice >= 0 && choice < static_cast<int>(shown)) {
                return found[choice].simfile;
            }
            if (choice != static_cast<int>(shown)) return {};
        }

        // Ask the user, the song and its folder are remembered for next time.
        HudNote("Locate the simfile for \"%s\" (%s/%s).",
                file.get("Title").c_str(), pack.c_str(), song.c_str());
        static SDL_DialogFileFilter filters[] = {
            {"Simfiles (*.ssc, *.sm)", "ssc;sm"},
        };
        fs::path chosen =
            gSystem->openFileDlg("Locate simfile", filters, 1, fs::path());
        if (!chosen.empty() && !IsSameSong(ReadSongInfo(chosen), file)) {
            HudWarning("\"%s\" does not look like \"%s\", check the chart.",
                       pathToUtf8(chosen.filename()).c_str(),
                       file.get("Title").c_str());
        }
        return chosen;
    }

    // ChartScore for the simfile that is open.
    static int OpenChartScore(const ReplayFile& file) {
        std::string type = StripEnumPrefix(file.get("StepsType"));
        Str::toLower(type);
        std::replace(type.begin(), type.end(), '_', '-');
        const std::string diff = StripEnumPrefix(file.get("Difficulty"));
        const int meter = Str::readInt(file.get("Meter"), -1);
        int best = 0;
        for (int i = 0; i < gSimfile->getNumCharts(); ++i) {
            const Chart* chart = gSimfile->getChart(i);
            if (!chart->style || chart->style->id != type) continue;
            if (!Str::iequal(GetDifficultyName(chart->difficulty), diff)) {
                continue;
            }
            int score = 1 + (chart->meter == meter ? 2 : 0) +
                        (chart->artist == file.get("Description") ? 1 : 0);
            best = std::max(best, score);
        }
        return best;
    }

    // Remembers where a song was found, so the next replays of it open
    // without searching or asking.
    void rememberSongFile(const std::string& key, const fs::path& simfile) {
        const size_t kMaxSongs = 200;
        std::erase_if(mySongFiles,
                      [&key](const auto& entry) { return entry.first == key; });
        mySongFiles.insert(mySongFiles.begin(), {key, pathToUtf8(simfile)});
        if (mySongFiles.size() > kMaxSongs) mySongFiles.resize(kMaxSongs);
    }

    // Remembers the folder that holds the packs, so the next replays from it
    // open without asking. A simfile the user picked can be anywhere, so the
    // folder is only taken if it is called Songs or the pack folder has the
    // name of the replay's pack. Most recent first.
    void rememberSongRoot(const fs::path& simfile, const ReplayFile& file) {
        const size_t kMaxRoots = 20;
        fs::path pack = simfile.parent_path().parent_path();
        fs::path root = pack.parent_path();
        if (root.empty() || root == root.root_path()) return;
        auto parts = SongPathParts(file);
        size_t n = parts.size();
        bool isRoot =
            Str::iequal(pathToUtf8(root.filename()), "Songs") ||
            (n >= 3 && Str::iequal(pathToUtf8(pack.filename()), parts[n - 3]));
        if (!isRoot) return;
        std::string str = pathToUtf8(root);
        std::erase(mySongRoots, str);
        mySongRoots.insert(mySongRoots.begin(), str);
        if (mySongRoots.size() > kMaxRoots) mySongRoots.resize(kMaxRoots);
    }

    // Opens the chart that matches the steps type and difficulty of the
    // replay. Returns false if none was found.
    bool selectChart() {
        std::string styleId = StripEnumPrefix(myFile.get("StepsType"));
        Str::toLower(styleId);
        std::replace(styleId.begin(), styleId.end(), '_', '-');

        std::string diffName = StripEnumPrefix(myFile.get("Difficulty"));
        int meter = Str::readInt(myFile.get("Meter"), -1);
        std::string description = myFile.get("Description");

        int best = -1, bestScore = -1;
        for (int i = 0; i < gSimfile->getNumCharts(); ++i) {
            const Chart* chart = gSimfile->getChart(i);
            if (!chart->style || chart->style->id != styleId) continue;
            if (!Str::iequal(GetDifficultyName(chart->difficulty), diffName)) {
                continue;
            }
            int score = 1;
            if (chart->meter == meter) score += 2;
            if (chart->artist == description) score += 1;
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

    void clearMarks() {
        myNotes.clear();
        myPresses.clear();
        myHeld.clear();
        myLetGos.clear();
        myMines.clear();
        myIssues.clear();
        myStats = Stats();
        myWarning.clear();
    }

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

    // Returns true if the chart that is open is the one the replay was played
    // on.
    bool chartMatches() {
        const Chart* chart = gChart->get();
        if (!chart || !chart->style) return false;
        std::string styleId = StripEnumPrefix(myFile.get("StepsType"));
        Str::toLower(styleId);
        std::replace(styleId.begin(), styleId.end(), '_', '-');
        std::string diffName = StripEnumPrefix(myFile.get("Difficulty"));
        return chart->style->id == styleId &&
               Str::iequal(GetDifficultyName(chart->difficulty), diffName);
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

    void rebuild() {
        clearMarks();
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

        const double rate = myFile.rate;
        const double shift = rate * myFile.globalOffset;

        // Notes that can be judged and mines, per column.
        std::vector<std::vector<Target>> taps(numCols), mines(numCols);
        double firstNote = 1e9, lastNote = -1e9;
        for (auto& note : *gNotes) {
            if (static_cast<int>(note.col) >= numCols) continue;
            if (note.isWarped || note.isFake || note.type == NOTE_FAKE) {
                continue;
            }
            Target t;
            t.time = note.time;
            t.endTime = note.endtime;
            if (note.type == NOTE_MINE) {
                mines[note.col].push_back(t);
                continue;
            }
            t.lift = note.type == NOTE_LIFT;
            t.hold = note.endrow > note.row;
            t.roll = note.type == NOTE_ROLL;
            taps[note.col].push_back(t);
            firstNote = std::min(firstNote, note.time);
            lastNote = std::max(lastNote, note.endtime);
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
        std::vector<const RawEvent*> judgments;
        for (auto& e : myFile.events) {
            if (e.type == 'T' && e.col >= 0 && e.col < numCols) {
                judgments.push_back(&e);
            }
        }
        std::stable_sort(judgments.begin(), judgments.end(),
                         [](const RawEvent* a, const RawEvent* b) {
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
                PressMark m;
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
                         [](const NoteMark& a, const NoteMark& b) {
                             return a.time != b.time ? a.time < b.time
                                                     : a.col < b.col;
                         });
        std::stable_sort(myPresses.begin(), myPresses.end(),
                         LessByTime<PressMark>);
        std::stable_sort(myLetGos.begin(), myLetGos.end(), LessByTime<Mark>);
        std::stable_sort(myMines.begin(), myMines.end(), LessByTime<Mark>);
        std::stable_sort(
            myHeld.begin(), myHeld.end(),
            [](const Span& a, const Span& b) { return a.begin < b.begin; });

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
    double matchHits(const std::vector<const RawEvent*>& judgments,
                     std::vector<std::vector<Target>>& taps,
                     std::vector<std::vector<Press>>& presses, double rate,
                     double shift, double bias, double tolerance,
                     bool measure) {
        std::vector<double> deltas;
        const double lookback = 0.5 + std::max(1.0, rate);
        for (const RawEvent* e : judgments) {
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

            NoteMark n;
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
                PressMark m;
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
                NoteMark& note = myNotes.back();
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
        PressMark m;
        m.time = drawTime;
        m.col = col;
        m.kind = kind;
        m.judge = judge;
        myPresses.push_back(m);
        return true;
    }

    void matchMisses(const std::vector<const RawEvent*>& judgments,
                     std::vector<std::vector<Target>>& taps, double rate,
                     double shift) {
        // A miss is reported once the note left the largest timing window.
        double window = 0.0;
        for (double w : myFile.window) window = std::max(window, w);
        const double delay = window * rate;
        for (const RawEvent* e : judgments) {
            if (e->score != "Miss") continue;
            const double t = e->time + shift;
            int target = nearestFree(taps[e->col], t - delay, 0.4);
            if (target < 0) continue;
            Target& tgt = taps[e->col][target];
            tgt.mark = static_cast<int>(myNotes.size());
            NoteMark n;
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

    // ================================================================================================
    // ReplayImpl :: navigation.

    void jumpToIssue(bool forward) override {
        if (!myIsOpen || myIssues.empty()) {
            HudNote("No replay issues to jump to.");
            return;
        }
        const double cursor = gView->getCursorTime();
        const double eps = 0.002;
        if (forward) {
            auto it = std::upper_bound(myIssues.begin(), myIssues.end(),
                                       cursor + eps);
            if (it == myIssues.end()) {
                HudNote("No more issues after the cursor.");
                return;
            }
            gView->setCursorTime(*it);
        } else {
            auto it = std::lower_bound(myIssues.begin(), myIssues.end(),
                                       cursor - eps);
            if (it == myIssues.begin()) {
                HudNote("No more issues before the cursor.");
                return;
            }
            gView->setCursorTime(*(it - 1));
        }
    }

    // ================================================================================================
    // ReplayImpl :: drawing.

    struct Geometry {
        int colX[SIM_MAX_COLUMNS] = {};
        int colW = 64;
        int numCols = 0;
        double begin = 0.0, end = 0.0;
    };

    bool visible() const {
        return myIsOpen && myStatus.empty() && gChart->isOpen() &&
               gStyle->getNumCols() == myFile.columns;
    }

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

    void drawBelowNotes() override {
        if (!visible() || !myShowHeldPanels) return;
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

    void drawAboveNotes() override {
        if (!visible()) return;
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
    void drawLabels(const Geometry& g) {
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

    void drawPanel() override {
        if (!myIsOpen) return;
        if (myShowPanel) drawSummary();
        if (timelineActive()) drawTimeline();
    }

    void drawSummary() {
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

        if (!myStatus.empty()) {
            text += "\n\n{tc:FF8040}" + myStatus + "{tc}";
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
    // ReplayImpl :: timeline.
    //
    // A strip left of the minimap that covers the whole chart with the same
    // vertical scale as the minimap: every judged note as a dot at its offset
    // (early on the left, late on the right), a running mean, misses and
    // dropped holds as lines across, and stray taps in a lane on the left.

    bool timelineActive() const {
        return myShowTimeline && visible() && !gTextOverlay->isOpen();
    }

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

    void seekTimeline(int y) {
        recti r = timelineRect();
        double begin = 0.0, end = 0.0;
        timelineRange(begin, end);
        if (end <= begin || r.h <= 0) return;
        double t = std::clamp(
            static_cast<double>(y - r.y) / static_cast<double>(r.h), 0.0, 1.0);
        if (gView->hasReverseScroll()) t = 1.0 - t;
        gView->setCursorOffset(begin + (end - begin) * t);
    }

    void drawTimeline() {
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

    void onMousePress(MousePress& evt) override {
        if (evt.button != Mouse::LMB || evt.handled || !timelineActive()) {
            return;
        }
        if (IsInside(timelineFrame(), evt.x, evt.y)) {
            seekTimeline(evt.y);
            myIsDragging = true;
            evt.handled = true;
        }
    }

    void onMouseRelease(MouseRelease& evt) override {
        if (evt.button == Mouse::LMB && myIsDragging) {
            seekTimeline(evt.y);
            myIsDragging = false;
        }
    }

    void tick() override {
        if (myIsDragging) seekTimeline(gSystem->getMousePos().y);
    }

    // ================================================================================================
    // ReplayImpl :: toggles.

    void toggleShowPresses() override {
        myShowPresses = !myShowPresses;
        gMenubar->update(Menubar::REPLAY);
    }

    void toggleShowHeldPanels() override {
        myShowHeldPanels = !myShowHeldPanels;
        gMenubar->update(Menubar::REPLAY);
    }

    void toggleShowLabels() override {
        myShowLabels = !myShowLabels;
        gMenubar->update(Menubar::REPLAY);
    }

    void toggleShowPanel() override {
        myShowPanel = !myShowPanel;
        gMenubar->update(Menubar::REPLAY);
    }

    void toggleShowTimeline() override {
        myShowTimeline = !myShowTimeline;
        gMenubar->update(Menubar::REPLAY);
    }

    bool hasShowPresses() const override { return myShowPresses; }
    bool hasShowHeldPanels() const override { return myShowHeldPanels; }
    bool hasShowLabels() const override { return myShowLabels; }
    bool hasShowPanel() const override { return myShowPanel; }
    bool hasShowTimeline() const override { return myShowTimeline; }
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
