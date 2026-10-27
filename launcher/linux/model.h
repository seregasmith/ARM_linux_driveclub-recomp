// The launcher's settings: one Row per line on screen, grouped into tabs, read from and written to driveclub.conf.
// The defaults are the game as run.sh starts it with an empty driveclub.conf (docs/LAUNCHER_PLAN.md, "Defaults").
#pragma once

#include "conf.h"

#include <string>
#include <vector>

enum Tab { kTabPlay, kTabGraphics, kTabDetail, kTabGame, kTabDriving, kTabAdvanced, kTabCount, kPagePatches = kTabCount, kPageCount };
extern const char* const kTabName[kTabCount];

enum class Kind {
    Cycle,  // left/right steps through the choices
    List,   // A opens the list of choices
    Slider, // left/right steps between min and max
    Toggle, // A or left/right
    Text,   // A opens the on-screen keyboard
    Button, // A does something (the app handles the row's id)
    Folder, // a game folder: A opens the folder chooser
    Info,   // text only
};

struct Choice {
    std::string value; // in driveclub.conf ("" = the key is left out)
    std::string label; // on screen
};

struct Row {
    std::string id;      // the driveclub.conf key, "patch:<name>", or "@..." for rows the app handles
    int page = kTabPlay; // a tab, or kPagePatches
    std::string section; // a header above the row when it differs from the row before's
    std::string label, hint;
    Kind kind = Kind::Info;
    std::vector<Choice> choices;
    int def = 0, sel = 0;            // Cycle, List, Toggle: choice index; Slider: the value
    int min = 0, max = 0, step = 1;  // Slider
    std::string unit;                // after a Slider's value
    std::string text, text_def;      // Text and Folder rows: the value; Info rows: the text on the right
    bool primary = false;            // Button: drawn as the big accent button (Play)
};

struct PatchInfo {
    std::string name, note;
    bool default_on = false, always = false;
};

class Model {
public:
    // `display_hz`: the screen's refresh rate (the frame cap's top), 0 when unknown.
    void Build(const std::vector<PatchInfo>& patches, int display_hz);
    void Load(const Conf& conf);
    void Store(Conf& conf) const;

    std::vector<Row> rows;
    int Find(const std::string& id) const;
    std::vector<int> Page(int page) const;

    // "" when the row can be changed, else why it cannot (shown in the help bar; the row is dimmed).
    std::string Locked(int row) const;
    // Settings rows only: whether the value is the default.
    bool IsSetting(int row) const;
    bool IsDefault(int row) const;
    void Reset(int row);
    // Left/right on a row: false when nothing changed.
    bool Step(int row, int dir);
    void SetChoice(int row, int index);
    void SetText(int row, const std::string& text);
    std::string ValueText(int row) const;
    // A short summary of the main choices for the Play tab.
    std::string Summary() const;
    // Patches changed in the full list (not counting the curated rows' own).
    int ChangedPatches() const;

private:
    void Changed(int row);
    void ApplyPreset(int preset);
    void DerivePreset();
    void ApplyDrivePreset(int preset);
    void DeriveDrivePreset();
    std::vector<PatchInfo> patches_;
    std::vector<std::string> managed_; // patch names the Graphics tab sets (kept out of the full list)
};
