#include "model.h"

#include <algorithm>
#include <cstdlib>
#include <set>

const char* const kTabName[kTabCount] = {"Play", "Graphics", "Detail", "Game", "Driving", "Advanced"};

namespace {

// Sizes the presets set (the Windows launcher's ApplyPreset, and run.sh's preset table).
struct Preset {
    const char* value;
    const char* label;
    const char* output;
    const char* render;
    const char* mirror;
};
const Preset kPresets[] = {
    {"handheld", "Handheld", "720p", "720p", "static"},
    {"low", "Low", "default", "720p", "live"},
    {"medium", "Medium", "1440p", "default", "live"},
    {"high", "High", "4k", "1440p", "live"},
    {"ultra", "Ultra", "4k", "4k", "live"},
};
constexpr int kPresetCount = 5;
const Preset kCustom = {"custom", "Custom", "default", "default", "live"};
const Preset& PresetAt(int i) { return i >= 0 && i < kPresetCount ? kPresets[i] : kCustom; }

// Driving presets: gearbox, brake assist, hardcore handling, wheel assist (choice indices; 0 = the game's own).
const int kDrive[3][4] = {{2, 1, 2, 1}, {1, 3, 1, 2}, {0, 0, 0, 0}};
const char* const kDriveRows[4] = {"gearbox", "brake_assist", "hardcore", "wheel_assist"};

const char* const kLanguages[21] = {"Japanese", "English (US)", "French", "Spanish", "German", "Italian", "Dutch",
                                    "Portuguese", "Russian", "Korean", "Chinese (Traditional)", "Chinese (Simplified)",
                                    "Finnish", "Swedish", "Danish", "Norwegian", "Polish", "Portuguese (Brazil)",
                                    "English (UK)", "Turkish", "Spanish (Mexico)"};

// The community patches shown on the Game tab (their full names in patches/DriveClub.xml).
struct Curated {
    const char* name;
    const char* label;
    const char* hint;
};
const Curated kCurated[] = {
    {"Disable motion blur and depth of field", "No motion blur or depth of field",
     "Turns off the game's motion blur and depth of field (they share one shader)."},
    {"Reset Disable", "No reset-to-track timer", "Disables the timer that puts the car back on the track."},
    {"Barrier Disable", "No track barriers", "Removes the invisible barriers at the track edges."},
    {"Disable Photomode Free Camera Collision", "Photo mode: camera through cars",
     "The free camera passes through cars, so you can look inside any car."},
    {"Disable Photomode Processing", "Photo mode: skip processing", "Photos are saved without the game's processing pass."},
};
const char* const kBlurPatch = "Disable motion blur and depth of field";

std::vector<Choice> OnOff() { return {{"0", "Off"}, {"1", "On"}}; }

bool Contains(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}
void Remove(std::vector<std::string>& list, const std::string& name) {
    list.erase(std::remove(list.begin(), list.end(), name), list.end());
}

int ChoiceOf(const Row& r, const std::string& value) {
    for (size_t i = 0; i < r.choices.size(); ++i) {
        if (r.choices[i].value == value) return int(i);
    }
    return -1;
}

} // namespace

int Model::Find(const std::string& id) const {
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].id == id) return int(i);
    }
    return -1;
}

std::vector<int> Model::Page(int page) const {
    std::vector<int> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].page == page) out.push_back(int(i));
    }
    return out;
}

