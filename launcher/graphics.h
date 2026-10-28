// Graphics detail the PS4 version keeps low, and the game's camera effects: an edited copy of the game's global.rpk
// with other values in its quality files (viewportquality.xml, vehiclelodgroupdata.xml).
#pragma once
#include <filesystem>
#include <string>

// -1 is Low where a setting has it: cheaper than the game, for slow GPUs (the Linux launcher's Detail tab).
struct GraphicsChoice {
  bool own_car = false;       // your car's full-detail model in races
  int opponents = 0;          // the other cars: -1 one detail step coarser, 0 game, 1 full detail up close
  int scenery = 0;            // -1 simpler models sooner, 0 game, 1 more detail further away (less pop-in), 2 photo mode's
  bool reflections = false;   // the higher-quality screen-space reflections of photo mode
  int shadows = 0;            // -1 the nearest cascade with 8 samples, 0 game, beyond 4 m: 1 16 samples, 2 32 samples
  int particles = 0;          // -1 half the particle limit, 0 game, 1 distant spray, smoke and dust kept, more particles
  bool dynamic_res = true;    // the game's dynamic resolution scaling (on in the game)
  bool motion_blur = true;    // the game's motion blur
  bool lens_dust = true;      // dirt on the camera lens against the sun
  bool heat_haze = true;      // heat shimmer over hot roads
  bool fxaa = true;           // the FXAA pass after the game's own anti-aliasing
  bool Default() const {
    return !own_car && !opponents && !scenery && !reflections && !shadows && !particles && dynamic_res &&
           motion_blur && lens_dust && heat_haze && fxaa;
  }
};

// Reads global.rpk from the player's own game archive and writes `out_dir`\global.rpk with the chosen values.
// The runtime serves it in place of the original (as a mods folder); the game folders are never touched. When
// every choice is at the game's default, the file is removed and nothing needs to be served (`active` false).
// Skips the work when the file in `out_dir` already matches the archive and the choices. Also writes
// `out_dir`/changes.txt (every value changed: old -> new, and the Low changes this archive has no value for) and the
// game's own quality file as viewportquality.xml.orig, for checking. Returns "" or a short description of a problem.
std::wstring BuildGraphics(const std::filesystem::path& update_dir, const std::filesystem::path& base_dir,
                           const std::filesystem::path& out_dir, const GraphicsChoice& choice, bool& active);
