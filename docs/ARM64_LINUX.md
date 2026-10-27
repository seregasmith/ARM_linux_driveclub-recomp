# DriveClub runtime on Linux and ARM64 (Odin 3, Armada OS)

This fork builds the DriveClub runtime as a native Linux program, and on ARM64 Linux (AYN Odin 3 on Armada OS,
other Snapdragon handhelds) runs the game's x86-64 code through **FEXCore** while everything else is native ARM64.
The approach and most of the guest-CPU code come from the Bloodborne ARM64 port
([seregasmith/ARM_linux_bbport](https://github.com/seregasmith/ARM_linux_bbport), branch `arm64-fex`), whose
runtime shares its origin with this one.

## Why not Proton on ARM64

With Proton (Wine + FEX/Box64 for the whole x86-64 Windows program) every instruction of `DriveClubGame.exe` is
emulated: the game, but also the PS4 system-library replacements, the PM4 command processor, the GCN-to-SPIR-V
shader recompiler, the texture detiling and all the Vulkan recording. That work is what the CPU spends a frame
on, so the frame rate stays around 20 whatever the render resolution (the GPU waits for the CPU).

Here only the game's own code runs in the x86-64 JIT. The runtime (`src/`), the GPU library (`gpu/`: shadPS4's
video core, shader recompiler, FSR) and Vulkan are compiled for ARM64 and run natively, the way
the bbport ARM64 port runs Bloodborne. Vulkan goes to the system's (or the AppImage's bundled) Turnip driver
directly, with no Wine/DXVK layers in between.

```
eboot.bin (x86-64)  ──►  FEXCore JIT (cpu/guest_cpu_fex.cpp, libbbcpu.so)
      │ imports: stubs that end in `syscall` → host functions (AAPCS64)
      ▼
runtime (src/, native ARM64) ── libkernel, libc glue, pad (SDL3), audio, files, saves
      │ sceGnmSubmitCommandBuffers
      ▼
GPU library (gpu/, native ARM64) ── PM4 → Vulkan, GCN → SPIR-V  ──►  Turnip (Adreno)
```

## Building

`build_docker.sh` builds in a Linux Docker container on any host (macOS with Docker Desktop, OrbStack or colima;
Linux with Docker Engine), with the toolchain from `shell.nix` pinned to one nixpkgs (the same revision as the
Bloodborne, inFAMOUS and Gravity Rush 2 ARM ports, so their Nix store volumes can be shared):

```bash
git submodule add https://github.com/FEX-Emu/FEX.git third_party/FEX      # once (aarch64 only)
git -C third_party/FEX checkout 9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf
git submodule update --init --recursive
./build_docker.sh                    # out/DriveClubGame, out/dc-prepare, out/dc-cpu-selftest, out/gpu/libbbgpu.so, out/fex/libbbcpu.so
./build_docker.sh appimage           # dist/DriveClub-recomp-aarch64.AppImage
./build_docker.sh --arch x86_64 appimage   # dist/DriveClub-recomp-x86_64.AppImage (Steam Deck, PCs)
```

The target defaults to Docker's own architecture (aarch64 on Apple Silicon). To reuse the Nix store another port's
script filled already (toolchain downloads and the patched Turnip): `DC_NIX_VOLUME=gr2-nix ./build_docker.sh` (or
`bbport-nix`, `iss-nix`). For the Odin 3's Adreno 830 build Turnip from the `turnip/gen8` branch:

```bash
git clone --depth 1 -b turnip/gen8 https://github.com/whitebelyash/mesa-unified ../mesa-unified
DC_TURNIP_SRC=../mesa-unified ./build_docker.sh appimage
```

`./build_docker.sh --help` lists every setting.

On an x86-64 Linux PC `bash build.sh` builds the same runtime natively (the game's code then runs on the host
CPU, no FEX), with the libraries from your distribution or from `nix-shell shell.nix`.

## Running

Copy the AppImage to the device, make it executable (`chmod +x DriveClub-recomp-aarch64.AppImage`) and add it to
Steam as a non-Steam game (Game Mode), or start it from a file manager.

