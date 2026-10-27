#!/bin/bash
# build_docker.sh: build this repository (ARM_Linux_driveclub: the DriveClub recomp runtime,
# OverkillLabs3/driveclub-recomp, with a Linux build) in a Linux Docker container on any host, for
# either target architecture. The same script as ARM_Linux_Gravity_Rush_2's and
# ARM_Linux_infamous_ss's, for this tree:
#   aarch64  ARM64 Linux devices (AYN Odin 3 on Armada OS and other Snapdragon 8 Elite handhelds):
#            the game's x86-64 code runs in FEXCore (cpu/, third_party/FEX), as in the Bloodborne
#            ARM port; the runtime and the renderer are native ARM64
#            -> dist/DriveClub-recomp-aarch64.AppImage
#   x86_64   x86-64 Linux PCs and the Steam Deck (the game's code runs directly on the CPU, no FEX)
#            -> dist/DriveClub-recomp-x86_64.AppImage
# Hosts: macOS on Apple Silicon or Intel (Docker Desktop, OrbStack or colima), Linux on x86-64 or
# arm64 (Docker Engine). A target that matches Docker's own architecture builds natively; the
# other one runs emulated (see "Emulation" below).
# Usage:  ./build_docker.sh [--arch aarch64|x86_64] [build|appimage|shell|clean]
#   --arch    target architecture (or DC_ARCH=...; arm64 and amd64 work too). Default: Docker's own
#             architecture, which needs no emulation (aarch64 on Apple Silicon, x86_64 on a PC)
#   build     (default) build.sh: out/DriveClubGame (the runtime), out/dc-launcher (the launcher),
#             out/dc-prepare (the game's
#             preparation from the dumps), out/gpu/libbbgpu.so, and on aarch64 out/fex/libbbcpu.so
#             (FEXCore)
#   appimage  build, then dist/DriveClub-recomp-<arch>.AppImage (packaging/appimage.sh with
#             packaging/default.nix: the runtime, run.sh, the patches, Mesa's Vulkan drivers; for
#             aarch64 Turnip, patched for the Adreno 830). The first run on aarch64 builds Turnip
#             from source. Data folder of the packaged runtime: ~/.local/share/driveclub-recomp
#   shell     interactive shell in that target's build environment (shell.nix), in the tree
#   clean     delete that target's Docker image and Nix store volume (the source tree is untouched)
# (The Windows release is built on Windows with MSYS2: BUILDING.md.)
#
# Build directories (build.sh always writes out/ and pgo/, so each target gets its own):
#   aarch64  out and pgo
#   x86_64   build-x86_64/out and build-x86_64/pgo, mounted over out and pgo in the container
#            (.gitignore's out/ and pgo/ patterns cover them)
#   Both targets share dist/ (the AppImage names carry the arch) and
#   packaging/runtime-paths.nix (rewritten by every appimage run): one build of a tree at a time.
#
# The script runs no git commands on the repository. Check out the submodules yourself first:
#   git submodule update --init --recursive
# (third_party/FEX is needed for aarch64 only.)
# Linux host with Docker Engine: files made in the container belong to root there, so at the end
# the script gives the root-owned files in what the build writes back to you (out, pgo, dist,
# packaging/runtime-paths.nix). Ownership only, never contents. Not with rootless Docker, Podman
# or Docker Desktop, which map the container's root to you already.
#
# Emulation (a target other than Docker's architecture) works, but is slower:
#   Apple Silicon -> x86_64: Rosetta. Docker Desktop: Settings > General > "Use Rosetta for
#     x86_64/amd64 emulation on Apple Silicon" (without it QEMU: much slower). OrbStack: built in.
#     colima: colima start --vm-type vz --vz-rosetta
#   Intel/AMD -> aarch64: QEMU, many times slower than native; for appimage, the Turnip (Mesa)
#     compile alone can take hours. Docker Desktop and OrbStack include QEMU. Linux:
#     register it once per boot: docker run --privileged --rm tonistiigi/binfmt --install arm64
#     (or install the distribution's qemu-user-static package).
#
# Settings (environment variables):
#   DC_ARCH         target, as --arch (the option wins)
#   DC_SRC          source tree to build (default: the directory this script is in)
#   DC_LTO          ON|OFF link-time optimization of the GPU library (build.sh's BB_LTO; default
#                   here: OFF, faster and far less RAM; build.sh's own default is ON)
#   BB_PGO          off|generate|use, passed to build.sh (default: build.sh's own choice, "use" when
#                   the target's pgo/ holds a profile)
#   JOBS            parallel compiler processes (default: picked from Docker's RAM, about 2 GB each).
#                   If the compiler still gets "Killed" (out of memory): JOBS=2 ./build_docker.sh
#   DC_NIX_VOLUME   Docker volume for the target's Nix store (default: dc-nix for aarch64,
#                   dc-nix-x86_64). The Gravity Rush 2, inFAMOUS and Bloodborne build_docker.sh pin
#                   the same nixpkgs, so their volumes can be shared to skip downloading the
#                   toolchain again (and, for appimage, building the same Turnip again: the
#                   derivation is the same):
#                     DC_NIX_VOLUME=gr2-nix ./build_docker.sh                      (aarch64)
#                     DC_NIX_VOLUME=gr2-nix-x86_64 ./build_docker.sh --arch x86_64
#                     DC_NIX_VOLUME=bbport-nix ./build_docker.sh                   (aarch64)
#                   `clean` deletes only this script's own dc-* volumes.
#   GITHUB_TOKEN    optional GitHub token for Nix downloads (appimage), if GitHub's API rate limit hits
#   DC_TURNIP_SRC   aarch64 appimage only (GR2_, ISS_ and BB_TURNIP_SRC work too): a Mesa source
#                   tree on this machine for the AppImage's Turnip instead of nixpkgs' Mesa release
#                   (packaging/default.nix), e.g. the turnip/gen8 branch (Adreno 8xx, the Odin 3):
#                     git clone --depth 1 -b turnip/gen8 https://github.com/whitebelyash/mesa-unified
#                   Mounted read-only into the container. (x86_64 packages nixpkgs' Mesa with the
#                   radeon and intel Vulkan drivers, no Turnip.)
#   DC_TURNIP_PATCHES  0: build that tree without the port's Turnip patches (default: applied where
#                   they apply, Adreno 830 chip IDs added where missing; GR2_/ISS_/BB_TURNIP_PATCHES too)
#   DC_FEX_STRICT   1: stop when third_party/FEX is not at the commit the guest CPU was tested with (default:
#                   a warning; cpu/guest_cpu_fex.cpp builds with FEX-2609.1 (9fbdc00) and FEX main of 2026-10-25 (1c4f04b))
#   DC_PACKAGE_SELFTEST  0: make the package without its check (every library the programs load
#                   resolves inside the package, run.sh parses, dc-prepare runs, the guest CPU runs
#                   x86-64 code (dc-cpu-selftest --quick); it stops a broken package)
#
# Written for macOS's stock bash 3.2 and any newer bash.
set -eo pipefail

