{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  addDriverRunpath,
  libdrm,
  libGL,
  libva,
  gst_all_1,
  callPackage,
  # NVENC/NVDEC headers. Deliberately not pkgs.nv-codec-headers: nixpkgs
  # stops at 12.1, which is too old for this fork's encoder
  # (see nix/nv-codec-headers.nix).
  nvCodecHeaders ? callPackage ./nv-codec-headers.nix { },
  # gstreamer-codecparsers-1.0 is optional in meson.build; without it the VP9
  # decoder is left out of the build.
  withVp9 ? true,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "nvidia-vaapi-driver";
  version = "0.1-nvenc";

  # Only the inputs meson actually reads, so editing the README or the
  # install scripts does not invalidate the build.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../meson.build
      ../meson_options.txt
      ../src
      ../nvidia-include
      ../subprojects
      ../tests
      ../samples
      ../COPYING
    ];
  };

  strictDeps = true;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    addDriverRunpath
  ];

  buildInputs = [
    libdrm
    libGL # EGL, via libglvnd
    libva
    nvCodecHeaders
  ]
  ++ lib.optionals withVp9 [
    gst_all_1.gstreamer
    gst_all_1.gst-plugins-bad
  ];

  # Upstream installs the driver next to libva's own drivers, which is a
  # read-only store path here. Put it under our own prefix instead; consumers
  # point LIBVA_DRIVERS_PATH at it (NixOS: hardware.graphics.extraPackages).
  postPatch = ''
    substituteInPlace meson.build \
      --replace-fail "nvidia_install_dir = libva_deps.get_variable(pkgconfig: 'driverdir')" \
                     "nvidia_install_dir = get_option('libdir') / 'dri'"
  '';

  # The meson suite drives a real NVENC/NVDEC engine through a render node, so
  # it cannot run in the sandbox. Run it from `nix develop` instead.
  doCheck = false;

  # libcuda.so.1 / libnvcuvid.so.1 / libnvidia-encode.so.1 are dlopened by
  # ffnvcodec's loader at runtime and are not linkable at build time.
  postFixup = ''
    addDriverRunpath "$out/lib/dri/nvidia_drv_video.so"
    if [ -e "$out/libexec/nvenc-helper" ]; then
      addDriverRunpath "$out/libexec/nvenc-helper"
    fi
  '';

  passthru = {
    # Convenience for consumers: LIBVA_DRIVERS_PATH value.
    driversPath = "${finalAttrs.finalPackage}/lib/dri";
  };

  meta = {
    description = "VA-API implementation for NVIDIA GPUs, with NVENC encode support";
    homepage = "https://github.com/elFarto/nvidia-vaapi-driver";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "nvenc-helper";
  };
})