It opens the **launcher** (`dc-launcher`, `launcher/linux/`, the plan in [LAUNCHER_PLAN.md](LAUNCHER_PLAN.md)):
a full-screen settings screen made for the gamepad (Steam Input's default gamepad layout works as it is; mouse,
touch and keyboard too). The first start searches your home folder, the SD card and USB drives for the DriveClub
dumps (the 1.28 update and the base game) and fills both folders in; otherwise a gamepad folder browser marks the
folders that hold them. **Play** (A on Play, or START anywhere) saves the settings, prepares the game (the first
time it converts the image: up to a minute, with a progress bar; errors are shown there) and starts it. When the
game exits you are back in Steam.

| button | in the launcher |
|---|---|
| D-pad / left stick | up/down: rows; left/right: change the value |
| A | choose, switch on/off, open a list, edit text |
| B | back; on a tab: "Quit to Steam?" |
| L1 / R1 | previous / next tab (Play, Graphics, Detail, Game, Driving, Advanced) |
| L2 / R2 | page up / down in long lists |
| START | Play |
| Y | the focused setting back to its default |

A dot before a setting marks one that is not at its default. Advanced → Reset all settings puts every setting back
(the game folders stay). The defaults are the game as it starts without the launcher. To skip the launcher (a
frontend, a second Steam entry): `./DriveClub-recomp-aarch64.AppImage --play`.

Without a desktop, or with `--update DIR --base DIR` (folders saved), the AppImage starts the game directly (with
zenity/kdialog folder pickers when the folders are missing and there is a desktop).

### Command-line options

| option | |
|---|---|
| (none) | the launcher (the game when there is no desktop) |
| `--play` | the game with the saved settings, without the launcher |
| `--launcher` | the launcher, whatever else is set (`DC_NO_LAUNCHER=1`: never) |
| `--update DIR --base DIR` | set the game folders (saved), then start the game |
| `--setup` | choose the game folders again (folder pickers) |
| `--config` | print the settings file's path and contents |
| `--list-patches` | the community patches (names for `patches_on`/`patches_off`) and their defaults |
| `--cpu-selftest` | check the guest CPU (FEXCore) without the game, then its speed |
| `--vulkan-info` | the Vulkan driver the game will use |
| `--help` | all options and settings |

Anything else is passed on to the runtime (and starts the game).

### Files

Everything lives in `~/.local/share/driveclub-recomp/` (`$XDG_DATA_HOME/driveclub-recomp`, or `DC_DATA_DIR`):

| path | |
|---|---|
| `driveclub.conf` | the settings (below) |
| `logs/driveclub_<date>_<time>.log` | one log per start |
| `prep/` | the game image prepared from the dumps (converted again when the folders change) |
| `unlock/`, `carswitch/`, `graphics/` | replacement game files built from your dumps (DLC unlock, faster car switching, graphics detail) |
| `user/savedata/` | save games |
| `user/shader/` | the shader cache |
| `user/patches_enabled.txt` | the community patches in use (written at each start from the settings) |
| `mods/` | loose-file mods: files here replace the game's own files of the same path |

### Settings (`driveclub.conf`)

The launcher edits it (and writes only the settings that differ from their defaults); `run.sh` reads it at every
start. It can also be edited by hand (the launcher keeps keys it does not know, but not comments), e.g. the
language from a terminal:

```bash
f=~/.local/share/driveclub-recomp/driveclub.conf
grep -q '^language=' "$f" && sed -i 's/^language=.*/language=8/' "$f" || echo 'language=8' >> "$f"
```