# nixos-unstable, 2026-10-10: the same as the Bloodborne, inFAMOUS and Gravity Rush 2 ARM ports.
NIXPKGS_REV=6510408d147d8c5e6d76043e899e9251756f6a75
DC_LTO=${DC_LTO:-OFF}
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
SRC=${DC_SRC:-$HERE}
ARCH=${DC_ARCH:-}
MODE=build
FEX_DIR=third_party/FEX
# The FEX commit cpu/guest_cpu_fex.cpp is written against: the Bloodborne ARM port's submodule
# (seregasmith/ARM_linux_bbport, branch arm64-fex; FEX-2609.1).
FEX_COMMIT=9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf

die() { echo "ERROR: $*" >&2; exit 1; }
usage() { echo "Usage: $0 [--arch aarch64|x86_64] [build|appimage|shell|clean]" >&2; exit 2; }
# aarch64 | x86_64 | "" for the spellings uname, Docker and people use.
norm_arch() {
    case $1 in
        aarch64|arm64|armv8*) echo aarch64 ;;
        x86_64|amd64|x64|x86-64|x86) echo x86_64 ;;
        *) echo "" ;;
    esac
}

while [ $# -gt 0 ]; do
    case $1 in
        --arch) [ $# -ge 2 ] || usage; ARCH=$2; shift 2 ;;
        --arch=*) ARCH=${1#--arch=}; shift ;;
        -h|--help) awk 'NR > 1 && /^set -eo/ { exit } NR > 1 { sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
        build|appimage|shell|clean) MODE=$1; shift ;;
        package) MODE=appimage; shift ;;
        *) echo "ERROR: unknown argument '$1'" >&2; usage ;;
    esac
done
case $DC_LTO in ON|OFF) ;; *) die "DC_LTO must be ON or OFF" ;; esac
case ${BB_PGO:-} in ''|off|generate|use) ;; *) die "BB_PGO must be off, generate or use" ;; esac
TURNIP_SRC_IN=${DC_TURNIP_SRC:-${GR2_TURNIP_SRC:-${ISS_TURNIP_SRC:-${BB_TURNIP_SRC:-}}}}
TURNIP_PATCHES=${DC_TURNIP_PATCHES:-${GR2_TURNIP_PATCHES:-${ISS_TURNIP_PATCHES:-${BB_TURNIP_PATCHES:-}}}}
case $TURNIP_PATCHES in ''|0|1) ;; *) die "DC_TURNIP_PATCHES must be 0 or 1" ;; esac
case ${DC_PACKAGE_SELFTEST:-} in ''|0|1) ;; *) die "DC_PACKAGE_SELFTEST must be 0 or 1" ;; esac

