{
  mkShell,
  nvidia-vaapi-driver,
  meson,
  ninja,
  pkg-config,
  ffmpeg,
  libva-utils,
  gdb,
  gst_all_1,
  lib,
}:

let
  # videotestsrc/mp4mux/parsers for tests/test_gstreamer.sh, plus the `va`
  # elements it looks for.
  gstPlugins = with gst_all_1; [
    gst-plugins-base
    gst-plugins-good
    gst-plugins-bad
  ];
in
mkShell {
  # Compilers, headers and pkg-config paths from the real build.
  inputsFrom = [ nvidia-vaapi-driver ];

  packages = [
    meson
    ninja
    pkg-config
    gdb
    # Used by the meson test suite: ffmpeg/vainfo smoke tests and the
    # gst-launch encode pipelines.
    ffmpeg
    libva-utils
    gst_all_1.gstreamer
  ]
  ++ gstPlugins;

  env = {
    GST_PLUGIN_SYSTEM_PATH_1_0 = lib.makeSearchPathOutput "lib" "lib/gstreamer-1.0" (
      gstPlugins ++ [ gst_all_1.gstreamer ]
    );
    LIBVA_DRIVER_NAME = "nvidia";
    NVD_LOG = "1";
  };

  shellHook = ''
    echo "nvidia-vaapi-driver dev shell"
    echo "  meson setup build && meson compile -C build"
    echo "  LIBVA_DRIVERS_PATH=\$PWD/build vainfo --display drm --device /dev/dri/renderD128"
    echo "  meson test -C build            # needs a working NVIDIA render node"
  '';
}