void Model::Build(const std::vector<PatchInfo>& patches, int display_hz) {
    patches_ = patches;
    rows.clear();
    auto add = [&](int page, const char* section, const char* id, const char* label, const char* hint, Kind kind) -> Row& {
        Row r;
        r.page = page;
        r.section = section;
        r.id = id;
        r.label = label;
        r.hint = hint;
        r.kind = kind;
        rows.push_back(std::move(r));
        return rows.back();
    };
    auto choice = [&](int page, const char* section, const char* id, const char* label, const char* hint,
                      std::vector<Choice> choices, int def, Kind kind = Kind::Cycle) -> Row& {
        Row& r = add(page, section, id, label, hint, kind);
        r.choices = std::move(choices);
        r.def = r.sel = def;
        return r;
    };
    auto toggle = [&](int page, const char* section, const char* id, const char* label, const char* hint, bool def) -> Row& {
        return choice(page, section, id, label, hint, OnOff(), def ? 1 : 0, Kind::Toggle);
    };
    auto slider = [&](int page, const char* section, const char* id, const char* label, const char* hint, int lo, int hi,
                      int step, int def, const char* unit) -> Row& {
        Row& r = add(page, section, id, label, hint, Kind::Slider);
        r.min = lo;
        r.max = hi;
        r.step = step;
        r.def = r.sel = def;
        r.unit = unit;
        return r;
    };

    // ---- Play
    add(kTabPlay, "", "@play", "Play", "Starts DriveClub with these settings.", Kind::Button).primary = true;
    add(kTabPlay, "Game folders", "update", "Update folder (version 1.28)",
        "The DriveClub v1.28 update dump: the folder with eboot.bin. A dump that is already 1.28 in one folder can be "
        "used for both.",
        Kind::Folder);
    add(kTabPlay, "Game folders", "base", "Base game folder", "The DriveClub base game dump: the folder with game000.dat.",
        Kind::Folder);
    add(kTabPlay, "Game folders", "@find", "Find the game folders again",
        "Searches your home folder, the SD card and USB drives for the DriveClub dumps.", Kind::Button);
    add(kTabPlay, "", "@quit", "Quit", "Closes the launcher without starting the game.", Kind::Button);

    // ---- Graphics
    {
        std::vector<Choice> c;
        for (const auto& p : kPresets) c.push_back({p.value, p.label});
        c.push_back({kCustom.value, kCustom.label});
        choice(kTabGraphics, "Performance", "preset", "Performance preset",
               "Sets the output and render resolution and the mirror updates in one step. Handheld suits the Odin: "
               "720p. Changing a size below makes it Custom.",
               c, 0);
    }
    choice(kTabGraphics, "Performance", "fps", "Frame rate",
           "60 FPS (deltatime) scales the game by elapsed time, so its speed stays right if the frame rate dips. "
           "Fixed tick uses the engine's own 60 Hz mode (speed follows the frame rate). Unlocked runs up to the frame cap.",
           {{"30", "30 FPS"}, {"60", "60 FPS"}, {"60fixed", "60 FPS (fixed tick)"}, {"unlocked", "Unlocked"}}, 1);
    slider(kTabGraphics, "Performance", "fps_cap", "Frame cap",
           "The highest frame rate with Unlocked. A little below what the game can hold gives the steadiest motion.", 30,
           std::max(display_hz > 60 ? display_hz : 144, 60), 1, 60, " FPS");
    choice(kTabGraphics, "Resolution", "output", "Output resolution",
           "The size of the finished picture (with the menus and HUD). It is scaled to fill the screen.",
           {{"720p", "720p"}, {"768p", "768p"}, {"900p", "900p"}, {"default", "1080p (game)"}, {"1440p", "1440p"},
            {"1800p", "1800p"}, {"4k", "4K"}, {"5k", "5K"}, {"8k", "8K"}},
           0, Kind::List);
    choice(kTabGraphics, "Resolution", "render", "Render resolution",
           "The size the 3D scene is drawn at before it is scaled to the output: lower is faster, higher sharper. "
           "Sizes above 1080p use more memory; the memory patch is added for you.",
           {{"720p", "720p"}, {"768p", "768p"}, {"900p", "900p"}, {"default", "1080p (game)"}, {"1440p", "1440p"},
            {"1800p", "1800p"}, {"4k", "4K"}, {"8k", "8K"}},
           0, Kind::List);
    choice(kTabGraphics, "Image quality", "aa", "Anti-aliasing",
           "FSR 3.1 replaces the game's anti-aliasing for a sharper, steadier picture and makes Upscaling possible.",
           {{"off", "Game"}, {"fsr3", "FSR 3.1"}}, 0);
    choice(kTabGraphics, "Image quality", "upscale", "Upscaling",
           "Draws the game at a lower resolution and FSR rebuilds it at the output size: more FPS for nearly the same "
           "picture. Native draws every pixel.",
           {{"native", "Native"}, {"quality", "Quality"}, {"balanced", "Balanced"}, {"performance", "Performance"},
            {"ultra_performance", "Ultra Perf."}},
           0);
    slider(kTabGraphics, "Image quality", "sharpness", "Sharpness",
           "How crisp the picture looks with FSR 3.1. 80 is recommended; very high values make edges rough.", 0, 100, 5,
           80, "");
    choice(kTabGraphics, "Image quality", "aniso", "Anisotropic filtering",
           "Keeps road and terrain textures sharp at a shallow angle. Automatic lets the runtime choose.",
           {{"", "Automatic"}, {"0", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}, 0);
    choice(kTabGraphics, "Rendering", "mirror", "Mirror and reflection updates",
           "The mirrors, rain and reflections are a second small scene drawn every frame. Static draws it once and keeps "
           "it: much faster, but reflections and rain on the glass stop updating.",
           {{"live", "Live"}, {"static", "Static (faster)"}}, 1);
    toggle(kTabGraphics, "Rendering", "two_thread", "Two-thread rendering",
           "Records each frame's draw commands on a second thread. Turn it off if you see flicker or broken graphics.",
           true);
    // ---- Graphics: frame generation by lsfg-vk, a Vulkan layer installed on the system, with the shaders of Lossless
    // Scaling (Steam). run.sh sets its variables for the game only (docs/ARM64_LINUX.md, "Frame generation").
    toggle(kTabGraphics, "Frame generation", "lsfg", "Lossless Scaling frame generation",
           "lsfg-vk adds generated frames between the game's own: smoother motion, a little more input lag. Needs "
           "lsfg-vk installed and Lossless Scaling owned and installed in Steam (any library, the SD card too).",
           false);
    {
        std::string hint = "Frames shown for each frame the game draws. The game's frame rate times this should match "
                           "the screen";
        const int hz = display_hz > 0 ? display_hz : 120;
        hint += display_hz > 0 ? " (" + std::to_string(hz) + " Hz here)" : "";
        hint += hz >= 120 ? ": 60 FPS × 2 or 30 FPS × 4 for 120 Hz." : ": 30 FPS × 2 for 60 Hz.";
        hint += " Above the refresh rate the game itself slows down.";
        choice(kTabGraphics, "Frame generation", "lsfg_multiplier", "Multiplier", hint.c_str(),
               {{"2", "2x"}, {"3", "3x"}, {"4", "4x"}}, 0);
    }
    slider(kTabGraphics, "Frame generation", "lsfg_flow_scale", "Flow scale",
           "The resolution motion is estimated at: lower is faster, 100% follows fine detail best. 70 to 80% suits a "
           "handheld.",
           25, 100, 5, 100, "%");
    toggle(kTabGraphics, "Frame generation", "lsfg_performance", "Performance mode",
           "A lighter frame generation model: much faster on a handheld, slightly softer generated frames.", false);
    add(kTabGraphics, "Frame generation", "@lsfg_check", "Check lsfg-vk and Lossless Scaling",
        "Shows what was found: the lsfg-vk layer, its library and Lossless.dll, or what is missing.", Kind::Button);

    // ---- Detail: an edited copy of the game's global.rpk (launcher/graphics.cpp), built from your own dump.
    toggle(kTabDetail, "Models", "detail.own_car", "Your car: full detail",
           "Draws your car with its most detailed model in races; the PS4 version uses the next one down.", false);
    toggle(kTabDetail, "Models", "detail.opponents", "Other cars: full detail",
           "The other cars get their most detailed models up close too.", false);
    choice(kTabDetail, "Models", "detail.scenery", "Scenery detail (less pop-in)",
           "Trees, rocks and roadside objects keep their detail further away. High: the pre-race camera's values; "
           "Ultra: photo mode's.",
           {{"game", "Game"}, {"high", "High"}, {"ultra", "Ultra"}}, 0);
    toggle(kTabDetail, "Models", "detail.reflections", "High-quality reflections",
           "Photo mode's finer reflections on the road and the cars, in races.", false);
    choice(kTabDetail, "Models", "detail.shadows", "Shadow edges",
           "Smoother shadow edges further than 4 m: the game uses 8 samples, High 16, Ultra 32.",
           {{"game", "Game"}, {"high", "High"}, {"ultra", "Ultra"}}, 0);
    toggle(kTabDetail, "Models", "detail.particles", "More particles",
           "Keeps distant spray, smoke and dust and allows 1.5 times as many particles.", false);
    toggle(kTabDetail, "Models", "detail.dynamic_res", "Dynamic resolution",
           "On (as on PS4), the game lowers the resolution for a moment when a frame takes too long.", true);
    toggle(kTabDetail, "Camera effects", "detail.motion_blur", "Motion blur",
           "The game's motion blur on the road and the scenery. Stays on with FSR 3.1, which needs the same shader.", true);
    toggle(kTabDetail, "Camera effects", "detail.lens_dust", "Lens dust", "Dirt on the camera lens when you face the sun.",
           true);
    toggle(kTabDetail, "Camera effects", "detail.heat_haze", "Heat haze", "The shimmer over hot roads in the distance.",
           true);
    toggle(kTabDetail, "Camera effects", "detail.fxaa", "FXAA pass",
           "With the game's anti-aliasing, an FXAA pass softens the picture after it. Off is a little sharper.", true);

    // ---- Game
    {
        std::vector<Choice> c;
        for (int i = 0; i < 21; ++i) c.push_back({std::to_string(i), kLanguages[i]});
        choice(kTabGame, "Player", "language", "Language",
               "The language of the game's menus and text (the console language). Japanese, Korean and Chinese may "
               "show missing characters.",
               c, 1, Kind::List);
    }
    {
        Row& r = add(kTabGame, "Player", "name", "Player name", "The name the game shows for you.", Kind::Text);
        r.text = r.text_def = "Player";
    }
    toggle(kTabGame, "Content", "dlc_unlock", "Unlock DLC content",
           "Makes the add-on cars, bikes, tours and livery decals playable. Built from your own game files; the game "
           "folders are not changed. Cars you earn by playing still unlock as you progress.",
           false);
    toggle(kTabGame, "Content", "fast_car_switch", "Faster car switching",
           "Shorter blur animations when you switch cars in the car select. Built from your own game files.", false);
    toggle(kTabGame, "On screen", "show_fps", "FPS counter", "The frame rate in a corner of the screen while playing.",
           false);
    toggle(kTabGame, "On screen", "mangohud", "MangoHud overlay",
           "MangoHud's performance overlay (frame times, CPU and GPU load).", false);
    for (const auto& c : kCurated) {
        bool def = false;
        for (const auto& p : patches) {
            if (p.name == c.name) def = p.default_on;
        }
        toggle(kTabGame, "Community patches", (std::string("patch:") + c.name).c_str(), c.label, c.hint, def);
    }

    // ---- Driving
    choice(kTabDriving, "Assists", "@drive_preset", "Driving preset",
           "Sets the four rows below in one step. Sim: manual gears, no assists, hardcore handling. Assisted: automatic "
           "gears, brake and steering help. Game Defaults: the game's own Options menu decides.",
           {{"sim", "Sim"}, {"assisted", "Assisted"}, {"game", "Game Defaults"}, {"custom", "Custom"}}, 2);
    choice(kTabDriving, "Assists", "gearbox", "Gearbox",
           "In-game: what the game's Options menu says. Manual is for paddles or a shifter.",
           {{"game", "In-game"}, {"automatic", "Automatic"}, {"manual", "Manual"}}, 0);
    choice(kTabDriving, "Assists", "brake_assist", "Brake assist",
           "In-game: what the game's Options menu says. The assist brakes for you before corners.",
           {{"game", "In-game"}, {"off", "Off"}, {"low", "Low"}, {"medium", "Medium"}, {"high", "High"}}, 0);
    choice(kTabDriving, "Assists", "hardcore", "Hardcore handling",
           "In-game: what the game's Options menu says. On: the cars handle more like real ones, with fewer hidden "
           "helpers.",
           {{"game", "In-game"}, {"off", "Off"}, {"on", "On"}}, 0);
    choice(kTabDriving, "Assists", "wheel_assist", "Wheel assist",
           "In-game: what the game's Options menu says. The game's steering help for steering wheels.",
           {{"game", "In-game"}, {"off", "Off"}, {"on", "On"}}, 0);

    // ---- Advanced
    add(kTabAdvanced, "Patches", "@patches", "All community patches",
        "Every patch in DriveClub.xml, on or off. The frame rate, resolution and memory patches follow the Graphics tab.",
        Kind::Button);
    toggle(kTabAdvanced, "Display", "fullscreen", "Fullscreen", "Off: the game plays in a window.", true);
    toggle(kTabAdvanced, "Display", "overlays", "Let overlays into the game",
           "Vulkan layers that add themselves (capture and overlay tools) are kept out of the game unless this is on.",
           false);
    choice(kTabAdvanced, "Guest CPU (FEX)", "env.FEX_TSOENABLED", "x86 memory ordering (TSO)",
           "Emulates the x86 memory order between the game's threads. Off is faster, but the game may then crash or "
           "misbehave.",
           {{"", "On"}, {"0", "Off"}}, 0);
    choice(kTabAdvanced, "Guest CPU (FEX)", "env.FEX_MULTIBLOCK", "Multiblock compilation",
           "Compiles larger pieces of the game's code at once: faster code, longer compile pauses.", {{"", "On"}, {"0", "Off"}},
           0);
    choice(kTabAdvanced, "Guest CPU (FEX)", "env.FEX_VECTORTSOENABLED", "Vector memory ordering",
           "With TSO on, also orders vector loads and stores. Slower; only for diagnosing crashes.",
           {{"", "Off"}, {"1", "On"}}, 0);
    add(kTabAdvanced, "Guest CPU (FEX)", "@env", "Environment variables",
        "Your own variables for the runtime (BB_...) or FEX (FEX_...): the env. lines of driveclub.conf.", Kind::Button);
    add(kTabAdvanced, "Tools", "@cpu_selftest", "CPU self-test",
        "Runs a few x86-64 functions through FEX without the game, then measures its speed.", Kind::Button);
    add(kTabAdvanced, "Tools", "@vulkan_info", "Vulkan info", "The Vulkan driver and device the game will use.",
        Kind::Button);
    add(kTabAdvanced, "Tools", "@log", "Latest log", "The end of the newest log in the logs folder.", Kind::Button);
    add(kTabAdvanced, "Maintenance", "@reprepare", "Prepare the game again",
        "Converts the game image from your dumps again at the next start (as on the first start).", Kind::Button);
    add(kTabAdvanced, "Maintenance", "@reset", "Reset all settings",
        "Every setting back to its default: the game as it starts without the launcher. The game folders are kept.",
        Kind::Button);

    // ---- All patches (a page of its own)
    managed_.clear();
    for (const auto& p : patches) {
        const std::string& n = p.name;
        if (n.find("Output resolution") != std::string::npos || n.find("Internal resolution") != std::string::npos ||
            n.rfind("Memory for up to", 0) == 0 || n.rfind("60 FPS with", 0) == 0) {
            if (n.find("Internal + Output") == std::string::npos) managed_.push_back(n);
        }
    }
    for (const auto& p : patches) {
        if (Contains(managed_, p.name)) continue;
        if (p.always) {
            Row& r = add(kPagePatches, "", "@always", p.name.c_str(), "Always applied: an internal fix.", Kind::Info);
            r.text = "Always";
            if (!p.note.empty()) r.hint = p.note;
            continue;
        }
        Row& r = toggle(kPagePatches, "", ("patch:" + p.name).c_str(), p.name.c_str(),
                        p.note.empty() ? "A community patch for DriveClub 1.28." : p.note.c_str(), p.default_on);
        (void)r;
    }
}

bool Model::IsSetting(int row) const {
    const Row& r = rows[row];
    if (r.id.empty() || r.id[0] == '@') return r.id == "@drive_preset";
    return r.kind != Kind::Folder && r.kind != Kind::Info && r.kind != Kind::Button;
}

bool Model::IsDefault(int row) const {
    const Row& r = rows[row];
    if (r.kind == Kind::Text) return r.text == r.text_def;
    return r.sel == r.def;
}

std::string Model::Locked(int row) const {
    const Row& r = rows[row];
    auto sel = [&](const char* id) {
        const int i = Find(id);
        return i < 0 ? std::string() : rows[i].choices.empty() ? std::to_string(rows[i].sel) : rows[i].choices[rows[i].sel].value;
    };
    const bool fsr = sel("aa") == "fsr3";
    if (r.id == "fps_cap" && sel("fps") != "unlocked") return "Used with the Unlocked frame rate.";
    if ((r.id == "upscale" || r.id == "sharpness") && !fsr) return "Needs FSR 3.1 anti-aliasing.";
    if (r.id == std::string("patch:") + kBlurPatch && fsr) return "FSR 3.1 needs the game's blur pass: it stays on with FSR.";
    if (r.id == "detail.motion_blur" && fsr) return "FSR 3.1 needs the game's motion blur shader: it stays on with FSR.";
    if (r.id == "env.FEX_VECTORTSOENABLED" && sel("env.FEX_TSOENABLED") == "0") return "Needs TSO emulation on.";
    if ((r.id == "lsfg_multiplier" || r.id == "lsfg_flow_scale" || r.id == "lsfg_performance") && sel("lsfg") != "1")
        return "Turn on Lossless Scaling frame generation first.";
    return "";
}

void Model::Reset(int row) {
    Row& r = rows[row];
    if (r.kind == Kind::Text) r.text = r.text_def;
    else if (r.id == "@drive_preset") ApplyDrivePreset(r.def);
    else r.sel = r.def;
    Changed(row);
}

void Model::SetChoice(int row, int index) {
    Row& r = rows[row];
    if (index < 0 || index >= int(r.choices.size())) return;
    if (r.id == "preset") {
        if (index < kPresetCount) ApplyPreset(index);
        return;
    }
    if (r.id == "@drive_preset") {
        if (index < 3) ApplyDrivePreset(index);
        return;
    }
    r.sel = index;
    Changed(row);
}

void Model::SetText(int row, const std::string& text) {
    rows[row].text = text;
    Changed(row);
}

bool Model::Step(int row, int dir) {
    Row& r = rows[row];
    if (!Locked(row).empty()) return false;
    switch (r.kind) {
    case Kind::Slider: {
        const int v = std::clamp(r.sel + dir * r.step, r.min, r.max);
        if (v == r.sel) return false;
        r.sel = v;
        Changed(row);
        return true;
    }
    case Kind::Toggle:
        r.sel = r.sel ? 0 : 1;
        Changed(row);
        return true;
    case Kind::Cycle:
    case Kind::List: {
        // The presets skip their "Custom" (it is what the rows below show when they match no preset).
        int n = int(r.choices.size());
        if (r.id == "preset") n = kPresetCount;
        if (r.id == "@drive_preset") n = 3;
        int i = r.sel >= n ? (dir > 0 ? 0 : n - 1) : (r.sel + dir + n) % n;
        SetChoice(row, i);
        return true;
    }
    default:
        return false;
    }
}

void Model::Changed(int row) {
    const std::string id = rows[row].id;
    if (id == "output" || id == "render") DerivePreset();
    for (const char* d : kDriveRows) {
        if (id == d) DeriveDrivePreset();
    }
    // A patch on two pages (a curated row and its row in the full list): keep them the same.
    if (id.rfind("patch:", 0) == 0) {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (int(i) != row && rows[i].id == id) rows[i].sel = rows[row].sel;
        }
    }
}