command -v docker >/dev/null || die "docker not found. Install Docker Desktop, OrbStack, colima or Docker Engine."
docker info >/dev/null 2>&1 || die "Docker is installed but not running (or this user may not use it)."

DOCKER_ARCH=$(norm_arch "$(docker info --format '{{.Architecture}}' 2>/dev/null)")
[ -n "$DOCKER_ARCH" ] || die "cannot tell Docker's CPU architecture (docker info --format '{{.Architecture}}')"
if [ -z "$ARCH" ]; then
    ARCH=$DOCKER_ARCH
else
    wanted=$(norm_arch "$ARCH")
    [ -n "$wanted" ] || die "unknown target architecture '$ARCH' (aarch64 or x86_64)"
    ARCH=$wanted
fi
case $ARCH in
    aarch64) PLATFORM=linux/arm64 ;;
    x86_64)  PLATFORM=linux/amd64 ;;
esac
DOCKER_PLATFORM_ARCH=${PLATFORM#linux/}   # arm64 | amd64, as `docker image inspect` reports it

# Per target: Docker image, Nix store volume (a store holds one architecture's Nix itself, so they
# can't share one), and build directory.
if [ "$ARCH" = aarch64 ]; then
    IMAGE="dc-builder:${NIXPKGS_REV:0:8}"
    OWN_VOLUME=dc-nix
    BUILD_DIR=""
else
    IMAGE="dc-builder:${NIXPKGS_REV:0:8}-$ARCH"
    OWN_VOLUME=dc-nix-$ARCH
    BUILD_DIR=build-$ARCH
fi
NIX_VOLUME=${DC_NIX_VOLUME:-$OWN_VOLUME}
OUT_DESC=${BUILD_DIR:+$BUILD_DIR/}out

if [ "$MODE" = clean ]; then
    docker image rm "$IMAGE" 2>/dev/null || true
    echo "Removed the $ARCH build image $IMAGE."
    if [ "$NIX_VOLUME" = "$OWN_VOLUME" ]; then
        docker volume rm "$NIX_VOLUME" 2>/dev/null || true
        echo "Removed the $ARCH Nix store volume $NIX_VOLUME (the next build downloads it again)."
    else
        echo "Kept the Nix store volume $NIX_VOLUME: it is not this script's own ($OWN_VOLUME)."
    fi
    echo "The source tree was not touched (this target's build outputs: $OUT_DESC/, dist/)."
    exit 0
fi

# The source tree: the project's own files must be there, and the submodules checked out.
SRC=$(cd -- "$SRC" 2>/dev/null && pwd) || die "DC_SRC directory not found"
[ -f "$SRC/build.sh" ] && [ -f "$SRC/shell.nix" ] && [ -d "$SRC/gpu" ] && [ -f "$SRC/packaging/default.nix" ] ||
    die "$SRC is not the DriveClub runtime's Linux tree (no build.sh, shell.nix, gpu/, packaging/default.nix). Put this script in its root or set DC_SRC."
[ "$ARCH" != aarch64 ] || [ -f "$SRC/cpu/guest_cpu_fex.cpp" ] ||
    die "$SRC has no cpu/guest_cpu_fex.cpp (the FEXCore guest CPU for aarch64)."
is_empty() { [ -z "$(ls -A "$1" 2>/dev/null)" ]; }
submodule_paths() { # paths from a .gitmodules file, without git
    [ -f "$1" ] || return 0
    while read -r key _ value; do
        if [ "$key" = path ]; then echo "$value"; fi
    done < "$1"
}
missing=""
for p in $(submodule_paths "$SRC/.gitmodules"); do
    if [ "$p" = "$FEX_DIR" ]; then continue; fi   # FEX: aarch64 only, checked on its own below
    if is_empty "$SRC/$p"; then missing="$missing $p"; fi
done
if [ "$ARCH" = aarch64 ] && [ -f "$SRC/$FEX_DIR/.gitmodules" ]; then
    for p in $(submodule_paths "$SRC/$FEX_DIR/.gitmodules"); do
        case $p in *fex-*-bins|*Catch2) continue ;; esac   # FEX's test data: not needed to build
        if is_empty "$SRC/$FEX_DIR/$p"; then missing="$missing $FEX_DIR/$p"; fi
    done
fi
if [ -n "$missing" ]; then
    echo "ERROR: submodules are not checked out in $SRC:" >&2
    for p in $missing; do echo "  $p" >&2; done
    echo "This script runs no git commands on your repository. Check them out yourself:" >&2
    echo "  cd \"$SRC\" && git submodule update --init --recursive" >&2
    exit 1
