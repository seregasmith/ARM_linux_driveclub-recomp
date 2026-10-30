# Linux launcher: plan

Status: **implemented** (2026-10-27): `launcher/linux/` (dc-launcher), `run.sh`, `tools/dc_prepare.cpp` and the
portable `launcher/datx.cpp`, `unlock.cpp`, `graphics.cpp`, `carswitch.cpp` (`launcher/fileio.h`). Tested
off-screen; not yet on the device. Where the implementation differs from the plan below:

- X has no "longer explanation": the help bar shows each setting's whole text. X deletes in the on-screen keyboard.
- The launcher writes only the settings that differ from their defaults; run.sh changes only the folder lines.
- The Play tab shows the main settings under the Play button instead of separate status lines.
- `dc-prepare --unlock`, `--car-switch`, `--detail KEY=VALUE` (instead of folder arguments): the folders are fixed
  (`<data>/unlock`, `carswitch`, `graphics`) and `prep/mods.txt` lists the ones the runtime serves.
- The folder browser's B goes back to the previous folder; Y uses the focused folder.

The original plan follows.

Today the AppImage goes straight into the game and every setting is a line in `driveclub.conf`. This plan adds a
launcher in the spirit of the Windows one (`launcher/driveclub_launcher.cpp`), made for a handheld: it runs in
Steam's Game Mode on Armada OS (SteamOS on ARM) and is driven with the gamepad first.

## Requirements

- Every setting can be changed with a gamepad through Steam Input. Mouse, touchscreen and keyboard work too, but the
  gamepad comes first.
- Settings: language, player name, resolutions, frame rate and frame cap, image quality, the performance switches,
  on-screen info, driving assists, the community patches; the add-on content unlock, graphics detail and faster car
  switching (ported from the Windows launcher); an Advanced tab.
- Game folders (the 1.28 update and the base game) are found automatically, with a gamepad folder browser when not.
- A Play button. After the game exits the player is back in Steam.

## What it is

A new program, `dc-launcher`, built by `build.sh` and packaged in the AppImage next to `DriveClubGame`.

- SDL3 for the window and the controllers, Dear ImGui for drawing. Both are in the project already: SDL3 is a
  runtime dependency, ImGui (`gpu/third_party/imgui`, with its SDL3 and SDL_Renderer backends) and the font with
  Cyrillic (`gpu/third_party/fonts/DejaVuSans.ttf`) are used by the in-game overlay. Nothing new goes into the
  AppImage, and it starts instantly (native ARM64).
- SDL_Renderer draws through Vulkan (the same bundled Turnip the game uses), falling back to OpenGL ES.
- The launcher keeps its own focus model: a list of rows per tab, which row is focused, what left/right and A do.
  ImGui only lays out and draws them (and handles mouse and touch hits). This gives console-like, predictable
  gamepad navigation instead of ImGui's general-purpose one.
- `driveclub.conf` stays the only settings file. The launcher reads it, edits it and writes it back; `run.sh`
  applies it. Editing the file by hand and starting with `--play` keep working.

## Start flow

```
AppImage → dc_vulkan.py → run.sh ──(no --play)──► dc-launcher ──Play──► prepare (progress screen)
                                                                            │
                                         exec run.sh --play  ◄──────────────┘
                                                │
                                         DriveClubGame ──exit──► back in Steam
```

- The launcher opens with **Play** focused: one press of A starts the game.
- Play saves `driveclub.conf`, then runs the preparation in the launcher: `dc-prepare` (the game image, converted
  only when the folders change) and the add-on content, detail and car-switch files when they are on. A progress
  screen shows it; errors are shown in the launcher with their details. (Today `run.sh` reports errors on stderr or
  with zenity, which nobody sees in Game Mode.)
- Then the launcher replaces itself with `run.sh --play` (exec): the game runs in the launcher's process, so Steam
  sees one game, and when it exits the player is back in Steam.
- Skipping the launcher: `run.sh --play`, `DC_NO_LAUNCHER=1`, or any game folder option on the command line.
  `run.sh` exports `DC_NO_LAUNCHER=1` before it starts the game, so the runtime's own restart (`runtime_restart`,
  which runs `bash run.sh`) goes back into the game, not into the launcher.

## Controls

