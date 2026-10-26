#!/usr/bin/env bash
# Builds the game runtime. See BUILDING.md.
#   Windows (MSYS2 CLANG64): out/DriveClubGame.exe
#   Linux x86-64 / aarch64:  out/DriveClubGame, out/gpu/libbbgpu.so, out/dc-prepare, out/dc-cpu-selftest
#                            (aarch64 also out/fex/libbbcpu.so: the game's x86-64 code runs in FEXCore's JIT)
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out

case $(uname -s) in
MINGW*|MSYS*|CYGWIN*)
    # shellcheck source=msys2-env.sh
    source ./msys2-env.sh
    for tool in clang cmake ninja pkg-config; do
        command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64 (see BUILDING.md)." >&2; exit 1; }
    done
    # Source paths in the binary (assert and log messages) are written relative to the repository.
    root="$(cygpath -m "$PWD")"
    cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off \
        -DBB_LTO="${BB_LTO:-OFF}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="-ffile-prefix-map=$root=." -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$root=." >/dev/null
    if ! ninja -C out/gpu DriveClubGame > out/gpu-build.log 2>&1; then
        grep -v '^\[' out/gpu-build.log | tail -40 >&2
        echo 'Build failed (full log: out/gpu-build.log)' >&2
        exit 1
    fi
    cp out/gpu/DriveClubGame.exe out/
    echo "Built $PWD/out/DriveClubGame.exe"
    exit 0
    ;;
esac

# ---- Linux -------------------------------------------------------------------------------------------------------
# Compilers: Clang by default (the Windows build's compiler; FEX needs it anyway), whatever CC/CXX say (nix-shell
# sets them to GCC); GCC with BB_CC=gcc BB_CXX=g++.
CC=${BB_CC:-$(command -v clang || echo "${CC:-}")}
CXX=${BB_CXX:-$(command -v clang++ || echo "${CXX:-}")}
if [[ -z $CC ]]; then CC=$(command -v cc || command -v gcc || true); fi
if [[ -z $CXX ]]; then CXX=$(command -v c++ || command -v g++ || true); fi
if [[ -z $CC || -z $CXX ]]; then echo 'Install Clang (or GCC) or set BB_CC/BB_CXX.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS (and in build_docker.sh) the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
arch=$(uname -m)

# GPU library (shadPS4 video core + drivers + network module), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off (default: use when pgo/
# holds a profile; GCC only). BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$PWD=." -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$PWD=." >/dev/null
echo "GPU library: $(basename "$CXX"), PGO $pgo, LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)

# Hosts other than x86-64: the game's code runs in FEXCore's JIT, linked into out/fex/libbbcpu.so
# (cpu/: FEX's CMake project builds it, cpu/fex_project.cmake adds the target). FEX needs Clang.
cpu=()
if [[ $arch == aarch64 ]]; then
    if [[ ! -f third_party/FEX/External/vixl/CMakeLists.txt ]]; then
        if git rev-parse --git-dir >/dev/null 2>&1; then git submodule update --init --recursive third_party/FEX; fi
    fi
    [[ -f third_party/FEX/CMakeLists.txt ]] || { echo 'third_party/FEX is missing: git submodule update --init --recursive' >&2; exit 1; }
    if [[ ! -f out/fex/build.ninja ]]; then
        CC=${FEX_CC:-clang} CXX=${FEX_CXX:-clang++} cmake -S third_party/FEX -B out/fex -G Ninja \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_PROJECT_FEX_INCLUDE="$PWD/cpu/fex_project.cmake" \
            -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DBUILD_STEAM_SUPPORT=OFF \
            -DBUILD_FEX_LINUX_TESTS=OFF -DENABLE_ZYDIS=OFF -DTUNE_ARCH="${FEX_TUNE_ARCH:-armv8.2-a}" \
            -DTUNE_CPU="${FEX_TUNE_CPU:-generic}" >/dev/null
    fi
    if ! ninja -C out/fex bbcpu > out/fex-build.log 2>&1; then
        grep -v '^\[' out/fex-build.log | tail -40 >&2
        echo 'Guest CPU library build failed (full log: out/fex-build.log)' >&2; exit 1
    fi
    echo 'Guest CPU: FEXCore (out/fex/libbbcpu.so)'
    cpu=(-Lout/fex -lbbcpu -Wl,-rpath,'$ORIGIN/cpu' -Wl,-rpath,"$PWD/out/fex")
elif [[ $arch != x86_64 ]]; then
    echo "Unsupported host architecture $arch (x86_64 or aarch64)." >&2; exit 1
fi

runtime=(src/runtime*.c)
# Third-party decoders: compiled once, without this project's warning policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -fPIC -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
# The loader is not position independent: the guest packs host pointers (TLS, thread handles in the brk heap)
# into 40 bits, so the executable and its heap stay low.
cflags=(-std=c11 -O2 -g -Wall -Wextra -Wno-format-truncation -Wno-unknown-pragmas -Wno-unknown-warning-option
        -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -pthread -fno-pie
        -ffile-prefix-map="$PWD"=.)
"$CC" "${cflags[@]}" -no-pie "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c \
    out/libatrac9.a -lm -ldl "${gpu[@]}" "${cpu[@]}" "${libraries[@]}" -o out/DriveClubGame
echo "Built $PWD/out/DriveClubGame"

# The guest CPU's self-test without the game (run.sh --cpu-selftest): FEXCore on ARM64, the host CPU on x86-64.
selftest=(tools/cpu_selftest.c)
[[ $arch == x86_64 ]] && selftest+=(src/runtime_cpu.c)
"$CC" -std=c11 -O2 -g -Wall -Wextra -pthread -Isrc "${selftest[@]}" "${cpu[@]}" -o out/dc-cpu-selftest
echo "Built $PWD/out/dc-cpu-selftest"

# First-run preparation (the launcher's, launcher/prepare.cpp) as a command for run.sh.
"$CXX" -std=c++20 -O2 -Wall -Wextra tools/dc_prepare.cpp -o out/dc-prepare
echo "Built $PWD/out/dc-prepare"

