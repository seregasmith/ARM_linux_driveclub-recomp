// dc-prepare: the launcher's first-run preparation (launcher/prepare.cpp) as a command for Linux, where the
// Windows launcher does not run (run.sh and the AppImage call it). It converts the player's own dumps into the
// image the runtime maps (<work>/prep/boot-linked.bin, content.bin, patches.bin) and writes the patch selection
// (<work>/user/patches_enabled.txt): the patches that are on by default in the XML, plus --enable, minus --disable.
//
//   dc-prepare --update DIR --base DIR --work DIR --patches-xml FILE [--enable NAME]... [--disable NAME]...
//   dc-prepare --patches-xml FILE --list
//
// The image is converted again only when the folders differ from the last run (<work>/prep/stamp.txt) or --force
// is given; otherwise only patches.bin is rebuilt (a second or so). Exit code 0 on success, 1 with a message.
#include "../launcher/prepare.cpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

void Usage() {
    std::fprintf(stderr,
                 "usage: dc-prepare --update DIR --base DIR --work DIR --patches-xml FILE\n"
                 "                  [--enable NAME]... [--disable NAME]... [--force]\n"
                 "       dc-prepare --patches-xml FILE --list\n");
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

} // namespace

int main(int argc, char** argv) {
    fs::path update, base, work, xml;
    std::vector<std::string> enable, disable;
    bool list = false, force = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "dc-prepare: %s needs a value\n", what);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--update") update = value("--update");
        else if (a == "--base") base = value("--base");
        else if (a == "--work") work = value("--work");
        else if (a == "--patches-xml") xml = value("--patches-xml");
        else if (a == "--enable") enable.push_back(value("--enable"));
        else if (a == "--disable") disable.push_back(value("--disable"));
        else if (a == "--list") list = true;
        else if (a == "--force") force = true;
        else if (a == "-h" || a == "--help") { Usage(); return 0; }
        else { std::fprintf(stderr, "dc-prepare: unknown option %s\n", a.c_str()); Usage(); return 1; }
    }
    if (xml.empty()) { Usage(); return 1; }
    std::vector<PatchEntry> patches;
    try {
        patches = ParsePatches(ReadText(xml));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dc-prepare: cannot read %s: %s\n", xml.c_str(), e.what());
        return 1;
    }
    if (list) {
        for (const auto& p : patches) {
            std::printf("%s%s\t%s\n", p.always ? "always " : p.default_on ? "on     " : "off    ", "", p.name.c_str());
        }
        return 0;
    }
    if (update.empty() || base.empty() || work.empty()) { Usage(); return 1; }
    // An update dump that already holds the base game (a merged dump) may be given for both.
    std::set<std::string> chosen;
    for (const auto& p : patches) {
        if (p.default_on) chosen.insert(p.name);
    }
    auto known = [&](const std::string& name) {
        for (const auto& p : patches) {
            if (p.name == name) return true;
        }
        return false;
    };
    for (const auto& name : enable) {
        if (!known(name)) std::fprintf(stderr, "dc-prepare: warning: no patch named \"%s\" in %s\n", name.c_str(), xml.c_str());
        chosen.insert(name);
    }
    for (const auto& name : disable) chosen.erase(name);
    try {
        fs::create_directories(work / "user");
        fs::create_directories(work / "prep");
        std::ofstream sel(work / "user" / "patches_enabled.txt", std::ios::binary | std::ios::trunc);
        for (const auto& name : chosen) sel << name << "\n";
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dc-prepare: cannot write to %s: %s\n", work.c_str(), e.what());
        return 1;
    }
    const fs::path stamp_path = work / "prep" / "stamp.txt";
    const std::string stamp_now = fs::absolute(update).string() + "|" + fs::absolute(base).string();
    const bool ready = fs::exists(work / "prep" / "boot-linked.bin") && fs::exists(work / "prep" / "image.meta") &&
                       fs::exists(work / "prep" / "eboot.elf");
    const bool patches_only = !force && ready && ReadText(stamp_path) == stamp_now;
    const std::string error = PrepareGame(update, base, work, xml, patches_only, [](int step, const std::string& text) {
        std::printf("[%d/6] %s\n", step, text.c_str());
        std::fflush(stdout);
    });
    if (!error.empty()) {
        std::fprintf(stderr, "dc-prepare: %s\n", error.c_str());
        return 1;
    }
    std::ofstream(stamp_path, std::ios::binary | std::ios::trunc) << stamp_now;
    return 0;
}