void Model::ApplyPreset(int preset) {
    const Preset& p = PresetAt(preset);
    const int o = Find("output"), r = Find("render"), m = Find("mirror"), s = Find("preset");
    rows[o].sel = std::max(0, ChoiceOf(rows[o], p.output));
    rows[r].sel = std::max(0, ChoiceOf(rows[r], p.render));
    rows[m].sel = std::max(0, ChoiceOf(rows[m], p.mirror));
    rows[s].sel = preset;
}

void Model::DerivePreset() {
    const int o = Find("output"), r = Find("render"), s = Find("preset");
    rows[s].sel = kPresetCount; // Custom
    for (int p = 0; p < kPresetCount; ++p) {
        if (rows[o].choices[rows[o].sel].value == kPresets[p].output &&
            rows[r].choices[rows[r].sel].value == kPresets[p].render) {
            rows[s].sel = p;
            break;
        }
    }
}

void Model::ApplyDrivePreset(int preset) {
    for (int k = 0; k < 4; ++k) rows[Find(kDriveRows[k])].sel = kDrive[preset][k];
    rows[Find("@drive_preset")].sel = preset;
}

void Model::DeriveDrivePreset() {
    const int s = Find("@drive_preset");
    rows[s].sel = 3; // Custom
    for (int p = 0; p < 3; ++p) {
        bool same = true;
        for (int k = 0; k < 4; ++k) same = same && rows[Find(kDriveRows[k])].sel == kDrive[p][k];
        if (same) {
            rows[s].sel = p;
            break;
        }
    }
}

