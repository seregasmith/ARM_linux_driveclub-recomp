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

Copy the AppImage to the device and start it once from a terminal (or a file manager):

```bash
chmod +x DriveClub-recomp-aarch64.AppImage
./DriveClub-recomp-aarch64.AppImage --update /path/to/CUSA00093-1.28 --base /path/to/CUSA00093
```

Without `--update/--base` it opens folder pickers (zenity or kdialog) when there is a desktop. The folders are
saved; later starts need no options, so the AppImage can be added to Steam or a frontend as it is.
Settings, the prepared game image, saves, the shader cache and the logs are in
`~/.local/share/driveclub-recomp/` (`DC_DATA_DIR`). `driveclub.conf` there holds the settings; `run.sh --help`
lists them. The defaults suit a handheld:

| key | default | |
|---|---|---|
| `preset` | `handheld` | 720p render and output, static mirrors (`low`, `medium`, `high`, `custom`) |
| `fps` | `60` | 60 FPS with the deltatime patch; `30`, `60fixed`, `unlocked` (with `fps_cap`) |
| `aa` | `off` | `fsr3` for FSR 3.1 anti-aliasing / upscaling (`upscale=quality`, ...) |
| `mangohud` | `0` | `1`: MangoHud overlay (bundled) |
| `two_thread` | `1` | the two-thread draw path |
| `env.BB_...` | | any runtime variable (BUILDING.md has the diagnostics ones) |

Logs: `~/.local/share/driveclub-recomp/logs/driveclub_<date>.log`. Vulkan driver check:
`./DriveClub-recomp-aarch64.AppImage --vulkan-info`. Guest CPU check, without the game (FEXCore runs a few x86-64
functions: calls both ways, SSE/AVX, atomics, four threads, RDTSC; then its speed):
`./DriveClub-recomp-aarch64.AppImage --cpu-selftest`. On an Apple M-series Linux VM it reports about 2.8 billion
iterations/s for its integer loop (native ARM64: 4.4), 34 ns per call from game code into the runtime and 92 ns per
call back.

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
- `run.sh`: the launcher's job on Linux; `build.sh` builds on Linux; `shell.nix`, `packaging/` and
  `build_docker.sh` for the Docker build and the AppImage.

## Not on Linux yet

These use Windows APIs and stay Windows-only for now: the high-FPS camera/replay workaround (`BB_CAMERA_PATCH`),
the field-of-view slider and F5/F6, the menu key-repeat timing fix, the scene overlay's time-of-day telemetry, the
motion-blur exposure clock, the physics force-feedback model's car hook, add-on content unlock, graphics detail
and faster car switching (they edit copies of the game's archives with Windows file calls), DLSS and the
Windows launcher itself. The game runs without them, as it does on Windows when they are off.

Online play does not work on any platform; on ARM64 the network module's callbacks are routed through the guest
CPU, but push events that pass pointers to host stack data are untested.

## Tuning (ARM64)

FEXCore reads its options from `FEX_*` variables; set them with `env.` lines in `driveclub.conf`, e.g.
`env.FEX_TSOENABLED=0` (no x86 memory-ordering emulation: faster, but the game's threads may then see each other's
writes out of order and crash) or `env.FEX_MULTIBLOCK=1` (the default). `env.BB_FRAME_STATS=1` writes the frame
statistics to the log every 5 seconds.

## Status

The first full build with `build_docker.sh` (Apple Silicon, FEX main of 2026-10-25) produced the AppImage; its
package check passed, and in an ARM64 Linux VM the AppImage starts (`--help`) and `dc-cpu-selftest` passes with
FEXCore. The first run of the game on an Odin 3 is still to be done. Report problems with the log file and, for a
crash, the lines starting with `Guest fault`, `Host fault` or `STOP:`.
