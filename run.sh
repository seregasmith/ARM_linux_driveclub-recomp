#!/usr/bin/env bash
# The DriveClub runtime on Linux: the launcher (dc-launcher, a gamepad settings screen) or the game itself. It
# prepares the game from your own dumps (dc-prepare), writes the settings the runtime reads and starts it.
#
#   bash run.sh                          the launcher (its Play starts the game); without a desktop: the game
#   bash run.sh --play                   the game with the saved settings, without the launcher
#   bash run.sh --launcher               the launcher, whatever else is set
#   bash run.sh --update DIR --base DIR  set the game folders (saved), then start the game
#   bash run.sh --setup                  choose the folders again (zenity/kdialog when there is a desktop)
#   bash run.sh --prepare-only           prepare the game with the saved settings and stop (the launcher's Play)
#   bash run.sh --config                 print the settings file's path and contents
#   bash run.sh --list-patches           the community patches and their defaults
#   bash run.sh --cpu-selftest           check the guest CPU (FEXCore on ARM64) without the game, and its speed
# Other options go to the runtime (and start the game). DC_NO_LAUNCHER=1: never the launcher.
#
# Settings: $DC_DATA_DIR/driveclub.conf (default ~/.local/share/driveclub-recomp), key=value lines (the launcher
# writes them; a missing key is the default):
#   update=/path/CUSA00093-patch   the version 1.28 update dump (eboot.bin); may be the same as base
#   base=/path/CUSA00093           the base game dump (game000.dat)
#   preset=handheld                handheld (720p, static mirrors) | low | medium | high | ultra | custom
#   output=                        720p | 900p | default (1080p) | 1440p | 1800p | 4k | 5k | 8k (default: the preset's)
#   render=                        720p | 900p | default (1080p) | 1440p | 1800p | 4k | 8k (default: the preset's)
#   fps=60                         30 | 60 (deltatime) | 60fixed | unlocked
#   fps_cap=60                     the frame cap with fps=unlocked
#   fullscreen=1                   0 for a window
#   aa=off                         off (the game's own) | fsr3
#   upscale=native                 native | quality | balanced | performance | ultra_performance (with aa=fsr3)
#   sharpness=80                   0..100 (FSR's RCAS)
#   mirror=                        live | static (default: the preset's)
#   aniso=                         0 | 2 | 4 | 8 | 16 (default: the runtime's)
#   show_fps=0                     1: the FPS counter
#   mangohud=0                     1: the MangoHud overlay (bundled in the AppImage)
#   two_thread=1                   the two-thread draw path (0 off)
#   name=Player                    the player name
#   language=                      the PS4 system language id (1 English, 2 French, 4 German, 8 Russian, ...)
#   dlc_unlock=0                   1: the add-on cars, bikes, tours and liveries, built from your own dumps
#   fast_car_switch=0              1: shorter car select animations, built from your own dumps
#   detail.KEY=VALUE               graphics detail (own_car, opponents, reflections, particles: 0|1; dynamic_res,
#                                  motion_blur, lens_dust, heat_haze, fxaa: 1|0; scenery, shadows: game|high|ultra)
#   gearbox=game                   game | automatic | manual
#   brake_assist=game              game | off | low | medium | high
#   hardcore=game                  game | off | on (hardcore handling)
#   wheel_assist=game              game | off | on
#   overlays=0                     1: Vulkan layers that install themselves are let into the game
#   patches_on=Name;Other name     extra community patches by name (run.sh --list-patches)
#   patches_off=Name               patches switched off
#   env.BB_SOMETHING=value         any runtime (or FEX_...) variable (BUILDING.md lists the diagnostics ones)
# Variables already set in the environment (BB_*, DC_*) win over the file.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# Packaged (AppImage, BB_PREBUILT=1): the binaries in bin/ next to this script; from source: out/.
if [[ -n ${BB_PREBUILT:-} && -x $here/bin/DriveClubGame ]]; then
    bin=$here/bin
