// The game files: the community patches, the game folders (checks with the Windows launcher's messages) and the
// search for the dumps. Built on launcher/prepare.cpp.
#pragma once

#include "model.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

std::vector<PatchInfo> ReadPatches(const std::filesystem::path& xml);

struct FolderCheck {
    bool update_ok = false, base_ok = false;
    std::string update_problem, base_problem; // "" when fine or not chosen
};
// A dump that already is version 1.28 in one folder serves as both: an empty `update` is then set to `base`.
FolderCheck CheckFolders(std::string& update, const std::string& base);

struct FoundDump {
    std::string path;
    enum Kind { Update, Base, Merged } kind;
    std::string title_id; // CUSA00093 (US) or CUSA00003 (EU)
};
// Searches the usual places (home 5 levels deep, SD cards and USB drives 6). `stop` ends it early; `visited` counts
// the folders looked at (for the progress line).
std::vector<FoundDump> FindDumps(const std::atomic<bool>& stop, std::atomic<int>& visited);
// What the search found, as the two folders when that is unambiguous (one merged dump, or one update and one base).
bool PickDumps(const std::vector<FoundDump>& found, std::string& update, std::string& base);
// Whether a folder looks like a dump for the browser's marks: 1 update, 2 base game, 3 both, 0 neither.
int DumpKind(const std::filesystem::path& folder);
