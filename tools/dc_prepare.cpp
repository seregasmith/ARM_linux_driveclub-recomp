// dc-prepare: the launcher's first-run preparation (launcher/prepare.cpp) as a command for Linux, where the
// Windows launcher does not run (run.sh and the AppImage call it). It converts the player's own dumps into the
// image the runtime maps (<work>/prep/boot-linked.bin, content.bin, patches.bin) and writes the patch selection
// (<work>/user/patches_enabled.txt): the patches that are on by default in the XML, plus --enable, minus --disable.
//
//   dc-prepare --update DIR --base DIR --work DIR --patches-xml FILE [--enable NAME]... [--disable NAME]...
//              [--unlock] [--car-switch] [--detail KEY=VALUE]... [--force]
//   dc-prepare --patches-xml FILE --list
//
// The image is converted again only when the folders differ from the last run (<work>/prep/stamp.txt) or --force
// is given; otherwise only patches.bin is rebuilt (a second or so). Exit code 0 on success, 1 with a message.
//
// Replacement game files built from the dumps, as the Windows launcher builds them (launcher/unlock.cpp,
// carswitch.cpp, graphics.cpp): --unlock (the add-on content, <work>/unlock), --car-switch (faster car select,
// <work>/carswitch), --detail (graphics detail, <work>/graphics; KEY: own_car, opponents, reflections, particles,
// dynamic_res, motion_blur, lens_dust, heat_haze, fxaa = 0|1, scenery, shadows = game|high|ultra). Each is rebuilt only
// when the dumps or the choices changed. <work>/prep/mods.txt lists the folders the runtime is to serve (--mods); one
// that cannot be built is left out with a warning, and the game starts without it.
#include "../launcher/prepare.cpp"
#include "../launcher/carswitch.h"
#include "../launcher/fileio.h"
#include "../launcher/graphics.h"
#include "../launcher/unlock.h"

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
                 "                  [--enable NAME]... [--disable NAME]... [--unlock] [--car-switch]\n"
                 "                  [--detail KEY=VALUE]... [--force]\n"
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
    bool list = false, force = false, unlock = false, car_switch = false;
    GraphicsChoice detail;
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
        else if (a == "--unlock") unlock = true;
        else if (a == "--car-switch") car_switch = true;
        else if (a == "--detail") {
            const std::string kv = value("--detail");
            const size_t eq = kv.find('=');
            const std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
            const bool on = v == "1" || v == "on" || v == "true";
            const int level = v == "high" || v == "1" ? 1 : v == "ultra" || v == "2" ? 2 : 0;
            if (k == "own_car") detail.own_car = on;
            else if (k == "opponents") detail.opponents = on;
            else if (k == "reflections") detail.reflections = on;
            else if (k == "particles") detail.particles = on;
            else if (k == "dynamic_res") detail.dynamic_res = on;
            else if (k == "motion_blur") detail.motion_blur = on;
            else if (k == "lens_dust") detail.lens_dust = on;
            else if (k == "heat_haze") detail.heat_haze = on;
            else if (k == "fxaa") detail.fxaa = on;
            else if (k == "scenery") detail.scenery = level;
            else if (k == "shadows") detail.shadows = level;
            else std::fprintf(stderr, "dc-prepare: warning: unknown --detail %s\n", k.c_str());
        }
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

    // Replacement files from the dumps (the runtime serves the folders in this order, after the player's mods).
    std::vector<fs::path> mods;
    auto built = [&](const char* what, const std::wstring& problem, const fs::path& dir, bool active) {
        if (!problem.empty()) {
            std::fprintf(stderr, "dc-prepare: warning: %s: %s. The game starts without it.\n", what,
                         fileio::Narrow(problem).c_str());
        } else if (active) {
            mods.push_back(fs::absolute(dir));
        }
    };
    if (unlock) {
        std::printf("[6/6] Unlocking the add-on content\n");
        std::fflush(stdout);
        built("add-on content", BuildUnlock(update, base, work / "unlock"), work / "unlock", true);
    }
    if (car_switch) {
        std::printf("[6/6] Faster car switching\n");
        std::fflush(stdout);
        built("faster car switching", BuildFastCarSwitch(update, base, work / "carswitch"), work / "carswitch", true);
    }
    std::error_code ec;
    if (!detail.Default() || fs::exists(work / "graphics" / "global.rpk", ec)) {
        if (!detail.Default()) {
            std::printf("[6/6] Graphics detail\n");
            std::fflush(stdout);
        }
        bool active = false;
        const std::wstring problem = BuildGraphics(update, base, work / "graphics", detail, active);
        built("graphics detail", problem, work / "graphics", active);
    }
    std::ofstream list_out(work / "prep" / "mods.txt", std::ios::binary | std::ios::trunc);
    for (const auto& dir : mods) list_out << dir.string() << "\n";
    return 0;
}
