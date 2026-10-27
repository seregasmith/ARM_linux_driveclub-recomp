// The preparation code itself (PrepareGame, SfoField, FolderIsVersion128, ParsePatches), as dc-prepare includes it.
#include "../prepare.cpp"

#include "game.h"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <system_error>

namespace {

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// True when the first 64 KiB of the file contain the text.
bool FileHas(const fs::path& path, const char* text) {
    std::ifstream in(path, std::ios::binary);
    std::string data(65536, '\0');
    in.read(data.data(), std::streamsize(data.size()));
    data.resize(size_t(in.gcount()));
    return data.find(text) != std::string::npos;
}

// DriveClub's param.sfo: the US release (CUSA00093) or the European one (CUSA00003).
std::string TitleOf(const fs::path& sfo) {
    if (FileHas(sfo, "CUSA00093")) return "CUSA00093";
    if (FileHas(sfo, "CUSA00003")) return "CUSA00003";
    return "";
}
bool IsDriveClub(const fs::path& sfo) { return !TitleOf(sfo).empty(); }

} // namespace

std::vector<PatchInfo> ReadPatches(const fs::path& xml) {
    std::vector<PatchInfo> out;
    std::ifstream in(xml, std::ios::binary);
    if (!in) return out;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        for (const auto& p : ParsePatches(text)) out.push_back({p.name, p.note, p.default_on, p.always});
    } catch (const std::exception&) {
    }
    return out;
}

FolderCheck CheckFolders(std::string& update, const std::string& base) {
    FolderCheck c;
    const fs::path b(base);
    const bool merged = !base.empty() && Exists(b / "game000.dat") && Exists(b / "eboot.bin") &&
                        Exists(b / "sce_sys" / "param.sfo") && IsDriveClub(b / "sce_sys" / "param.sfo") &&
                        FolderIsVersion128(b);
    if (merged && update.empty()) update = base;
    if (merged && update == base) {
        c.update_ok = c.base_ok = true;
        return c;
    }
    const fs::path u(update);
    c.update_ok = !update.empty() && Exists(u / "eboot.bin") && Exists(u / "sce_sys" / "param.sfo") &&
                  !Exists(u / "game000.dat") && IsDriveClub(u / "sce_sys" / "param.sfo") && FolderIsVersion128(u);
    c.base_ok = !base.empty() && Exists(b / "game000.dat") && IsDriveClub(b / "sce_sys" / "param.sfo");
    // Why a folder is not accepted, in words a player can act on.
    if (!update.empty() && !c.update_ok) {
        const bool eboot = Exists(u / "eboot.bin"), game = Exists(u / "game000.dat"), sfo = Exists(u / "sce_sys" / "param.sfo");
        if (!Exists(u)) c.update_problem = "This folder does not exist (any more). Choose it again.";
        else if (game) c.update_problem = "This folder holds the base game. If your dump is already version 1.28 in this one folder, choose it for both. Otherwise choose the v1.28 update here.";
        else if (!eboot || !sfo) c.update_problem = "Not the update: no eboot.bin and sce_sys here. If it is a parent folder, open it and choose the folder inside.";
        else if (!IsDriveClub(u / "sce_sys" / "param.sfo")) c.update_problem = "This is another game, not DriveClub (CUSA00093 or CUSA00003).";
        else c.update_problem = "This update is not version 1.28 (its param.sfo has another version). Choose the v1.28 update dump.";
    }
    if (!base.empty() && !c.base_ok) {
        const bool eboot = Exists(b / "eboot.bin"), game = Exists(b / "game000.dat");
        if (!Exists(b)) c.base_problem = "This folder does not exist (any more). Choose it again.";
        else if (eboot && !game) c.base_problem = "This is the update, not the base game. Choose the base game folder here (it contains game000.dat).";
        else if (game && !IsDriveClub(b / "sce_sys" / "param.sfo")) c.base_problem = "This is another game, not DriveClub (CUSA00093 or CUSA00003).";
        else c.base_problem = "Not the base game: no game000.dat here. If it is a parent folder, open it and choose the folder inside.";
    }
    return c;
}

int DumpKind(const fs::path& folder) {
    const fs::path sfo = folder / "sce_sys" / "param.sfo";
    if (!Exists(sfo) || !IsDriveClub(sfo)) return 0;
    const bool game = Exists(folder / "game000.dat"), eboot = Exists(folder / "eboot.bin");
    if (game && eboot && FolderIsVersion128(folder)) return 3;
    if (game) return 2;
    if (eboot && FolderIsVersion128(folder)) return 1;
    return 0;
}

std::vector<FoundDump> FindDumps(const std::atomic<bool>& stop, std::atomic<int>& visited) {
    std::vector<FoundDump> found;
    std::vector<std::pair<fs::path, int>> roots; // folder, levels below it to search
    // SD cards and USB drives are mounted at /run/media/<user>/<label> (SteamOS) or /media/<user>/<label>, and
    // dumps often sit a few folders inside (Games/PS4/DriveClub).
    if (const char* home = std::getenv("HOME"); home && *home) roots.push_back({home, 5});
    for (const char* top : {"/run/media", "/media", "/mnt"}) roots.push_back({top, 6});
    constexpr int kMaxFolders = 60000; // a bound on the time a search takes
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
        if (stop || visited >= kMaxFolders) return;
        ++visited;
        if (const int kind = DumpKind(dir)) {
            const FoundDump::Kind k = kind == 3 ? FoundDump::Merged : kind == 2 ? FoundDump::Base : FoundDump::Update;
            found.push_back({dir.string(), k, TitleOf(dir / "sce_sys" / "param.sfo")});
            return; // a game folder has no other dump inside
        }
        if (depth == 0) return;
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (stop) return;
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue; // hidden: settings, caches, Steam's own folders
            std::error_code e2;
            if (!it->is_directory(e2) || it->is_symlink(e2)) continue;
            walk(it->path(), depth - 1);
        }
    };
    for (const auto& [root, depth] : roots) {
        if (Exists(root)) walk(root, depth);
    }
    // The same folder reached twice (a mount under /media and /run/media): once.
    std::vector<FoundDump> unique;
    for (const auto& f : found) {
        bool dup = false;
        for (const auto& u : unique) {
            std::error_code ec;
            dup = dup || u.path == f.path || fs::equivalent(u.path, f.path, ec);
        }
        if (!dup) unique.push_back(f);
    }
    return unique;
}

bool PickDumps(const std::vector<FoundDump>& found, std::string& update, std::string& base) {
    std::vector<const FoundDump*> merged, updates, bases;
    for (const auto& f : found) {
        (f.kind == FoundDump::Merged ? merged : f.kind == FoundDump::Update ? updates : bases).push_back(&f);
    }
    if (merged.size() == 1 && updates.empty()) {
        update = base = merged[0]->path;
        return true;
    }
    if (merged.empty() && updates.size() == 1 && bases.size() == 1) {
        update = updates[0]->path;
        base = bases[0]->path;
        return true;
    }
    return false;
}