| input | action |
|---|---|
| D-pad / left stick up, down | move between rows |
| D-pad / left stick left, right | change the focused value (cycles, sliders, switches) |
| A | open a list, flip a switch, press a button |
| B | back: close a list or dialog; on a tab, "Quit to Steam?" |
| L1 / R1 | previous / next tab |
| L2 / R2 | page up / down in long lists |
| Start | Play, from any tab |
| X | the focused setting's longer explanation |
| Y | reset the focused setting to its default (see "Defaults") |
| keyboard | arrows, Enter (A), Esc (B), Tab / Shift+Tab (R1 / L1), F5 (Play) |
| mouse, touch | click or tap a row; tap a value to change it |

Button glyphs follow the Xbox layout (the Odin 3's). Controllers can be connected and disconnected at any time; any
connected one controls the launcher.

How each kind of setting behaves:

- **Choice, up to 6 values:** a `◀ 60 FPS ▶` row, left/right cycles.
- **Choice, more values** (language, output resolution, patch lists): A opens a list; up/down, A chooses, B cancels.
- **Slider** (frame cap, sharpness): left/right steps, holding speeds up.
- **Switch:** A or left/right.
- **Button** (Play, Change folder, the tools): A.
- **Text** (player name): A opens the on-screen keyboard (below).
- **Locked rows** are dimmed and the help bar says why (Upscaling needs FSR 3.1; Motion blur cannot change while
  FSR 3.1 is on; the frame cap needs the Unlocked frame rate; the frame generation rows need it switched on).

## Screen

- A row of tabs at the top (switched with L1/R1), the focused tab's rows below, and at the bottom a help bar: the
  focused row's explanation (the Windows launcher's texts) and the button hints (`Ⓐ Change  Ⓑ Back  L1/R1 Tabs
  START Play`).
- Text is scaled from the window height: made for the Odin 3's 6-inch 1920×1080 screen, readable at 1280×800 (Steam
  Deck) and in a desktop window.
- Dark theme with one accent colour, like the Windows launcher.

## Tabs