void Model::Load(const Conf& conf) {
    const std::vector<std::string> on = SplitList(conf.Get("patches_on")), off = SplitList(conf.Get("patches_off"));
    for (size_t i = 0; i < rows.size(); ++i) {
        Row& r = rows[i];
        if (r.kind == Kind::Folder) {
            r.text = conf.Get(r.id);
            continue;
        }
        if (r.id.rfind("patch:", 0) == 0) {
            const std::string name = r.id.substr(6);
            r.sel = Contains(off, name) ? 0 : Contains(on, name) ? 1 : r.def;
            continue;
        }
        if (!IsSetting(int(i)) || r.id == "preset" || r.id == "output" || r.id == "render" || r.id == "mirror" ||
            r.id == "@drive_preset") {
            continue;
        }
        if (r.kind == Kind::Text) {
            r.text = conf.Has(r.id) ? conf.Get(r.id) : r.text_def;
        } else if (r.kind == Kind::Slider) {
            r.sel = conf.Has(r.id) ? std::clamp(std::atoi(conf.Get(r.id).c_str()), r.min, r.max) : r.def;
        } else {
            const int c = conf.Has(r.id) ? ChoiceOf(r, conf.Get(r.id)) : -1;
            r.sel = c >= 0 ? c : r.def;
        }
    }
    // The sizes: a preset's own unless the file names them (as run.sh reads them).
    int preset = 0;
    const std::string p = conf.Get("preset", "handheld");
    for (int k = 0; k < kPresetCount; ++k) {
        if (p == kPresets[k].value) preset = k;
    }
    if (p == kCustom.value) preset = kPresetCount;
    const Preset& pr = PresetAt(preset);
    for (const auto& [id, fallback] : {std::pair<const char*, const char*>{"output", pr.output}, {"render", pr.render},
                                       {"mirror", pr.mirror}}) {
        Row& r = rows[Find(id)];
        int c = ChoiceOf(r, conf.Get(id, fallback));
        if (c < 0) c = ChoiceOf(r, fallback);
        r.sel = std::max(0, c);
    }
    DerivePreset();
    DeriveDrivePreset();
}