else
    bin=$here/out
fi
game=$bin/DriveClubGame
prepare=$bin/dc-prepare
patch_xml=${DC_PATCH_XML:-$here/patches/DriveClub.xml}
data=${DC_DATA_DIR:-${BB_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/driveclub-recomp}}
conf=$data/driveclub.conf
mkdir -p "$data"

mode=auto
die() { echo "DriveClub: $*" >&2; notify "$*"; exit 1; }
notify() {
    # A message box when started from a desktop entry (no terminal to read). Not for the launcher, which shows it.
    [[ -t 2 || $mode == prepare ]] && return 0
    if command -v zenity >/dev/null; then zenity --error --no-wrap --title DriveClub --text "$1" 2>/dev/null || true
    elif command -v kdialog >/dev/null; then kdialog --title DriveClub --error "$1" 2>/dev/null || true; fi
}

# ---- settings file -------------------------------------------------------------------------------------------
declare -A cfg=([preset]=handheld [fps]=60 [fps_cap]=60 [fullscreen]=1 [aa]=off [upscale]=native [sharpness]=80
                [show_fps]=0 [two_thread]=1 [name]=Player)
declare -A cfg_env=()
if [[ -f $conf ]]; then
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        [[ -z $line || $line == \#* || $line != *=* ]] && continue
        key=${line%%=*}; value=${line#*=}
        if [[ $key == env.* ]]; then cfg_env[${key#env.}]=$value; else cfg[$key]=$value; fi
    done < "$conf"
fi
# Only the folders change here: the file's other lines stay as the launcher (or you) wrote them.
save_folders() {
    {
        echo "update=${cfg[update]}"
        echo "base=${cfg[base]}"
        [[ -f $conf ]] && grep -v -E '^(update|base)=' "$conf" || true
    } > "$conf.tmp" && mv "$conf.tmp" "$conf"
}
pick_folder() { # title -> path on stdout, empty when there is no picker or it was cancelled
    if [[ -n ${DISPLAY:-}${WAYLAND_DISPLAY:-} ]]; then
        if command -v zenity >/dev/null; then zenity --file-selection --directory --title "$1" 2>/dev/null || true; return; fi
        if command -v kdialog >/dev/null; then kdialog --title "$1" --getexistingdirectory "$HOME" 2>/dev/null || true; return; fi
    fi
}

setup=0 folders_given=0
# mode: auto: the launcher when there is a desktop and nothing else was asked for; launcher; play; prepare
args=()
while [[ $# -gt 0 ]]; do
    case $1 in
    --update) cfg[update]=$2; folders_given=1; shift 2 ;;
    --base) cfg[base]=$2; folders_given=1; shift 2 ;;
    --setup) setup=1; shift ;;
    --config) echo "$conf"; [[ -f $conf ]] && cat "$conf"; exit 0 ;;
    --list-patches) exec "$prepare" --patches-xml "$patch_xml" --list ;;
    --cpu-selftest) shift; exec "$bin/dc-cpu-selftest" "$@" ;;
    --play) mode=play; shift ;;
    --launcher) mode=launcher; shift ;;
    --prepare-only) mode=prepare; shift ;;
    -h|--help) awk 'NR == 1 { next } !/^#/ { exit } { sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) args+=("$1"); shift ;;
    esac
