#!/usr/bin/env bash
# Builds dist/DriveClub-recomp-<arch>.AppImage (x86_64, aarch64): the runtime's current build (run build.sh
# first), run.sh and the patches, bundled with their Nix closure and Mesa's Vulkan drivers (nix-appimage: the
# AppImage mounts its /nix/store with user namespaces). Adapted from the bbport ARM64 port's packaging.
# Running it starts the game (run.sh): the first start asks for the game folders, or
#   ./DriveClub-recomp-aarch64.AppImage --update <1.28 update dump> --base <base game dump>
# Data (prepared image, saves, shader cache, logs, driveclub.conf): ~/.local/share/driveclub-recomp (DC_DATA_DIR).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f out/DriveClubGame && -f out/dc-launcher && -f out/dc-prepare && -f out/dc-cpu-selftest && -f out/gpu/libbbgpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
arch=$(uname -m)
libs=(out/DriveClubGame out/dc-launcher out/dc-prepare out/dc-cpu-selftest out/gpu/libbbgpu.so)
if [[ $arch == aarch64 ]]; then
    [[ -f out/fex/libbbcpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
    libs+=(out/fex/libbbcpu.so)
fi
# NIX: the nix command (default nix; e.g. "nix-portable nix" without a system Nix).
# BB_NIXPKGS: the nixpkgs the build's nix-shell used (-I nixpkgs=...), when not <nixpkgs>.
# BB_APPIMAGE_BUNDLER: the nix-appimage bundler flake (pin it to a commit to skip GitHub's API).
read -r -a nix <<< "${NIX:-nix}"
include=()
if [[ -n ${BB_NIXPKGS:-} ]]; then include=(-I "nixpkgs=$BB_NIXPKGS"); fi
bundler=${BB_APPIMAGE_BUNDLER:-github:ralismark/nix-appimage}
root=$PWD
# The libraries' store paths: for each library a binary needs (NEEDED), the RUNPATH directory it is found in
# (their closures come along). Not every RUNPATH entry: built in nix-shell, the binaries list the lib
# directories of the whole build environment (compilers, headers, ...), hundreds of MB the game never loads.
needed_dirs() {
    local elf=$1 lib dir
    local -a rpath
    mapfile -t rpath < <(readelf -d "$elf" | sed -n 's/.*R\{0,1\}U\{0,1\}N\{0,1\}PATH.*\[\(.*\)\]/\1/p' | tr ':' '\n')
    for lib in $(readelf -d "$elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        for dir in "${rpath[@]}"; do
            [[ $dir == /nix/store/* && -e $dir/$lib ]] && { echo "$dir"; break; }
        done
    done
}
{
    echo '['
    for elf in "${libs[@]}"; do
        needed_dirs "$elf"
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && "${nix[@]}" bundle --impure "${include[@]}" --bundler "$bundler" \
    --expr "import $root/packaging {}")
mkdir -p dist
# The bundle's link is named after the package (driveclub-recomp.AppImage).
image=$(readlink "$work"/*.AppImage | head -n 1)
# nix-portable: its /nix/store is ~/.nix-portable/nix/store outside its sandbox.
if [[ ! -e $image && -e ${NP_LOCATION:-$HOME}/.nix-portable$image ]]; then
    image=${NP_LOCATION:-$HOME}/.nix-portable$image
fi
install -m755 "$image" "dist/DriveClub-recomp-$arch.AppImage"
# A GC root for the package: nix-collect-garbage keeps it, and with it the outputs built here from source (the
# patched Turnip), so the next AppImage does not build them again (only a nixpkgs update does).
nix-build --impure "${include[@]}" packaging -o out/nix-roots/driveclub > /dev/null
ls -lh "dist/DriveClub-recomp-$arch.AppImage"