void Model::Store(Conf& conf) const {
    std::vector<std::string> on = SplitList(conf.Get("patches_on")), off = SplitList(conf.Get("patches_off"));
    std::set<std::string> done;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r.kind == Kind::Folder) {
            if (r.text.empty()) conf.Erase(r.id);
            else conf.Set(r.id, r.text);
            continue;
        }
        if (r.id.rfind("patch:", 0) == 0) {
            const std::string name = r.id.substr(6);
            if (!done.insert(name).second) continue;
            Remove(on, name);
            Remove(off, name);
            if (r.sel != r.def) (r.sel ? on : off).push_back(name);
            continue;
        }
        if (!IsSetting(int(i)) || r.id == "preset" || r.id == "output" || r.id == "render" || r.id == "mirror" ||
            r.id == "@drive_preset") {
            continue;
        }
        if (r.kind == Kind::Text) {
            if (r.text == r.text_def) conf.Erase(r.id);
            else conf.Set(r.id, r.text);
        } else if (r.kind == Kind::Slider) {
            if (r.sel == r.def) conf.Erase(r.id);
            else conf.Set(r.id, std::to_string(r.sel));
        } else if (r.sel == r.def || r.choices[r.sel].value.empty()) {
            conf.Erase(r.id);
        } else {
            conf.Set(r.id, r.choices[r.sel].value);
        }
    }
    // The preset, and the sizes where they differ from the preset's.
    const int preset = rows[Find("preset")].sel;
    const Preset& pr = PresetAt(preset);
    if (preset == 0) conf.Erase("preset");
    else conf.Set("preset", pr.value);
    for (const auto& [id, preset_value] : {std::pair<const char*, const char*>{"output", pr.output}, {"render", pr.render},
                                           {"mirror", pr.mirror}}) {
        const Row& r = rows[Find(id)];
        const std::string& v = r.choices[r.sel].value;
        if (v == preset_value) conf.Erase(id);
        else conf.Set(id, v);
    }
    if (on.empty()) conf.Erase("patches_on");
    else conf.Set("patches_on", JoinList(on));
    if (off.empty()) conf.Erase("patches_off");
    else conf.Set("patches_off", JoinList(off));
}