done
if [[ $mode == auto ]]; then
    # Game folders or runtime options on the command line, DC_NO_LAUNCHER (the game restarting itself, a frontend)
    # or no desktop: the game. Otherwise the launcher.
    if [[ $folders_given == 0 && $setup == 0 && ${#args[@]} == 0 && -z ${DC_NO_LAUNCHER:-} && -x $bin/dc-launcher &&
          -n ${DISPLAY:-}${WAYLAND_DISPLAY:-} ]]; then
        mode=launcher
    else
        mode=play
    fi
fi
if [[ $mode == launcher ]]; then
    [[ -x $bin/dc-launcher ]] || die "the launcher is not built ($bin/dc-launcher). Run: bash build.sh"
    export DC_DATA_DIR=$data
    exec "$bin/dc-launcher" --run-sh "$here/run.sh" --data "$data" --patches-xml "$patch_xml" --bin "$bin"
fi
[[ -x $game && -x $prepare ]] || die "the runtime is not built ($game). Run: bash build.sh"
# The game's memory model and FEXCore (ARM64) assume 4 KB pages (some ARM64 kernels use 16 KB).
page=$(getconf PAGESIZE 2>/dev/null || echo 4096)
[[ $page == 4096 ]] || die "this kernel uses ${page}-byte memory pages; the runtime needs 4 KB pages"

folders_were="${cfg[update]:-}|${cfg[base]:-}"
if [[ $mode != prepare ]]; then
    if [[ $setup == 1 || -z ${cfg[base]:-} || ! -d ${cfg[base]:-/nonexistent} ]]; then
        folder=$(pick_folder "DriveClub: the base game folder (with game000.dat)")
        [[ -n $folder ]] && cfg[base]=$folder
    fi
    if [[ $setup == 1 || -z ${cfg[update]:-} || ! -d ${cfg[update]:-/nonexistent} ]]; then
        folder=$(pick_folder "DriveClub: the version 1.28 update folder (with eboot.bin; the base folder if it is already 1.28)")
        [[ -n $folder ]] && cfg[update]=$folder
    fi
fi
[[ -z ${cfg[update]:-} && -n ${cfg[base]:-} && -f ${cfg[base]}/eboot.bin ]] && cfg[update]=${cfg[base]}
# Absolute paths: the game starts in this script's directory.
for key in update base; do
    [[ -n ${cfg[$key]:-} && -d ${cfg[$key]} ]] && cfg[$key]=$(cd -- "${cfg[$key]}" && pwd)
done
[[ -n ${cfg[base]:-} && -f ${cfg[base]}/game000.dat ]] ||
    die "set the base game folder (the one with game000.dat): bash run.sh --update <1.28 update dir> --base <base game dir>"
[[ -n ${cfg[update]:-} && -f ${cfg[update]}/eboot.bin ]] ||
    die "set the version 1.28 update folder (the one with eboot.bin): bash run.sh --update <dir> --base <dir>"
[[ "${cfg[update]}|${cfg[base]}" == "$folders_were" ]] || save_folders

# ---- patches and runtime settings ---------------------------------------------------------------------------------
on=() off=()
# The sizes and the mirror updates: the preset's, unless the file names them (as the launcher writes them).
declare -A preset_output=([handheld]=720p [low]=default [medium]=1440p [high]=4k [ultra]=4k [custom]=default)
declare -A preset_render=([handheld]=720p [low]=720p [medium]=default [high]=1440p [ultra]=4k [custom]=default)
declare -A preset_mirror=([handheld]=static [low]=live [medium]=live [high]=live [ultra]=live [custom]=live)
[[ -n ${preset_output[${cfg[preset]}]:-} ]] || die "unknown preset=${cfg[preset]} (handheld, low, medium, high, ultra, custom)"
output=${cfg[output]:-${preset_output[${cfg[preset]}]}}
render=${cfg[render]:-${preset_render[${cfg[preset]}]}}
mirror=${cfg[mirror]:-${preset_mirror[${cfg[preset]}]}}
declare -A output_patch=([720p]="720p [1280x720] Output resolution" [900p]="900p [1600x900] Output resolution"
                         [1440p]="1440p [2560x1440] Output resolution"
                         [1800p]="1800p [3200x1800] Output resolution" [4k]="4K [3840x2160] Output resolution"
                         [5k]="5K [5120x2880] Output resolution" [8k]="8K [7680x4320] Output resolution")
declare -A render_patch=([720p]="720p [1280x720] Internal resolution" [900p]="900p [1600x900] Internal resolution"
                         [1440p]="1440p [2560x1440] Internal resolution [DEPTH BUG]"
                         [1800p]="1800p [3200x1800] Internal resolution [DEPTH BUG]"
                         [4k]="4K [3840x2160] Internal resolution [DEPTH BUG]" [8k]="8K [7680x4320] Internal resolution [DEPTH BUG]")
if [[ $output != default ]]; then
    [[ -n ${output_patch[$output]:-} ]] || die "unknown output=$output (720p, 900p, default, 1440p, 1800p, 4k, 5k, 8k)"
    on+=("${output_patch[$output]}")
fi
if [[ $render != default ]]; then
    [[ -n ${render_patch[$render]:-} ]] || die "unknown render=$render (720p, 900p, default, 1440p, 1800p, 4k, 8k)"
    on+=("${render_patch[$render]}")
fi
# Sizes above 1080p need the game's memory patch and a larger direct memory budget (the Windows launcher's
# MemoryTier): the larger of the output's and the render size's tier.
tier_of() { case $1 in 5k|8k) echo 3 ;; 1800p|4k) echo 2 ;; 1440p) echo 1 ;; *) echo 0 ;; esac; }
tier=$(tier_of "$output"); (( $(tier_of "$render") > tier )) && tier=$(tier_of "$render")
memory_patches=('' "Memory for up to 1440p" "Memory for up to 4K" "Memory for up to 8K")
dmem=(5056 8056 10056 17056)
(( tier > 0 )) && on+=("${memory_patches[$tier]}")
fps_limit='' present_mode=''
case ${cfg[fps]} in
30)       off+=("60 FPS with deltatime") ;;
60)       on+=("60 FPS with deltatime"); fps_limit=60 ;;
60fixed)  off+=("60 FPS with deltatime"); on+=("60 FPS with fixed tickrate") ;;
unlocked) on+=("60 FPS with deltatime"); fps_limit=${cfg[fps_cap]}; present_mode=Fifo ;;
*) die "unknown fps=${cfg[fps]} (30, 60, 60fixed, unlocked)" ;;
esac
IFS=';' read -r -a extra_on <<< "${cfg[patches_on]:-}"
IFS=';' read -r -a extra_off <<< "${cfg[patches_off]:-}"
# FSR 3.1 needs the game's blur pass (the Windows launcher's BlurLocked).
[[ ${cfg[aa]} == fsr3 ]] && extra_off+=("Disable motion blur and depth of field")
prep_args=(--update "${cfg[update]}" --base "${cfg[base]}" --work "$data" --patches-xml "$patch_xml")
for p in "${on[@]}" "${extra_on[@]}"; do [[ -n $p ]] && prep_args+=(--enable "$p"); done
for p in "${off[@]}" "${extra_off[@]}"; do [[ -n $p ]] && prep_args+=(--disable "$p"); done
# Replacement game files built from your own dumps (launcher/unlock.cpp, carswitch.cpp, graphics.cpp).
[[ ${cfg[dlc_unlock]:-0} == 1 ]] && prep_args+=(--unlock)
[[ ${cfg[fast_car_switch]:-0} == 1 ]] && prep_args+=(--car-switch)
for key in "${!cfg[@]}"; do
    [[ $key == detail.* ]] && prep_args+=(--detail "${key#detail.}=${cfg[$key]}")
