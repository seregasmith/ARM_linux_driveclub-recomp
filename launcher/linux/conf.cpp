#include "conf.h"

#include <cstdio>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {
// The order run.sh writes the keys in; others follow in the order they came.
const char* const kOrder[] = {"update", "base", "preset", "output", "render", "fps", "fps_cap", "fullscreen", "aa",
                              "upscale", "sharpness", "mirror", "aniso", "two_thread", "show_fps", "mangohud", "name",
                              "language", "dlc_unlock", "fast_car_switch", "gearbox", "brake_assist", "hardcore",
                              "wheel_assist", "overlays", "patches_on", "patches_off"};
} // namespace

bool Conf::Load(const fs::path& path) {
    items_.clear();
    std::error_code ec;
    if (!fs::exists(path, ec)) return true;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        Set(line.substr(0, eq), line.substr(eq + 1));
    }
    return true;
}

std::string Conf::Save(const fs::path& path) const {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return "cannot write " + tmp.string();
        out << "# DriveClub runtime settings (run.sh --help lists the keys). Saved by the launcher.\n";
        std::vector<bool> done(items_.size(), false);
        for (const char* key : kOrder) {
            for (size_t i = 0; i < items_.size(); ++i) {
                if (!done[i] && items_[i].first == key) {
                    out << items_[i].first << '=' << items_[i].second << '\n';
                    done[i] = true;
                }
            }
        }
        for (int pass = 0; pass < 2; ++pass) { // other settings, then the env. lines
            for (size_t i = 0; i < items_.size(); ++i) {
                const bool env = items_[i].first.rfind("env.", 0) == 0;
                if (!done[i] && env == (pass == 1)) {
                    out << items_[i].first << '=' << items_[i].second << '\n';
                    done[i] = true;
                }
            }
        }
        if (!out.flush()) return "cannot write " + tmp.string();
    }
    fs::rename(tmp, path, ec);
    if (ec) return "cannot replace " + path.string() + ": " + ec.message();
    return "";
}

bool Conf::Has(const std::string& key) const {
    for (const auto& item : items_) {
        if (item.first == key) return true;
    }
    return false;
}

std::string Conf::Get(const std::string& key, const std::string& fallback) const {
    for (const auto& item : items_) {
        if (item.first == key) return item.second;
    }
    return fallback;
}

void Conf::Set(const std::string& key, const std::string& value) {
    for (auto& item : items_) {
        if (item.first == key) {
            item.second = value;
            return;
        }
    }
    items_.emplace_back(key, value);
}

void Conf::Erase(const std::string& key) {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].first == key) {
            items_.erase(items_.begin() + long(i));
            return;
        }
    }
}

std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(';', start);
        const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) out.push_back(part);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string JoinList(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) out += ';';
        out += name;
    }
    return out;
}