fi
# The commit checked out in a submodule, read from its git directory (no git commands).
submodule_head() { # $1: submodule path in the tree
    local gitfile="$SRC/$1/.git" dir head ref
    if [ -f "$gitfile" ]; then
        dir=$(sed -n 's/^gitdir: //p' "$gitfile")
        case $dir in /*) ;; *) dir="$SRC/$1/$dir" ;; esac
    elif [ -d "$gitfile" ]; then
        dir=$gitfile
    else
        return 0
    fi
    head=$(cat "$dir/HEAD" 2>/dev/null || true)
    case $head in
        "ref: "*)
            ref=${head#ref: }
            cat "$dir/$ref" 2>/dev/null || grep " $ref\$" "$dir/packed-refs" 2>/dev/null | cut -d' ' -f1 || true ;;
        *) echo "$head" ;;
    esac
}
FEX_HEAD=""
if [ "$ARCH" = aarch64 ] && [ ! -f "$SRC/$FEX_DIR/External/vixl/CMakeLists.txt" ]; then
    echo "ERROR: FEX (FEXCore runs the game's x86-64 code on aarch64) is not checked out in $SRC/$FEX_DIR." >&2
    echo "This script runs no git commands on your repository. Add it yourself, at the commit the Bloodborne" >&2
    echo "ARM port builds with ($FEX_COMMIT):" >&2
    echo "  cd \"$SRC\" && git submodule add https://github.com/FEX-Emu/FEX.git $FEX_DIR" >&2
    echo "  git -C $FEX_DIR checkout $FEX_COMMIT && git submodule update --init --recursive $FEX_DIR" >&2
    echo "(already a submodule: cd \"$SRC\" && git submodule update --init --recursive $FEX_DIR)" >&2
    exit 1
fi
if [ "$ARCH" = aarch64 ]; then
    FEX_HEAD=$(submodule_head "$FEX_DIR")
    if [ -n "$FEX_HEAD" ] && [ "$FEX_HEAD" != "$FEX_COMMIT" ]; then
        if [ "${DC_FEX_STRICT:-}" = 1 ]; then kind=ERROR; else kind=WARNING; fi
        echo "$kind: $FEX_DIR has FEX ${FEX_HEAD:0:9} checked out; the guest CPU (cpu/guest_cpu_fex.cpp) was tested with" >&2
        echo "${FEX_COMMIT:0:9} (FEX-2609.1, the Bloodborne ARM port's). FEX's internal API changes between commits; to use that one:" >&2
        echo "  cd \"$SRC\" && git -C $FEX_DIR checkout $FEX_COMMIT" >&2
        echo "  git submodule update --init --recursive $FEX_DIR && git add $FEX_DIR" >&2
        [ "$kind" = WARNING ] || exit 1
    fi
fi

emulation_help() {
    echo "How to enable it:" >&2
    if [ "$(uname -s)" = Darwin ]; then
        echo "  Docker Desktop: Settings > General > \"Use Rosetta for x86_64/amd64 emulation on Apple Silicon\"" >&2
        echo "                  (arm64 containers on an Intel Mac: built in, QEMU)" >&2
        echo "  OrbStack:       built in" >&2
        echo "  colima:         colima start --vm-type vz --vz-rosetta   (Apple Silicon, x86_64 target)" >&2
    else
        echo "  Register QEMU for $PLATFORM (lasts until reboot):" >&2
        echo "    docker run --privileged --rm tonistiigi/binfmt --install $DOCKER_PLATFORM_ARCH" >&2
        echo "  or install your distribution's qemu-user-static package (binfmt support included)." >&2
    fi
}

echo "==> Target: $ARCH ($PLATFORM); Docker runs on $DOCKER_ARCH"
if [ "$ARCH" != "$DOCKER_ARCH" ]; then
    echo "==> Emulated build (Rosetta or QEMU): slower than a native $ARCH build. Checking that $PLATFORM containers run"
    got=$(docker run --rm --platform "$PLATFORM" nixos/nix:latest uname -m 2>&1 | tail -n 1) || true
    if [ "$(norm_arch "$got")" != "$ARCH" ]; then
        echo "ERROR: this Docker can't run $PLATFORM containers (test output: ${got:-nothing})." >&2
        emulation_help
        exit 1
    fi
    if [ "$ARCH" = aarch64 ]; then
        echo "    aarch64 under QEMU is many times slower than native; the appimage step's Turnip compile can take hours." >&2
    fi
fi

mem=$(docker info --format '{{.MemTotal}}' 2>/dev/null || echo 0)
ncpu=$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 0)
case $mem in ''|*[!0-9]*) mem=0 ;; esac
case $ncpu in ''|*[!0-9]*) ncpu=0 ;; esac
mem_mib=$((mem / 1048576))
# Heavy C++ files (shadPS4 core, FEXCore, -g) take up to ~2 GB each; ninja's default
# (cores + 2 jobs) runs a 10-core Mac out of memory.
if [ -z "${JOBS:-}" ]; then
    JOBS=$((mem_mib / 2048))
    if [ "$ncpu" -gt 0 ] && [ "$JOBS" -gt "$ncpu" ]; then JOBS=$ncpu; fi
    if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
fi
case $JOBS in ''|*[!0-9]*|0) die "JOBS must be a positive number" ;; esac
echo "==> Source tree: $SRC (build directory: $OUT_DESC/)"
if [ "$NIX_VOLUME" != "$OWN_VOLUME" ]; then
    echo "==> Nix store: shared volume $NIX_VOLUME"
fi
# A Mesa tree for Turnip (aarch64 appimage step): mounted read-only at /turnip-src in the container,
# where packaging/default.nix reads BB_TURNIP_SRC and BB_TURNIP_PATCHES.
turnip_flags=()
if [ -n "$TURNIP_SRC_IN" ]; then
    if [ "$ARCH" != aarch64 ]; then
        echo "WARNING: DC_TURNIP_SRC is for aarch64 builds only (Turnip is the Adreno driver); ignored for $ARCH." >&2
    else
        TURNIP_SRC=$(cd -- "$TURNIP_SRC_IN" 2>/dev/null && pwd) || die "Turnip source directory not found: $TURNIP_SRC_IN"
        [ -f "$TURNIP_SRC/VERSION" ] && [ -d "$TURNIP_SRC/src/freedreno/vulkan" ] ||
            die "not a Mesa source tree (no VERSION or src/freedreno/vulkan): $TURNIP_SRC"
        turnip_flags=(-v "$TURNIP_SRC":/turnip-src:ro -e BB_TURNIP_SRC=/turnip-src -e BB_TURNIP_PATCHES="$TURNIP_PATCHES")
        echo "==> Turnip: Mesa $(cat "$TURNIP_SRC/VERSION") from $TURNIP_SRC" \
             "(port's patches: $([ "$TURNIP_PATCHES" = 0 ] && echo off || echo where they apply))"
        [ "$MODE" = appimage ] || echo "    (used by the appimage step only)"
    fi
fi
echo "==> Docker VM: ${ncpu} CPUs, ${mem_mib} MiB RAM; compiling with JOBS=${JOBS}"
if [ "$mem_mib" -lt 7500 ]; then
    echo "WARNING: little RAM for this build. Give Docker 8 GB or more" \
         "(Docker Desktop: Settings > Resources > Memory), or expect a slow build." >&2
fi

# Linux host, rootful Docker Engine: the build's files belong to root on the host; the container
# gives them back to this user at the end (DC_OWNER). sudo ./build_docker.sh: to the sudo user.
OWNER=""
if [ "$(uname -s)" = Linux ]; then
    case "$(docker --version 2>/dev/null) $(docker info --format '{{.OperatingSystem}} {{.SecurityOptions}}' 2>/dev/null)" in
        # Podman, rootless Docker, Docker Desktop for Linux: they map the container's root to this
        # user already (a chown in the container would give the files to a subordinate ID).
        *odman*|*rootless*|*"Docker Desktop"*) ;;
        *) OWNER="${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}" ;;
    esac
fi

# Mount points that exist before Docker needs them (Docker Engine would create missing ones as
# root). x86_64: its own out/ and pgo/ go over the tree's.
mkdir -p "$SRC/out" "$SRC/pgo"
build_mounts=()
if [ -n "$BUILD_DIR" ]; then
    mkdir -p "$SRC/$BUILD_DIR/out" "$SRC/$BUILD_DIR/pgo"
    build_mounts=(-v "$SRC/$BUILD_DIR/out":/src/out -v "$SRC/$BUILD_DIR/pgo":/src/pgo)
fi

# Image: the official Nix image plus a pinned nixpkgs tree (NIX_PATH), so shell.nix's
# `import <nixpkgs>` always resolves to the same package set. git is there for build.sh's own
# `git rev-parse`; safe.directory lets it work on the mounted tree.
image_arch=$(docker image inspect --format '{{.Architecture}}' "$IMAGE" 2>/dev/null || true)
if [ "$image_arch" != "$DOCKER_PLATFORM_ARCH" ]; then
    if [ -n "$image_arch" ]; then echo "==> $IMAGE is a $image_arch image: building it again for $DOCKER_PLATFORM_ARCH"; fi
    echo "==> Building image $IMAGE ($PLATFORM)"
    docker build --platform "$PLATFORM" -t "$IMAGE" --build-arg NIXPKGS_REV="$NIXPKGS_REV" - <<'DOCKERFILE'
FROM nixos/nix:latest
ARG NIXPKGS_REV
RUN mkdir -p /opt/nixpkgs \
 && curl -fsSL "https://github.com/NixOS/nixpkgs/archive/${NIXPKGS_REV}.tar.gz" \
    | tar -xz --strip-components=1 -C /opt/nixpkgs \
 && (command -v git >/dev/null \
     || nix-env --option sandbox false --option filter-syscalls false --option build-users-group '' \
            -f /opt/nixpkgs -iA git) \
 && git config --global --add safe.directory '*'
ENV NIX_PATH=nixpkgs=/opt/nixpkgs
WORKDIR /src
DOCKERFILE
fi

# Runs inside the container, in /src (the mounted tree). $1 = mode.
# Only the Nix image's own tools here (bash, coreutils, findutils, grep, curl, tar, git): no sed
# outside nix-shell.
read -r -d '' INNER <<'INNER_SCRIPT' || true
set -euo pipefail
mode=$1; shift
cd /src
# Linux host with Docker Engine: root-owned files in what the build writes go back to the host
# user, also when the build fails. Ownership only.
if [ -n "${DC_OWNER:-}" ]; then
    give_back() {
        local p
        for p in out pgo dist packaging/runtime-paths.nix; do
            if [ -e "$p" ] || [ -L "$p" ]; then
                find "$p" -user 0 -exec chown -h "$DC_OWNER" {} + 2>/dev/null || true
            fi
        done
    }
    trap give_back EXIT
fi
# For every nix command here: flakes for `nix bundle` (AppImage), no build sandbox inside Docker,
# no separate build users (root in a throwaway container: builds do not depend on the image's
# nixbld group, which a rebuilt or pruned image may lack), no seccomp filter (unsupported under
# Rosetta and QEMU), and derivations built from source limited to $DC_JOBS cores, like the compiler.
# Without the sandbox every build runs with HOME=/homeless-shelter on the container's real file
# system, and Nix refuses to start a build while that directory exists ("home directory
# /homeless-shelter exists; please remove it"). Anything that writes to that home creates it and
# stops every build after it. Three guards:
# - HOME is /root and XDG_CACHE_HOME a folder on the Nix store volume for every command here,
#   nix-shell commands included (Nix itself keeps caches in $XDG_CACHE_HOME/nix, else ~/.cache/nix:
#   run with HOME=/homeless-shelter, it creates the directory);
# - a post-build hook removes it after each build (max-jobs = 1: builds run one at a time, and Nix
#   waits for the hook before it starts the next one);
# - each step here removes it before it starts and after it ends, and names what was in it.
# The cache on the volume: Nix keeps the flake inputs it downloaded there (the AppImage bundler and
# its own nixpkgs, ~40 MB from GitHub), and in the container's /root every appimage run downloaded
# them again.
export HOME=/root XDG_CACHE_HOME=/nix/var/cache/build-docker
mkdir -p /tmp/nixhooks "$XDG_CACHE_HOME"
printf '#!/bin/sh\nrm -rf /homeless-shelter\n' > /tmp/nixhooks/post-build
chmod +x /tmp/nixhooks/post-build
clear_homeless() { # $1: what left it (for the message)
    if [ -e /homeless-shelter ]; then
        echo "==> Removing /homeless-shelter left by $1 (contents: $(ls -A /homeless-shelter /homeless-shelter/.cache 2>/dev/null | tr '\n' ' '))"
        rm -rf /homeless-shelter
    fi
}
clear_homeless "the image"
# in_shell LABEL COMMAND: COMMAND in the build environment (shell.nix), with the home above.
in_shell() {
    local status=0
    clear_homeless "an earlier step"
    nix-shell shell.nix --run "export HOME=/root XDG_CACHE_HOME=$XDG_CACHE_HOME; $2" || status=$?
    clear_homeless "$1"
    return "$status"
}
export NIX_CONFIG="experimental-features = nix-command flakes
sandbox = false
build-users-group =
filter-syscalls = false
max-jobs = 1
cores = ${DC_JOBS:-2}
post-build-hook = /tmp/nixhooks/post-build"
# Optional GitHub token (any classic token, no scopes): 5000 API requests/hour instead of 60.
if [ -n "${GITHUB_TOKEN:-}" ]; then
    NIX_CONFIG="$NIX_CONFIG
access-tokens = github.com=${GITHUB_TOKEN}"
fi
echo "==> Container: $(uname -m)"
if [ "$DC_ARCH" = aarch64 ]; then
    echo "==> Kernel page size: $(grep -m1 KernelPageSize /proc/self/smaps | tr -s ' ' | cut -d' ' -f2-) (FEXCore assumes 4 kB)"
fi
if [ "$mode" = shell ]; then
    # exec would skip the EXIT trap: run the shell, then give the files back.
    nix-shell shell.nix --command "export HOME=/root XDG_CACHE_HOME=$XDG_CACHE_HOME; return"
    exit $?
fi
# build.sh calls plain `ninja` (cores + 2 jobs). This wrapper, first in PATH, adds -j $DC_JOBS.
mkdir -p /tmp/dcwrap
cat > /tmp/dcwrap/ninja <<'WRAPPER'
#!/bin/sh
real=$(PATH=${PATH#/tmp/dcwrap:}; export PATH; command -v ninja)
exec "$real" -j "${DC_JOBS:-2}" "$@"
WRAPPER
chmod +x /tmp/dcwrap/ninja
if [ "$DC_ARCH" = aarch64 ]; then
    # FEX turns LTO on by default, which needs llvm-ar and LLVM's linker plugin; shell.nix's clang
    # has neither ("CMAKE_C_COMPILER_AR-NOTFOUND: command not found"). build.sh configures FEX only
    # when out/fex/build.ninja is missing, and CMake keeps existing cache entries, so a pre-seeded
    # ENABLE_LTO=OFF turns it off without touching build.sh.
    if [ -f out/fex/CMakeCache.txt ] && ! grep -q '^ENABLE_LTO:BOOL=OFF$' out/fex/CMakeCache.txt; then
        echo "==> FEX was configured with LTO: starting its build directory (out/fex) over without it"
        rm -rf out/fex
    fi
    # Another FEX commit than the one out/fex was configured for: start that directory over too.
    if [ -n "${DC_FEX_HEAD:-}" ] && [ -f out/fex/CMakeCache.txt ] && [ "$(cat out/fex/.dc-fex-commit 2>/dev/null)" != "$DC_FEX_HEAD" ]; then
        echo "==> third_party/FEX is now ${DC_FEX_HEAD:0:9}: starting its build directory (out/fex) over"
        rm -rf out/fex
    fi
    if [ ! -f out/fex/CMakeCache.txt ]; then
        mkdir -p out/fex && echo 'ENABLE_LTO:BOOL=OFF' > out/fex/CMakeCache.txt
    fi
    [ -z "${DC_FEX_HEAD:-}" ] || echo "$DC_FEX_HEAD" > out/fex/.dc-fex-commit
fi
echo "==> Building for ${DC_ARCH} (BB_LTO=${BB_LTO}, BB_PGO=${BB_PGO:-build.sh default}, ${DC_JOBS} parallel jobs); logs: out/gpu-build.log$([ "$DC_ARCH" = aarch64 ] && echo ', out/fex-build.log')"
in_shell "the build" 'PATH=/tmp/dcwrap:$PATH bash build.sh'
echo "==> Built:"
in_shell "the build summary" \
    'for f in out/DriveClubGame out/dc-launcher out/dc-prepare out/dc-cpu-selftest out/gpu/libbbgpu.so out/fex/*.so; do [ -f "$f" ] && printf "  %-24s %s\n" "$f" "$(readelf -h "$f" | sed -n "s/ *Machine: *//p")"; done; true'
if [ "$mode" = appimage ]; then
    # appimage.sh names the bundler without a commit (github:ralismark/nix-appimage), so Nix asks
    # GitHub's API for its latest commit: 60 anonymous requests per hour per IP, often used up on
    # shared IPs. A pinned bundler (BB_APPIMAGE_BUNDLER) downloads as a plain archive.
    bundler_rev=7946addbc0d97e358a6d7aefe5e82310f0fe6b18   # ralismark/nix-appimage, 2025-09-15
    if [ "$DC_ARCH" = aarch64 ]; then
        echo "==> Packaging the AppImage (nix bundle). The first run builds the patched Turnip (Mesa) driver" \
             "from source$([ -n "${BB_TURNIP_SRC:-}" ] && echo ', from the mounted Mesa tree')"
    else
        echo "==> Packaging the AppImage (nix bundle)"
    fi
    if [ "${DC_PACKAGE_SELFTEST:-}" = 0 ]; then
        echo "    DC_PACKAGE_SELFTEST=0: without the package's selftest"
    fi
    # A download cut short or refused (GitHub) is not the package's fault: up to three tries.
    for attempt in 1 2 3; do
        if in_shell "the packaging" "set -o pipefail; BB_NIXPKGS=/opt/nixpkgs BB_APPIMAGE_BUNDLER=github:ralismark/nix-appimage/$bundler_rev bash packaging/appimage.sh 2>&1 | tee /tmp/appimage.log"; then
            exit 0
        fi
        grep -q -E 'Truncated tar archive|unable to download|Failure when receiving data|Connection reset|Could not resolve host' \
            /tmp/appimage.log || break
        [ "$attempt" -lt 3 ] || break
        echo "==> A download failed (see above): trying again in 10 s (try $((attempt + 1)) of 3)"
        sleep 10
    done
    # The bundler's last step packs the package's closure with `mksquashfs <each store path> ...
    # -no-strip`. Under emulation (an x86_64 build on Apple Silicon: Rosetta or QEMU) mksquashfs
    # 4.6.1 fails there with any two sources ("Can't have two different sources with same
    # pathname"), natively it works. Everything that step needs is built by then, so it is done
    # here instead with the bundler's own commands, its one -no-strip call given the closure as a
    # single staged directory (hard links next to the store: no copies). Then the rest of
    # appimage.sh: the AppImage into dist/, the package's GC root. (As in the Bloodborne port.)
    drv=$(grep -o "/nix/store/[a-z0-9]\{32\}-[^ '\"]*\.AppImage\.drv" /tmp/appimage.log | head -n 1 || true)
    if ! grep -q "two different sources with same pathname" /tmp/appimage.log || [ -z "$drv" ]; then
        exit 1
    fi
    echo "==> mksquashfs can't pack the closure list under emulation; packing the AppImage here from $drv"
    cat > /tmp/assemble-appimage.sh <<'ASSEMBLE'
# $1: the bundler's AppImage derivation, $2: where to write the AppImage.
set -euo pipefail
drv=$1 result=$2
text=$(cat "$drv")
# Its build command: the "buildCommand" string of the derivation's environment, unescaped.
cmd=${text#*\(\"buildCommand\",\"}
cmd=${cmd%%\"\),\(*}
p=$'\x01'
cmd=${cmd//\\\\/$p}; cmd=${cmd//\\n/$'\n'}; cmd=${cmd//\\t/$'\t'}; cmd=${cmd//\\r/$'\r'}
cmd=${cmd//\\\"/\"}; cmd=${cmd//$p/\\}
inputs=${text#*\(\"nativeBuildInputs\",\"}
inputs=${inputs%%\"*}
squashfs=""
for w in $inputs; do if [ -x "$w/bin/mksquashfs" ]; then squashfs=$w; fi; done
[ -n "$squashfs" ] || { echo "ERROR: no mksquashfs among the derivation's inputs ($inputs)" >&2; exit 1; }
case $cmd in *mksquashfs*-no-strip*) ;; *) echo "ERROR: unexpected build command in $drv" >&2; exit 1 ;; esac
export PATH="$squashfs/bin:$PATH"
stage=/nix/.dc-appimage-root   # in the store's file system, so cp -l makes hard links
# mksquashfs SOURCES... DEST OPTIONS...: with -no-strip, the sources become one directory tree.
mksquashfs() {
    case " $* " in *" -no-strip "*) ;; *) command mksquashfs "$@"; return ;; esac
    local args=("$@") i=0 a opts=()
    while [ "$i" -lt "${#args[@]}" ] && [[ ${args[i]} != -* ]]; do i=$((i + 1)); done
    local dest=${args[i - 1]} sources=("${args[@]:0:i-1}")
    for a in "${args[@]:i}"; do [ "$a" = -no-strip ] || opts+=("$a"); done
    rm -rf "$stage"
    for a in "${sources[@]}"; do
        mkdir -p "$stage$(dirname "$a")"
        cp -al "$a" "$stage$a" 2>/dev/null || cp -a "$a" "$stage$a"
    done
    echo "  mksquashfs: ${#sources[@]} store paths as one directory"
    command mksquashfs "$stage" "$dest" "${opts[@]}"
    rm -rf "$stage"
}
work=$(mktemp -d)
trap 'rm -rf "$work" "$stage"' EXIT
cd "$work"
export out=$work/driveclub-recomp.AppImage
eval "$cmd"
install -m755 "$out" "$result"
ASSEMBLE
    mkdir -p dist
    in_shell "the AppImage assembly" "bash /tmp/assemble-appimage.sh '$drv' /src/dist/DriveClub-recomp-$DC_ARCH.AppImage"
    # appimage.sh's last steps: a GC root for the package (keeps what was built from source).
    in_shell "the GC root" 'nix-build --impure packaging -o out/nix-roots/driveclub > /dev/null'
    ls -lh "dist/DriveClub-recomp-$DC_ARCH.AppImage"
fi
INNER_SCRIPT

tty_flags=()
if [ -t 0 ] && [ -t 1 ]; then tty_flags=(-it); fi

# "${arr[@]+"${arr[@]}"}" keeps bash 3.2 happy with an empty array.
docker run --rm ${tty_flags[@]+"${tty_flags[@]}"} \
    --platform "$PLATFORM" \
    -v "$NIX_VOLUME":/nix -v "$SRC":/src \
    ${build_mounts[@]+"${build_mounts[@]}"} \
    -e DC_ARCH="$ARCH" -e DC_OWNER="$OWNER" -e DC_JOBS="$JOBS" -e DC_FEX_HEAD="$FEX_HEAD" \
    -e BB_LTO="$DC_LTO" -e BB_PGO="${BB_PGO:-}" \
    -e GITHUB_TOKEN="${GITHUB_TOKEN:-}" -e DC_PACKAGE_SELFTEST="${DC_PACKAGE_SELFTEST:-}" \
    -e SDL_VIDEO_DRIVER=dummy -e SDL_AUDIO_DRIVER=dummy \
    ${turnip_flags[@]+"${turnip_flags[@]}"} \
    "$IMAGE" bash -c "$INNER" inner "$MODE"

case $MODE in
    shell) ;;
    appimage) echo "==> Done. $SRC/dist/DriveClub-recomp-$ARCH.AppImage (for $ARCH Linux; run it, or with --update DIR --base DIR the first time)." ;;
    *) echo "==> Done. Linux $ARCH build in $SRC/$OUT_DESC (runs inside the container, or packaged with: $0 --arch $ARCH appimage)." ;;
esac
