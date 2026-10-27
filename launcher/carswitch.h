// Builds the menu files that make the car switch in the car select faster.
#pragma once
#include <filesystem>
#include <string>

// Reads the three showroom menu files (Canada, Chile, UK) from the player's own game archive and writes copies with
// shorter blur animations to `out_dir`. The runtime serves them in place of the originals; the game folders are never
// touched. Skips the work when the copies in `out_dir` already match the archive. Returns "" or a short description of
// a problem.
std::wstring BuildFastCarSwitch(const std::filesystem::path& update_dir, const std::filesystem::path& base_dir,
                                const std::filesystem::path& out_dir);
