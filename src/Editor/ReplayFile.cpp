#include <Editor/ReplayFile.h>

#include <Core/StringUtils.h>

#include <System/File.h>

namespace Vortex {

std::string StripEnumPrefix(const std::string& s) {
    auto pos = s.find('_');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

bool ParseReplay(const fs::path& path, ReplayFile& out, std::string& error) {
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

        ReplayEvent e;
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

};  // namespace Vortex
