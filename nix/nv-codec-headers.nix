{
  lib,
  stdenvNoCC,
  fetchFromGitHub,
  # Keep this in step with the NVENC API your installed NVIDIA driver supports:
  # headers newer than the driver make nvEncOpenEncodeSessionEx fail with
  # NV_ENC_ERR_INVALID_VERSION at runtime. 13.0.19.x is what current distro
  # packages (Fedora's nv-codec-headers) ship.
  version ? "13.0.19.1",
  hash ? "sha256-GGHR5h7vJ0Z7QnrSP/Yyg3DDoVrmwv4RMVYGrlSRnNk=",
}:

# nixpkgs only packages up to nv-codec-headers 12.1, which predates
# NV_ENC_BUFFER_FORMAT_P210 and the rest of the NVENC 13 API this driver uses.
stdenvNoCC.mkDerivation (finalAttrs: {
  pname = "nv-codec-headers";
  inherit version;

  src = fetchFromGitHub {
    owner = "FFmpeg";
    repo = "nv-codec-headers";
    tag = "n${finalAttrs.version}";
    inherit hash;
  };

  makeFlags = [ "PREFIX=$(out)" ];

  meta = {
    description = "FFmpeg version of headers for NVENC/NVDEC - ${version}";
    homepage = "https://git.videolan.org/?p=ffmpeg/nv-codec-headers.git";
    license = lib.licenses.mit;
    platforms = lib.platforms.all;
  };
})
