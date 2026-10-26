# Packaged DriveClub runtime for Linux: the prebuilt binaries (build.sh first), run.sh, the patches and Mesa's
# Vulkan drivers, with their whole Nix closure. `bash packaging/appimage.sh` turns it into an AppImage;
# `nix-build packaging` alone gives result/bin/driveclub. Adapted from the bbport ARM64 port's packaging.
{ pkgs ? import <nixpkgs> { }
  # Store paths the prebuilt binaries load libraries from (their RUNPATHs), written by
  # appimage.sh. Nix only finds references to its inputs, and the binaries were built outside.
, runtimePaths ? (if builtins.pathExists ./runtime-paths.nix then import ./runtime-paths.nix else [ ])
}:
let
  lib = pkgs.lib;
  # aarch64 hosts: the game's x86-64 code runs in FEXCore (out/fex/libbbcpu.so, build.sh).
  arm = pkgs.stdenv.hostPlatform.isAarch64;
  root = ./..;
  wanted = [
    "run.sh" "out" "out/DriveClubGame" "out/dc-prepare" "out/dc-cpu-selftest" "out/gpu" "out/gpu/libbbgpu.so" "patches"
    "patches/DriveClub.xml" "packaging" "packaging/dc_vulkan.py"
  ] ++ lib.optionals arm [ "out/fex" "out/fex/libbbcpu.so" ];
  src = builtins.path {
    name = "driveclub-src";
    path = root;
    filter = path: type:
      let rel = lib.removePrefix (toString root + "/") (toString path);
      in builtins.elem rel wanted;
  };
  python = pkgs.python3;
  # Mesa comes with the package. dc_vulkan.py adds the host NVIDIA ICD (x86-64) and only its vendor libraries
  # (matching the host's kernel module).
  # aarch64: Adreno (Turnip), Mali (Panfrost), Raspberry Pi (V3DV), Apple (Asahi), AMD.
  icdArch = pkgs.stdenv.hostPlatform.parsed.cpu.name;
  icdNames = if arm then [ "freedreno" "panfrost" "broadcom" "asahi" "radeon" ] else [ "radeon" "intel" ];
  # Mesa 26's Turnip loses the device on the Adreno 740 (SM8550) without these patches. Only freedreno is
  # rebuilt; the other drivers stay the binary cache's Mesa.
  turnipPatches = [
    ./mesa-sm8550/0001-add-a830-chip-id.patch
    ./mesa-sm8550/0001-freedreno-ir3-vulkan-disable-bindless-ubo-const-lowering.patch
    ./mesa-sm8550/001-fix-freedreno-vulkan.patch
  ];
  # BB_TURNIP_SRC: a Mesa source tree for Turnip instead of nixpkgs' release, e.g. a checkout of
  # whitebelyash/mesa-unified's turnip/gen8 branch (Adreno 8xx work not yet in Mesa, the Odin 3's Adreno 830)
  # or Mesa main. The Adreno 830 chip IDs of 0001-add-a830-chip-id.patch are added where the tree lacks them;
  # the other patches are applied where they apply, skipped where the tree has them already, and a conflict
  # stops the build. BB_TURNIP_PATCHES=0 builds the tree as it is.
  turnipSrc = builtins.getEnv "BB_TURNIP_SRC";
  turnipPortPatches = builtins.getEnv "BB_TURNIP_PATCHES" != "0";
  turnip = (pkgs.mesa.override {
    galliumDrivers = [ "freedreno" ];
    vulkanDrivers = [ "freedreno" ];
    vulkanLayers = [ ];
    enablePatentEncumberedCodecs = false;
  }).overrideAttrs (old: {
    patches = old.patches ++ turnipPatches;
    mesonFlags = map (flag: if lib.hasPrefix "-Dtools=" flag then "-Dtools=" else flag) old.mesonFlags
      ++ [ "-Dgallium-va=disabled" ];
    postInstall = old.postInstall + ''
      mkdir -p $opencl $spirv2dxil
    '';
  } // lib.optionalAttrs (turnipSrc != "") {
    version = lib.removeSuffix "\n" (builtins.readFile (/. + turnipSrc + "/VERSION")) + "-src";
    src = builtins.path {
      name = "mesa-src";
      path = /. + turnipSrc;
      filter = path: type: baseNameOf path != ".git";
    };
    # nixpkgs' own patches only (the port's are applied below, where they still apply).
    patches = old.patches;
    postPatch = ''
      patchShebangs .
    '' + lib.optionalString turnipPortPatches ''
      python3 - src/freedreno/common/freedreno_devices.py <<'PY'
      import re, sys
      path = sys.argv[1]
      s = open(path).read()
      anchor = 'GPUId(chip_id=0xffff44050000, name="Adreno (TM) 830"),'
      if anchor not in s:
          sys.exit("driveclub: no Adreno 830 entry in freedreno_devices.py (BB_TURNIP_PATCHES=0: none of the port's changes)")
      added = []
      for chip in ("0xffff44050001", "0x44050000"):
          if not re.search(r"chip_id=" + chip + r"\b", s, re.I):
              s = s.replace(anchor, anchor + "\n        GPUId(chip_id=" + chip + ", name=\"Adreno (TM) 830\"),", 1)
              added.append(chip)
      open(path, "w").write(s)
      print("driveclub: Adreno 830 chip IDs", "added: " + ", ".join(added) if added else "already in the tree")
      PY
      dc_patch() {
        if patch -p1 -R --dry-run -s -f < "$1" > /dev/null 2>&1; then
          echo "driveclub: $(basename "$1" | cut -d- -f2-) is already in this Mesa tree"
        elif patch -p1 --dry-run -s -f < "$1" > /dev/null 2>&1; then
          patch -p1 -s -f < "$1"
          echo "driveclub: applied $(basename "$1" | cut -d- -f2-)"
        else
          echo "driveclub: $(basename "$1" | cut -d- -f2-) does not apply to this Mesa tree (BB_TURNIP_PATCHES=0: build without the port's patches)" >&2
          exit 1
        fi
      }
      dc_patch ${./mesa-sm8550/0001-freedreno-ir3-vulkan-disable-bindless-ubo-const-lowering.patch}
      dc_patch ${./mesa-sm8550/001-fix-freedreno-vulkan.patch}
    '';
  });
  icdMesa = name: if arm && name == "freedreno" then turnip else pkgs.mesa;
  icds = lib.concatMapStringsSep ":"
    (name: "${icdMesa name}/share/vulkan/icd.d/${name}_icd.${icdArch}.json") icdNames;
  mangohud = pkgs.mangohud;
  # DC_PACKAGE_SELFTEST=0 (build_docker.sh, appimage.sh run nix with --impure): the package without its check.
  selftest = builtins.getEnv "DC_PACKAGE_SELFTEST" != "0";
