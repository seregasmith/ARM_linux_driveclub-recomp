# Linux build environment: `nix-shell shell.nix --run 'bash build.sh'` (build.sh enters it by itself when
# pkg-config cannot find Vulkan and SDL3; build_docker.sh uses it inside Docker). Adapted from the bbport
# ARM64 port's shell.nix.
{ pkgs ? import <nixpkgs> {} }:
let
  # libdecor (window frames on GNOME's Wayland) without its GTK 3 plugin; its cairo plugin draws the frames.
  libdecor = pkgs.libdecor.overrideAttrs (old: {
    buildInputs = pkgs.lib.remove pkgs.gtk3 old.buildInputs;
    mesonFlags = old.mesonFlags ++ [ "-Dgtk=disabled" ];
  });
  # SDL3 names zenity (its message boxes) by store path, and zenity brings a whole GTK4 with GStreamer into
  # the AppImage. This one runs the host's zenity when there is one.
  sdl3 = pkgs.sdl3.override {
    inherit libdecor;
    zenity = pkgs.writeShellScriptBin "zenity" ''
      host=$(command -v zenity) && exec "$host" "$@"
      exit 1
    '';
  };
in
pkgs.mkShell ({
  packages = with pkgs; [
    clang gcc gnumake cmake ninja pkg-config python3 binutils git
    vulkan-headers vulkan-loader sdl3
    # GPU library (gpu/): shadPS4 video core dependencies
    ffmpeg-headless boost fmt magic-enum robin-map xxhash vulkan-memory-allocator glslang spirv-cross xbyak zydis
    spirv-headers miniz libx11 libxcb xorgproto wayland
    # Network module (gpu/bbnet): shadNet client, HTTPS, UPnP
    protobuf nlohmann_json openssl zlib miniupnpc
  ];
} // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isAarch64 {
  # Guest CPU library (cpu/: FEXCore) builds with Clang only (build.sh).
  FEX_CC = "${pkgs.clang}/bin/clang";
  FEX_CXX = "${pkgs.clang}/bin/clang++";
})