How each setting reaches the game: **patch** (a community patch from `patches/DriveClub.xml`, applied by
`dc-prepare`), **env** (a runtime variable set by `run.sh`), **file** (a file `run.sh` writes in `user/`), **mods** (a
folder of replacement game files built from the player's own archive and served by the runtime with `--mods`).

### 1. Play

| row | control | |
|---|---|---|
| Play | button (focused at start) | saves, prepares, starts the game |
| Update folder (1.28) | ✓ / ✗ and the path; A: change | see "Game folders" |
| Base game folder | ✓ / ✗ and the path; A: change | |
| Status lines | text | add-on content on, Vulkan driver (e.g. Turnip Adreno 830), warnings (16 KB-page kernel, no Vulkan device) |
| Quit | button | back to Steam |

### 2. Graphics

| row | values | reaches the game |
|---|---|---|
| Performance preset | Handheld, Low, Medium, High, Ultra, Custom | fills the rows below (Windows launcher's `ApplyPreset`): Handheld 720p output and render, static mirrors; Low 720p render; Medium 1440p output; High 4K output, 1440p render; Ultra 4K output and render. Changing a row below shows Custom |
| Frame rate | 30, 60, 60 (fixed tick), Unlocked | patch + env (`BB_FPS_LIMIT`, `BB_PRESENT_MODE`) |
| Frame cap | 30 … the display's refresh rate, default 60 (Unlocked only) | env `BB_FPS_LIMIT` |
| Output resolution | 720p, 768p, 900p, 1080p (game), 1440p, 1800p, 4K, 5K, 8K (list) | patch; the memory patch ("Memory for up to 1440p/4K/8K") and `BB_DMEM_MB` follow the larger of output and render, as in the Windows launcher's `MemoryTier` |
| Render resolution | 720p, 768p, 900p, 1080p (game), 1440p, 1800p, 4K, 8K (list) | patch (as above) |
| Anti-aliasing | Game, FSR 3.1 | env `BB_AA` |
| Upscaling | Native, Quality, Balanced, Performance, Ultra Perf. (FSR 3.1 only) | env `BB_AA_SCALE` |
| Sharpness | 0 … 100 (FSR 3.1 only) | env `BB_FSR_SHARPNESS` |
| Anisotropic filtering | Automatic, Off, 2x, 4x, 8x, 16x | env `BB_ANISO` |
| Mirror and reflection updates | Live, Static | env `BB_MIRROR_RATE` |
| Two-thread rendering | on, off | file `user/toggles.txt` |
| Lossless Scaling frame generation | on, off | lsfg-vk's environment configuration, its layer let in (`run.sh`; ARM64_LINUX.md, "Frame generation") |
| Multiplier | 2x, 3x, 4x (frame generation only) | env `LSFGVK_MULTIPLIER` |
| Flow scale | 25 … 100 % (frame generation only) | env `LSFGVK_FLOW_SCALE` |
| Performance mode | on, off (frame generation only) | env `LSFGVK_PERFORMANCE_MODE` |
| Check lsfg-vk and Lossless Scaling | button: what `run.sh --lsfg-status` found ("ready" / "not ready" on the row) | |

### 3. Detail (ported from `launcher/graphics.cpp`)

| row | values | reaches the game |
|---|---|---|
| Your car: full detail | on, off | mods (an edited `global.rpk`) |
| Other cars | Low, Game, Full detail | mods |
| Scenery detail | Low, Game, High, Ultra | mods; Low also `BB_CULL_SCALE=1` |
| High-quality reflections | on, off | mods |
| Shadows | Low, Game, High, Ultra | mods; Low also `BB_CULL_SCALE_SHADOW=1` |
| Particles | Low, Game, More | mods |
| Dynamic resolution | on, off | mods |
| Motion blur | on, off (locked on with FSR 3.1) | mods |
| Lens dust | on, off | mods |
| Heat haze | on, off | mods |
| FXAA pass | on, off | mods |

All at the game's defaults: no file is built and nothing is served.

### 4. Game

| row | values | reaches the game |
|---|---|---|
| Language | the game's 21 languages (list) | env `BB_LANGUAGE` |
| Player name | text (on-screen keyboard) | env `BB_USER_NAME` |
| Unlock DLC content | on, off | mods (ported `launcher/unlock.cpp`, `datx.cpp`) |
| Unlock every car and bike | on, off (v0.4.0; includes the add-on content) | mods (`dc-prepare --unlock-all`) |
| Faster car switching | off (default), on | mods (ported `launcher/carswitch.cpp`; always on in the Windows launcher, off here so the default stays as the game runs today) |
| Show FPS counter | on, off | file `user/driveclub.ini` |
| MangoHud overlay | on, off | env `MANGOHUD` |
| Disable motion blur and depth of field | on, off (locked off with FSR 3.1) | patch |
| No reset-to-track timer | on, off | patch |
| Disable track barriers | on, off | patch |
| Photo mode: free camera through cars | on, off | patch |
| Photo mode: skip processing | on, off | patch |

### 5. Driving

| row | values | reaches the game |
|---|---|---|
| Driving preset | Sim, Assisted, Game Defaults | fills the four rows below |
| Gearbox | In-game, Automatic, Manual | env `BB_GEARBOX` |
| Brake assist | In-game, Off, Low, Medium, High | env `BB_BRAKE_ASSIST` |
| Hardcore handling | In-game, Off, On | env `BB_HARDCORE` |
| Wheel assist | In-game, Off, On | env `BB_WHEEL_ASSIST` |

"In-game" leaves the setting to the game's own Options menu.

### 6. Advanced

| row | |
|---|---|
| All patches | every patch in `DriveClub.xml` with its description, on/off (a list; `dc-prepare --list`) |
| FEX options | TSO emulation on/off and the other useful `FEX_*` switches (stored as `env.FEX_...`) |
| Environment variables | the `env.NAME=value` lines: add, edit, remove |
| Fullscreen | on (default), off |
| Let overlays into the game | off (default), on (`VK_LOADER_LAYERS_DISABLE` unset) |
| CPU self-test | runs `dc-cpu-selftest`, shows its output |
| Vulkan info | the driver and device the game will get |
| Latest log | the end of the newest `logs/driveclub_*.log` |
| Prepare the game again | `dc-prepare --force` |
| Reset all settings | everything back to the defaults below, after a confirmation (the game folders are kept) |

## Game folders

- **First start, or a folder that is gone:** the launcher searches `$HOME` (a few levels deep), the SD card and USB
  drives (`/run/media/*/*`, `/media`, `/mnt`) for `sce_sys/param.sfo` with DriveClub's title ID (`CUSA00093` US,
  `CUSA00003` EU) and sorts what it finds into update (version `01.28`, `eboot.bin`) and base game (`game000.dat`).
  - One update and one base found: both are filled in and shown for confirmation.
  - Several: a list to choose from.
  - None: the folder browser opens.
- **Folder browser:** a full-screen list driven by the gamepad (A opens a folder, B goes up, Y uses this folder),
  with shortcuts to Home, the SD card and USB drives. Folders with `eboot.bin` or `game000.dat` are marked.
- **Checks:** the Windows launcher's folder validation and its messages (`driveclub_launcher.cpp`, the
  `CUSA00093/CUSA00003` checks, and `launcher/prepare.cpp`'s version 1.28 check). A 1.28 dump in one folder
  (`game000.dat` next to the 1.28 `eboot.bin`) serves as both.

## On-screen keyboard

For the player name: a gamepad keyboard grid in the launcher (Latin and Cyrillic layouts, X backspace, Y switches the
layout, Start done). A physical keyboard types directly. Steam's own keyboard (Steam + X) can be used too, but a
non-Steam program cannot open it reliably.

## Settings file

`driveclub.conf` keeps its keys; the launcher adds these (all optional; a missing key is the default):

| key | values |
|---|---|
| `output` | `720p`, `768p`, `900p`, `default`, `1440p`, `1800p`, `4k`, `5k`, `8k` |
| `render` | `720p`, `768p`, `900p`, `default`, `1440p`, `1800p`, `4k`, `8k` |
| `dlc_unlock`, `unlock_all` | `0`, `1` |
| `fast_car_switch` | `0`, `1` |
| `detail.own_car`, `detail.reflections` | `0`, `1` |
| `detail.opponents`, `detail.particles` | `low`, `0`, `1` |
| `detail.dynamic_res`, `detail.motion_blur`, `detail.lens_dust`, `detail.heat_haze`, `detail.fxaa` | `1`, `0` |
| `detail.scenery`, `detail.shadows` | `low`, `game`, `high`, `ultra` |
| `gearbox` | `game`, `automatic`, `manual` |
| `brake_assist` | `game`, `off`, `low`, `medium`, `high` |
| `hardcore`, `wheel_assist` | `game`, `off`, `on` |
| `overlays` | `0`, `1` |

`preset` stays a shortcut: when `output` and `render` are absent, `run.sh` applies the preset's sizes as today, so
files written by hand keep working.

## Defaults

The defaults are the game as it starts today, without the launcher (`run.sh` with an empty `driveclub.conf`), so
a fresh install, a reset row and "Reset all settings" all give exactly today's game.

| setting | default | what the game gets |
|---|---|---|
| Performance preset | Handheld | output 720p, render 720p, static mirrors |
| Frame rate | 60 | the "60 FPS with deltatime" patch, `BB_FPS_LIMIT=60` |
| Frame cap | 60 | used only with Unlocked |
| Output / render resolution | 720p / 720p | from the preset; no memory patch, `BB_DMEM_MB=5056` |
| Anti-aliasing | Game | `BB_AA=off` |
| Upscaling, Sharpness | Native, 80 | used only with FSR 3.1 |
| Anisotropic filtering | Automatic | `BB_ANISO` unset: the runtime's choice |
| Mirror and reflection updates | Static | from the preset (`BB_MIRROR_RATE=0`) |
| Two-thread rendering | on | `user/toggles.txt` |
| Lossless Scaling frame generation | off | (2x, flow scale 100 %, performance mode off when on) |
| Detail tab | all at the game's values | no `global.rpk` built |
| Language | English (US) | `BB_LANGUAGE` unset: the runtime's 1 |
| Player name | Player | |
| Unlock DLC content | off | |
| Unlock every car and bike | off | |
| Faster car switching | off | |
| Show FPS counter, MangoHud | off, off | |
| Curated patches | all off | |
| Driving preset and the four assists | Game Defaults, In-game | the game's own Options menu decides |
| All patches | `DriveClub.xml`'s own defaults | on: "Red zone fix" (always), "60 FPS with deltatime"; the preset adds the two 720p patches |
| FEX options, environment variables | none | no `env.` lines |
| Fullscreen | on | |
| Let overlays into the game | off | `VK_LOADER_LAYERS_DISABLE='~implicit~'` |

How to get back to them:

- **One setting:** Y on its row.
- **Everything:** Advanced → Reset all settings. It asks first, then writes `driveclub.conf` with only the game
  folders in it (the same file a first start makes), which also removes the `env.` lines and the patch choices.
- **By hand:** delete `driveclub.conf` except its `update=` and `base=` lines.

The launcher marks rows that differ from their default (a dot next to the value), so a changed setting is easy to
find.

## Changes outside the launcher

- `launcher/unlock.cpp`, `datx.cpp`, `graphics.cpp`, `carswitch.cpp`: Windows file calls replaced by portable C++
  (`std::filesystem`, `<fstream>`), with `std::filesystem::path` in their interfaces (it takes the Windows launcher's
  `std::wstring` paths as they are). The Windows launcher keeps building.
- `tools/dc_prepare.cpp`: `--unlock DIR`, `--detail DIR key=value...` and `--car-switch DIR` build those folders
  (skipped when already up to date, as on Windows).
- `run.sh`: starts `dc-launcher` unless `--play`/`DC_NO_LAUNCHER`; reads the new keys (resolutions, memory tier,
  assists, detail, add-on content, car switch, overlays) and passes the built folders with `--mods`. `DriveClubGame` takes
  4 `--mods` options (`src/probe.c`; the file layer up to 8): the player's `mods/`, add-on content, detail, car switch.
- `build.sh`: builds `out/dc-launcher` (ImGui's sources and the SDL3 backends, linked to SDL3).
- `packaging/default.nix`, `appimage.sh`: ship `dc-launcher`; the package check also renders every tab off-screen
  (`dc-launcher --selftest`, SDL's offscreen video driver).
- `docs/ARM64_LINUX.md`: the launcher replaces "no launcher window" in Running.
- Added after version 1: Low levels on the Detail tab (other cars, scenery, shadows, particles; ARM64_LINUX.md,
  "Tuning"), relative to the archive's own values; `graphics/changes.txt` lists what was changed.
- Added after version 1: Lossless Scaling frame generation (lsfg-vk, a layer installed on the system) on the
  Graphics tab; `run.sh --lsfg-status` finds lsfg-vk and `Lossless.dll` for the launcher's check and the game's start.

## Not in version 1

- Benchmark tab (the runtime's benchmark works through `env.BB_BENCHMARK...` meanwhile).
- The features that are Windows-only in the runtime: high-FPS camera workaround, field-of-view slider (F5/F6),
  motion-blur exposure, wheel setup and force feedback, DLSS, the runtime's own frame generation, HDR, update
  checker. (Lossless Scaling frame generation through lsfg-vk is on the Graphics tab.)
- Ultrawide resolutions and their FOV fix as rows (they stay available in Advanced → All patches).

## Phases

1. **Skeleton:** window, font, tabs, focus model, help bar, button hints; reads and writes `driveclub.conf`; Play;
   `run.sh` / AppImage wiring and the restart path.
2. **Settings that work today:** Graphics, Game, Driving with the dependency rules and the memory tier; on-screen
   keyboard; `run.sh` support for the new keys.
3. **Folders:** search, browser, first-start flow, progress and error screens.
4. **Ports:** add-on content unlock, Detail, car switch (`dc-prepare` options, the Detail tab, the Game tab rows).
5. **Advanced tab.**
6. **Testing** (below), then the Odin 3.

## Testing

- Screenshots of every tab and dialog, rendered off-screen in the build container, reviewed before anything goes to
  the device.
- A scripted walk through the menus (`dc-launcher --script`, gamepad events from a file): every row reachable, every
  value reachable and saved, Play writes the expected `driveclub.conf`.
- `run.sh` against stand-in binaries: the keys turn into the right patches, variables and `--mods` folders.
- On the Odin 3 in Game Mode: Steam Input's default gamepad layout, the built-in controls and an external
  controller; first start with the dumps on the SD card; Play, exit, back in Steam.

## Defaults chosen (can still change)

- Player name: the launcher's own on-screen keyboard (Latin and Cyrillic).
- Fullscreen is in Advanced (a handheld always plays fullscreen).
- No Benchmark tab in version 1.
- The launcher's text is in English, kept in one place so it can be translated later.
- Xbox button glyphs.
- Faster car switching starts off (the Windows launcher always applies it), so the defaults are today's game.