done
[[ ${cfg[aa]} == fsr3 ]] && prep_args+=(--detail motion_blur=1) # FSR needs the game's motion blur shader too
"$prepare" "${prep_args[@]}" || die "preparing the game failed (see above)"

user=$data/user
mkdir -p "$user" "$data/logs" "$data/mods"
printf 'show_fps=%s\n' "${cfg[show_fps]}" > "$user/driveclub.ini"
# The renderer optimizations switched off (gpu/shim/bbport_toggles.h): the launcher's values.
if [[ ${cfg[two_thread]} == 0 ]]; then echo 2336462745600 > "$user/toggles.txt"; else echo 2199023792128 > "$user/toggles.txt"; fi

setdefault() { [[ -n ${!1:-} ]] || export "$1=$2"; }
setdefault BB_GPU_USER_DIR "$user"
setdefault BB_CRASH_DIR "$data/logs"
setdefault BB_CONFIG "$user/driveclub.ini"
setdefault BB_TOGGLE_FILE "$user/toggles.txt"
setdefault BB_USER_NAME "${cfg[name]}"
setdefault BB_FULLSCREEN "${cfg[fullscreen]}"
setdefault BB_CAMERA_PATCH 0
setdefault BB_FRAME_GEN off
setdefault BB_DMEM_MB "${dmem[$tier]}"
[[ -n $fps_limit ]] && setdefault BB_FPS_LIMIT "$fps_limit"
[[ -n $present_mode ]] && setdefault BB_PRESENT_MODE "$present_mode"
[[ -n ${cfg[language]:-} ]] && setdefault BB_LANGUAGE "${cfg[language]}"
[[ -n ${cfg[aniso]:-} ]] && setdefault BB_ANISO "${cfg[aniso]}"
case $mirror in static) setdefault BB_MIRROR_RATE 0 ;; live) setdefault BB_MIRROR_RATE 1 ;; esac
case ${cfg[aa]} in
fsr3) setdefault BB_AA fsr3; setdefault BB_AA_SCALE "${cfg[upscale]}"
      setdefault BB_FSR_SHARPNESS "$(awk -v s="${cfg[sharpness]}" 'BEGIN { printf "%.2f", s / 100 }')" ;;