in
pkgs.stdenv.mkDerivation {
  pname = "driveclub-recomp";
  version = "0.1";
  inherit src;
  nativeBuildInputs = [ pkgs.makeShellWrapper ];
  buildInputs = map builtins.storePath runtimePaths;
  dontBuild = true;
  dontConfigure = true;
  # The binaries live under share/ (next to run.sh): strip them too, which also drops the compiler and header
  # paths their debug info would keep in the closure.
  stripDebugList = [ "share/driveclub/bin" ];
  # patchelf (RPATH shrinking) corrupts the non-PIE game binary's symbol versions; it finds its libraries
  # through $ORIGIN/gpu, $ORIGIN/cpu and the build's RUNPATH.
  dontPatchELF = true;
  installPhase = ''
    runHook preInstall
    d=$out/share/driveclub
    mkdir -p $d/bin $d/patches $out/bin
    install -m755 run.sh $d/run.sh
    install -m644 patches/DriveClub.xml $d/patches/DriveClub.xml
    install -m644 packaging/dc_vulkan.py $d/dc_vulkan.py
    install -m755 out/DriveClubGame $d/bin/DriveClubGame
    install -m755 out/dc-prepare $d/bin/dc-prepare
    install -m755 out/dc-cpu-selftest $d/bin/dc-cpu-selftest
    install -Dm755 out/gpu/libbbgpu.so $d/bin/gpu/libbbgpu.so
    ${lib.optionalString arm "install -Dm755 out/fex/libbbcpu.so $d/bin/cpu/libbbcpu.so"}
    # Their debug info names the build environment's headers (the full compilers, Vulkan headers): Nix took
    # those for dependencies. Symbols stay (crash reports).
    ${pkgs.binutils-unwrapped}/bin/strip --strip-debug $d/bin/DriveClubGame $d/bin/dc-prepare $d/bin/dc-cpu-selftest
    runHook postInstall
  '';
  postFixup = ''
    # bin/driveclub is a static program (dc-entry.c) that clears the host's LD_PRELOAD (Steam's overlay) and
    # similar before the wrapper's own dynamic programs start.
    mkdir -p $out/libexec
    gcc -O2 -Wall -Werror -static -L${pkgs.glibc.static}/lib -DTARGET="\"$out/libexec/driveclub\"" \
      ${./dc-entry.c} -o $out/bin/driveclub
    makeShellWrapper ${python}/bin/python3 $out/libexec/driveclub \
      --add-flags "$out/share/driveclub/dc_vulkan.py ${pkgs.bash}/bin/bash $out/share/driveclub/run.sh" \
      --prefix XDG_DATA_DIRS : ${mangohud}/share \
      --set-default LC_ALL C.UTF-8 \
      --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath [ pkgs.libglvnd pkgs.libx11 pkgs.libxext ]} \
      --set BB_VULKANINFO ${pkgs.vulkan-tools}/bin/vulkaninfo \
      --set BB_PREBUILT 1 \
      --set BB_BUNDLED_VK_DRIVER_FILES ${icds} \
      --prefix PATH : ${lib.makeBinPath [ pkgs.bash pkgs.coreutils pkgs.gawk pkgs.gnused pkgs.gnugrep pkgs.procps ]} \
      --run '[ -r "''${SSL_CERT_FILE:-}" ] || export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt; [ -d "''${SSL_CERT_DIR:-}" ] || unset SSL_CERT_DIR' \
      --run 'export DC_DATA_DIR=''${DC_DATA_DIR:-''${XDG_DATA_HOME:-$HOME/.local/share}/driveclub-recomp}; export BB_DATA_DIR=$DC_DATA_DIR; mkdir -p "$DC_DATA_DIR"'
  '' + lib.optionalString selftest ''
    # Selftest: every library the programs load resolves (the dynamic loader lists them without running the
    # program), run.sh parses, dc-prepare reads the patches, dc_vulkan.py compiles, and the guest CPU (FEXCore on
    # aarch64) runs x86-64 code: calls both ways, SSE/AVX, atomics, threads (4 KB-page kernels only, as the game).
    d=$out/share/driveclub
    for b in $d/bin/DriveClubGame $d/bin/dc-prepare $d/bin/dc-cpu-selftest; do
      if LD_TRACE_LOADED_OBJECTS=1 $b | grep -q 'not found'; then
        echo "selftest: libraries of $b not found:" >&2
        LD_TRACE_LOADED_OBJECTS=1 $b | grep 'not found' >&2
        exit 1
      fi
    done
    ${pkgs.bash}/bin/bash -n $d/run.sh
    $d/bin/dc-prepare --patches-xml $d/patches/DriveClub.xml --list > /dev/null
    ${python}/bin/python3 -c 'import sys; compile(open(sys.argv[1]).read(), sys.argv[1], "exec")' $d/dc_vulkan.py
    if [ "$(getconf PAGESIZE)" = 4096 ]; then
      $d/bin/dc-cpu-selftest --quick
    else
      echo "selftest: $(getconf PAGESIZE)-byte pages here, guest CPU not checked"
    fi
    echo "selftest: package ok"
  '';
  passthru = { inherit turnip; };
  meta.mainProgram = "driveclub";
}
