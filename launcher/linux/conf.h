// driveclub.conf: the Linux settings file (key=value lines), shared by the launcher and run.sh.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class Conf {
public:
    // Missing file: empty settings. Returns false only when the file exists and cannot be read.
    bool Load(const std::filesystem::path& path);
    // Writes the file atomically (a temporary file renamed over it). Returns "" or the problem.
    std::string Save(const std::filesystem::path& path) const;

    bool Has(const std::string& key) const;
    std::string Get(const std::string& key, const std::string& fallback = "") const;
    void Set(const std::string& key, const std::string& value);
    void Erase(const std::string& key);
    // Every entry, in file order (env.NAME entries included).
    const std::vector<std::pair<std::string, std::string>>& Items() const { return items_; }

private:
    std::vector<std::pair<std::string, std::string>> items_;
};

// patches_on / patches_off: names separated by ';'.
std::vector<std::string> SplitList(const std::string& text);
std::string JoinList(const std::vector<std::string>& names);
