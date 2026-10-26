#!/usr/bin/env bash
# Linux launcher of the DriveClub runtime: the Windows launcher's job (DriveClub.exe) as a script, for desktop
# Linux, handhelds and the AppImage. It prepares the game from your own dumps (dc-prepare), writes the settings
# the runtime reads and starts it.
#
#   bash run.sh                          start with the saved settings (the first start asks for the folders)
#   bash run.sh --update DIR --base DIR  set the game folders (saved), then start
#   bash run.sh --setup                  choose the folders again (zenity/kdialog when there is a desktop)
#   bash run.sh --config                 print the settings file's path and contents
#   bash run.sh --list-patches           the community patches and their defaults
#   bash run.sh --cpu-selftest           check the guest CPU (FEXCore on ARM64) without the game, and its speed
#
# Settings: $DC_DATA_DIR/driveclub.conf (default ~/.local/share/driveclub-recomp), key=value lines:
#   update=/path/CUSA00093-patch   the version 1.28 update dump (eboot.bin); may be the same as base
#   base=/path/CUSA00093           the base game dump (game000.dat)
#   preset=handheld                handheld (720p, static mirrors) | low | medium | high | custom
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
#   language=                      the PS4 system language id (1 English, 2 French, 4 German, ...)
#   patches_on=Name;Other name     extra community patches by name (run.sh --list-patches)
#   patches_off=Name               patches switched off
#   env.BB_SOMETHING=value         any runtime variable (BUILDING.md lists the diagnostics ones)
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

die() { echo "DriveClub: $*" >&2; notify "$*"; exit 1; }
notify() {
    # A message box when started from a desktop entry (no terminal to read).
    [[ -t 2 ]] && return 0
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
save_conf() {
    {
        echo "# DriveClub runtime settings (run.sh lists the keys). Saved by run.sh."
        for key in update base preset fps fps_cap fullscreen aa upscale sharpness mirror aniso show_fps mangohud \
                   two_thread name language patches_on patches_off; do
            [[ -n ${cfg[$key]:-} ]] && echo "$key=${cfg[$key]}"
        done
        for key in "${!cfg_env[@]}"; do echo "env.$key=${cfg_env[$key]}"; done
    } > "$conf.tmp" && mv "$conf.tmp" "$conf"
}
pick_folder() { # title -> path on stdout, empty when there is no picker or it was cancelled
    if [[ -n ${DISPLAY:-}${WAYLAND_DISPLAY:-} ]]; then
        if command -v zenity >/dev/null; then zenity --file-selection --directory --title "$1" 2>/dev/null || true; return; fi
        if command -v kdialog >/dev/null; then kdialog --title "$1" --getexistingdirectory "$HOME" 2>/dev/null || true; return; fi
    fi
}

setup=0
args=()
while [[ $# -gt 0 ]]; do
    case $1 in
    --update) cfg[update]=$2; shift 2 ;;
    --base) cfg[base]=$2; shift 2 ;;
    --setup) setup=1; shift ;;
    --config) echo "$conf"; [[ -f $conf ]] && cat "$conf"; exit 0 ;;
    --list-patches) exec "$prepare" --patches-xml "$patch_xml" --list ;;
    --cpu-selftest) shift; exec "$bin/dc-cpu-selftest" "$@" ;;
    --play) shift ;; # the AppImage's "start the game" (this script always does)
    -h|--help) awk 'NR == 1 { next } !/^#/ { exit } { sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) args+=("$1"); shift ;;
    esac
done
[[ -x $game && -x $prepare ]] || die "the runtime is not built ($game). Run: bash build.sh"
# The game's memory model and FEXCore (ARM64) assume 4 KB pages (some ARM64 kernels use 16 KB).
page=$(getconf PAGESIZE 2>/dev/null || echo 4096)
[[ $page == 4096 ]] || die "this kernel uses ${page}-byte memory pages; the runtime needs 4 KB pages"

if [[ $setup == 1 || -z ${cfg[base]:-} || ! -d ${cfg[base]:-/nonexistent} ]]; then
    folder=$(pick_folder "DriveClub: the base game folder (with game000.dat)")
    [[ -n $folder ]] && cfg[base]=$folder
fi
if [[ $setup == 1 || -z ${cfg[update]:-} || ! -d ${cfg[update]:-/nonexistent} ]]; then
    folder=$(pick_folder "DriveClub: the version 1.28 update folder (with eboot.bin; the base folder if it is already 1.28)")
    [[ -n $folder ]] && cfg[update]=$folder
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
save_conf

# ---- patches and runtime settings from the preset ---------------------------------------------------------------
on=() off=()
mirror=${cfg[mirror]:-}
case ${cfg[preset]} in
handheld) on+=("720p [1280x720] Output resolution" "720p [1280x720] Internal resolution"); mirror=${mirror:-static} ;;
low)      on+=("720p [1280x720] Internal resolution") ;;
medium)   on+=("1440p [2560x1440] Output resolution" "Memory for up to 1440p") ;;
high)     on+=("4K [3840x2160] Output resolution" "1440p [2560x1440] Internal resolution [DEPTH BUG]" "Memory for up to 4K") ;;
custom)   ;;
*) die "unknown preset=${cfg[preset]} (handheld, low, medium, high, custom)" ;;
esac
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
prep_args=(--update "${cfg[update]}" --base "${cfg[base]}" --work "$data" --patches-xml "$patch_xml")
for p in "${on[@]}" "${extra_on[@]}"; do [[ -n $p ]] && prep_args+=(--enable "$p"); done
for p in "${off[@]}" "${extra_off[@]}"; do [[ -n $p ]] && prep_args+=(--disable "$p"); done
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
case ${cfg[preset]} in medium) setdefault BB_DMEM_MB 8056 ;; high) setdefault BB_DMEM_MB 10056 ;; *) setdefault BB_DMEM_MB 5056 ;; esac
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
for key in "${!cfg_env[@]}"; do setdefault "$key" "${cfg_env[$key]}"; done
# MangoHud (bundled in the AppImage): mangohud=1 in the settings, or MANGOHUD=1.
[[ ${cfg[mangohud]:-0} == 1 ]] && setdefault MANGOHUD 1
# Vulkan layers that install themselves (overlays, capture tools) stay out unless asked for, as on Windows.
[[ ${MANGOHUD:-0} == 1 ]] || setdefault VK_LOADER_LAYERS_DISABLE '~implicit~'
summary="preset=${cfg[preset]} fps=${cfg[fps]} aa=${cfg[aa]} upscale=${cfg[upscale]} mirror=${mirror:-default} two_thread=${cfg[two_thread]}"
export BB_SETTINGS_SUMMARY="run.sh: $summary"

prep=$data/prep
log=$data/logs/driveclub_$(date +%Y%m%d_%H%M%S).log
echo "DriveClub: $summary; log $log"
# The runtime restarts itself through "bash run.sh" in its working directory (in-game settings menu).
cd "$here"
exec > >(tee "$log") 2>&1
exec "$game" "$prep/boot-linked.bin" --content-profile "$prep/content.bin" --patches "$prep/patches.bin" \
    --app0 "${cfg[update]}" --app0-base "${cfg[base]}" --user "$user" --mods "$data/mods" \
    --stub-unresolved --timeout 0 "${args[@]}"