| key | default | values |
|---|---|---|
| `update` | | the version 1.28 update dump (with `eboot.bin`); may be the same folder as `base` |
| `base` | | the base game dump (with `game000.dat`) |
| `preset` | `handheld` | `handheld` (720p render and output, static mirrors), `low` (720p render), `medium` (1440p output), `high` (4K output, 1440p render), `ultra` (4K output and render), `custom` (the sizes below) |
| `output` | the preset's | `720p`, `768p` (1360x768), `900p` (1600x900), `default` (1080p), `1440p`, `1800p`, `4k`, `5k`, `8k`; above 1080p the memory patch is added |
| `render` | the preset's | `720p`, `768p`, `900p`, `default` (1080p), `1440p`, `1800p`, `4k`, `8k` |
| `fps` | `60` | `30`, `60` (60 FPS with the deltatime patch), `60fixed` (60 FPS with a fixed tick rate), `unlocked` (with `fps_cap`) |
| `fps_cap` | `60` | the frame cap with `fps=unlocked` |
| `fullscreen` | `1` | `0`: a window |
| `aa` | `off` | `off` (the game's own), `fsr3` (FSR 3.1 anti-aliasing / upscaling) |
| `upscale` | `native` | with `aa=fsr3`: `native`, `quality`, `balanced`, `performance`, `ultra_performance` |
| `sharpness` | `80` | `0`..`100` (FSR's RCAS) |
| `mirror` | the preset's | `live`, `static` |
| `aniso` | the runtime's | `0`, `2`, `4`, `8`, `16` (anisotropic filtering) |
| `show_fps` | `0` | `1`: the FPS counter |
| `mangohud` | `0` | `1`: the MangoHud overlay (bundled) |
| `two_thread` | `1` | `0`: switch off the two-thread draw path |
| `name` | `Player` | the player name |
| `language` | `1` | the console language (below) |
| `dlc_unlock` | `0` | `1`: the add-on cars, bikes, tours and livery decals, built from your own dumps |
| `fast_car_switch` | `0` | `1`: shorter car select animations, built from your own dumps |
| `detail.KEY` | the game's | graphics detail (an edited copy of `global.rpk`): `own_car`, `opponents`, `reflections`, `particles` `0`/`1`; `dynamic_res`, `motion_blur`, `lens_dust`, `heat_haze`, `fxaa` `1`/`0`; `scenery`, `shadows` `game`/`high`/`ultra` |
| `gearbox` | `game` | `game` (the game's Options menu), `automatic`, `manual` |
| `brake_assist` | `game` | `game`, `off`, `low`, `medium`, `high` |
| `hardcore`, `wheel_assist` | `game` | `game`, `off`, `on` |
| `overlays` | `0` | `1`: Vulkan layers that install themselves are let into the game |
| `patches_on` | | extra community patches by name, separated by `;` (`--list-patches`) |
| `patches_off` | | patches switched off, the same way |
| `env.NAME` | | any variable for the runtime (`env.BB_...`, BUILDING.md lists the diagnostics ones) or FEXCore (`env.FEX_...`, see Tuning) |

Variables already set in the environment (`BB_*`, `DC_*`) win over the file.

**Language.** The game has no language menu of its own: it uses the console's system language, which is
`language=` here (the Windows launcher's Language choice):

| id | language | id | language | id | language |
|---|---|---|---|---|---|
| 0 | Japanese | 7 | Portuguese | 14 | Danish |
| 1 | English (US) | 8 | Russian | 15 | Norwegian |
| 2 | French | 9 | Korean | 16 | Polish |
| 3 | Spanish | 10 | Chinese (Traditional) | 17 | Portuguese (Brazil) |
| 4 | German | 11 | Chinese (Simplified) | 18 | English (UK) |
| 5 | Italian | 12 | Finnish | 19 | Turkish |
| 6 | Dutch | 13 | Swedish | 20 | Spanish (Mexico) |

### Checks

The launcher's Advanced tab runs both and shows the newest log. `--vulkan-info` shows the Vulkan driver the game
gets (the bundled Turnip on Adreno). `--cpu-selftest` runs a few
x86-64 functions through FEXCore without the game (calls both ways, SSE/AVX, atomics, four threads, RDTSC), then
measures its speed. On an Apple M-series Linux VM it reports about 2.8 billion iterations/s for its integer loop
(native ARM64: 4.4), 34 ns per call from game code into the runtime and 92 ns per call back.

## What changed for ARM64 (and Linux)

- `src/guest_cpu.h`, `src/runtime_cpu.c`: the crossing between game code and host code. On x86-64 plain calls
  (unchanged behaviour); elsewhere `cpu/guest_cpu_fex.cpp` (FEXCore). Every place where the runtime calls the
  game's code (thread entries, `pthread_once`, TLS destructors, `atexit` handlers, the game's option setters, the
  benchmark's string assign, the car-select freeze fix) goes through `guest_cpu_call`; every host function the
  game calls (imports, patch thunks) through `guest_cpu_host_function`.
- `gpu/shim/bbport_guest_call.h`: `GuestCall()` for the GPU library's calls of game callbacks (AvPlayer
  allocators and file callbacks, the SystemShared allocator, the readback getter hooks, NetCtl/NP callbacks).
- `gpu/shim/bbport_cpu.h`: cycle counter and spin hint without x86 intrinsics.
- The SRT walker (shader resource tables) is an interpreted program instead of x86-64 machine code on ARM64
  (`flatten_extended_userdata_pass.cpp`, from the bbport ARM64 port).
- Write faults on ARM64 are told from read faults by the ESR record of the signal frame (`signal_context.cpp`).
- The SSE4a rewrite runs on ARM64 too (FEX is given the SSE4.1 forms).
- The car-select freeze fix (#185) now also applies on Linux.
- `tools/dc_prepare.cpp`: the launcher's preparation (`launcher/prepare.cpp`, unchanged) as a command.
- `tools/cpu_selftest.c`: the guest CPU's crossings checked without the game (`run.sh --cpu-selftest`; the
  package build runs it too).
- `launcher/linux/`: the Linux launcher (`dc-launcher`, SDL3 and Dear ImGui); `run.sh` turns its settings into the
  patches, variables and folders the runtime takes; `build.sh` builds on Linux; `shell.nix`, `packaging/` and
  `build_docker.sh` for the Docker build and the AppImage.
- `launcher/datx.cpp`, `unlock.cpp`, `graphics.cpp`, `carswitch.cpp`: portable C++ now (`launcher/fileio.h`), so
  dc-prepare builds the add-on content unlock, graphics detail and faster car switching on Linux too
  (`--unlock`, `--detail`, `--car-switch`).

## Not on Linux yet

These use Windows APIs and stay Windows-only for now: the high-FPS camera/replay workaround (`BB_CAMERA_PATCH`),
the field-of-view slider and F5/F6, the menu key-repeat timing fix, the scene overlay's time-of-day telemetry, the
motion-blur exposure clock, the physics force-feedback model's car hook, wheel setup, DLSS, frame generation, HDR
and the update checker. The game runs without them, as it does on Windows when they are off.

Online play does not work on any platform; on ARM64 the network module's callbacks are routed through the guest
CPU, but push events that pass pointers to host stack data are untested.

## Tuning (ARM64)

FEXCore reads its options from `FEX_*` variables; set them with `env.` lines in `driveclub.conf`, e.g.
`env.FEX_TSOENABLED=0` (no x86 memory-ordering emulation: faster, but the game's threads may then see each other's
writes out of order and crash) or `env.FEX_MULTIBLOCK=1` (the default). `env.BB_FRAME_STATS=1` writes the frame
statistics to the log every 5 seconds.

## Status

The first full build with `build_docker.sh` (Apple Silicon, FEX main of 2026-10-25) produced the AppImage; its
package check passed, `dc-cpu-selftest` passes with FEXCore in an ARM64 Linux VM, and on an Odin 3 (Armada OS) the
AppImage starts the game. The launcher was tested off-screen (every page and dialog drawn, a scripted walk through
the menus, its settings checked against `run.sh`), not yet on the Odin 3 with Steam Input. Frame rate and rendering
on the Odin 3 are still to be measured. Report problems with the log file and, for a crash, the lines starting with
`Guest fault`, `Host fault` or `STOP:`.