std::string Model::ValueText(int row) const {
    const Row& r = rows[row];
    switch (r.kind) {
    case Kind::Slider:
        return std::to_string(r.sel) + r.unit;
    case Kind::Text:
    case Kind::Folder:
    case Kind::Info:
        return r.text;
    case Kind::Button:
        return "";
    default:
        return r.sel >= 0 && r.sel < int(r.choices.size()) ? r.choices[r.sel].label : "";
    }
}

std::string Model::Summary() const {
    auto v = [&](const char* id) { return ValueText(Find(id)); };
    std::string s = v("preset") + " preset · " + v("fps");
    if (rows[Find("fps")].choices[rows[Find("fps")].sel].value == "unlocked") s += " (cap " + v("fps_cap") + ")";
    s += " · output " + v("output") + ", render " + v("render");
    if (rows[Find("aa")].choices[rows[Find("aa")].sel].value == "fsr3") s += " · FSR 3.1 " + v("upscale");
    if (rows[Find("lsfg")].sel == 1) s += " · frame generation " + v("lsfg_multiplier");
    s += " · " + v("language");
    return s;
}

int Model::ChangedPatches() const {
    int n = 0;
    for (const auto& r : rows) {
        if (r.page == kPagePatches && r.kind == Kind::Toggle && r.sel != r.def) ++n;
    }
    return n;
}