*)    setdefault BB_AA off ;;
esac
# The driving assists ("game": the game's own Options menu decides).
for pair in gearbox:BB_GEARBOX brake_assist:BB_BRAKE_ASSIST hardcore:BB_HARDCORE wheel_assist:BB_WHEEL_ASSIST; do
    value=${cfg[${pair%%:*}]:-game}
    [[ $value == game ]] || setdefault "${pair#*:}" "$value"
done
for key in "${!cfg_env[@]}"; do setdefault "$key" "${cfg_env[$key]}"; done
# MangoHud (bundled in the AppImage): mangohud=1 in the settings, or MANGOHUD=1.
[[ ${cfg[mangohud]:-0} == 1 ]] && setdefault MANGOHUD 1
# Vulkan layers that install themselves (overlays, capture tools) stay out unless asked for, as on Windows.
[[ ${MANGOHUD:-0} == 1 || ${cfg[overlays]:-0} == 1 ]] || setdefault VK_LOADER_LAYERS_DISABLE '~implicit~'
summary="preset=${cfg[preset]} output=$output render=$render fps=${cfg[fps]} aa=${cfg[aa]} upscale=${cfg[upscale]} mirror=$mirror two_thread=${cfg[two_thread]}"
export BB_SETTINGS_SUMMARY="run.sh: $summary"

prep=$data/prep
# The folders dc-prepare built replacement files in (prep/mods.txt), after your own mods folder.
mods=(--mods "$data/mods")
if [[ -f $prep/mods.txt ]]; then
    while IFS= read -r dir; do [[ -n $dir ]] && mods+=(--mods "$dir"); done < "$prep/mods.txt"
fi
if [[ $mode == prepare ]]; then
    echo "DriveClub: prepared ($summary)"
    exit 0
fi
# The runtime restarts itself through "bash run.sh": straight into the game again, not the launcher.
export DC_NO_LAUNCHER=1
log=$data/logs/driveclub_$(date +%Y%m%d_%H%M%S).log
echo "DriveClub: $summary; log $log"
# The runtime restarts itself through "bash run.sh" in its working directory (in-game settings menu).
cd "$here"
exec > >(tee "$log") 2>&1
exec "$game" "$prep/boot-linked.bin" --content-profile "$prep/content.bin" --patches "$prep/patches.bin" \
    --app0 "${cfg[update]}" --app0-base "${cfg[base]}" --user "$user" "${mods[@]}" \
    --stub-unresolved --timeout 0 ${args[@]+"${args[@]}"}
