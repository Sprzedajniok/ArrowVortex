#include <Editor/ReplayLocate.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

#include <Core/StringUtils.h>
#include <Core/Utils.h>
#include <Core/Xmr.h>

#include <System/File.h>
#include <System/System.h>

#include <Simfile/Chart.h>
#include <Simfile/Simfile.h>

#include <Managers/SimfileMan.h>
#include <Managers/StyleMan.h>

#include <Editor/Common.h>

namespace Vortex {

namespace {

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
    std::string full = NormalizeName(file.get("Title") + file.get("Subtitle"));
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

// ChartScore for the simfile that is open.
static int OpenChartScore(const ReplayFile& file) {
    int best = 0;
    for (int i = 0; i < gSimfile->getNumCharts(); ++i) {
        best = std::max(best, ScoreReplayChart(file, *gSimfile->getChart(i)));
    }
    return best;
}

};  // anonymous namespace

int ScoreReplayChart(const ReplayFile& file, const Chart& chart) {
    std::string type = StripEnumPrefix(file.get("StepsType"));
    Str::toLower(type);
    std::replace(type.begin(), type.end(), '_', '-');
    const std::string diff = StripEnumPrefix(file.get("Difficulty"));
    if (!chart.style || chart.style->id != type) return 0;
    if (!Str::iequal(GetDifficultyName(chart.difficulty), diff)) return 0;
    return 1 + (chart.meter == Str::readInt(file.get("Meter"), -1) ? 2 : 0) +
           (chart.artist == file.get("Description") ? 1 : 0);
}

// ================================================================================================
// ReplayLibrary.

void ReplayLibrary::load(XmrNode& node) {
    if (const char* roots = node.get("songRoots")) {
        for (auto& root : Str::split(roots, "|", false, true)) {
            mySongRoots.push_back(UnescapeSetting(root));
        }
    }
    if (const char* files = node.get("songFiles")) {
        for (auto& entry : Str::split(files, "|", false, true)) {
            auto sep = entry.find('>');
            if (sep == std::string::npos) continue;
            mySongFiles.emplace_back(UnescapeSetting(entry.substr(0, sep)),
                                     UnescapeSetting(entry.substr(sep + 1)));
        }
    }
}

void ReplayLibrary::save(XmrNode& node) const {
    std::vector<std::string> roots, files;
    for (auto& root : mySongRoots) roots.push_back(EscapeSetting(root));
    for (auto& [song, file] : mySongFiles) {
        files.push_back(EscapeSetting(song) + ">" + EscapeSetting(file));
    }
    node.addAttrib("songRoots", Str::join(roots, "|").c_str());
    node.addAttrib("songFiles", Str::join(files, "|").c_str());
}

// Finds the simfile the replay was played on. The replay stores the
// ITGmania path ("/Songs/Pack/Song/song.ssc"). In order, this tries: the
// simfile that is already open (useCurrent is set), songs that were found
// somewhere else before, the same pack and song folder in every folder
// that holds packs, the song folder in any pack, any simfile with the same
// title and artist, and finally asks the user. Sets remember when the song
// was not where the replay says.
fs::path ReplayLibrary::locateSimfile(const fs::path& replayPath,
                                      const ReplayFile& file, bool& useCurrent,
                                      bool& remember) {
    auto parts = SongPathParts(file);
    std::string pack, song, name;
    size_t n = parts.size();
    if (n >= 3) pack = parts[n - 3], song = parts[n - 2], name = parts[n - 1];

    // The simfile that is open already.
    if (gSimfile->isOpen()) {
        fs::path dir = utf8ToPath(gSimfile->getDir());
        if (dir.filename().empty()) dir = dir.parent_path();
        const Simfile* sim = gSimfile->get();
        SongInfo info = {sim->title, sim->titleTr, sim->subtitle, sim->artist,
                         sim->artistTr};
        bool sameDir =
            !song.empty() && Str::iequal(pathToUtf8(dir.filename()), song) &&
            Str::iequal(pathToUtf8(dir.parent_path().filename()), pack);
        // By title only if it also has the played chart, the same song
        // is often in several packs with different charts.
        if (sameDir || (IsSameSong(info, file) && OpenChartScore(file) >= 3)) {
            useCurrent = true;
            return {};
        }
    }

    // A song that was found somewhere else before.
    std::error_code ec;
    for (auto& [key, path] : mySongFiles) {
        if (key == SongKey(file) && fs::is_regular_file(utf8ToPath(path), ec)) {
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
        return pathToUtf8(sim.parent_path().parent_path().filename()) + " / " +
               pathToUtf8(sim.parent_path().filename());
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
            std::string chart = c.chart >= 3 ? "has the played chart"
                                : c.chart > 0
                                    ? "has a " + diff + " chart, other meter"
                                    : "no " + diff + " chart";
            text += std::format("{}. {}  ({})\n", i + 1, packAndSong(c.simfile),
                                chart);
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
    HudNote("Locate the simfile for \"%s\" (%s/%s).", file.get("Title").c_str(),
            pack.c_str(), song.c_str());
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

// Remembers where a song was found, so the next replays of it open
// without searching or asking.
void ReplayLibrary::rememberSongFile(const ReplayFile& file,
                                     const fs::path& simfile) {
    const std::string key = SongKey(file);
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
void ReplayLibrary::rememberSongRoot(const fs::path& simfile,
                                     const ReplayFile& file) {
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

};  // namespace Vortex
