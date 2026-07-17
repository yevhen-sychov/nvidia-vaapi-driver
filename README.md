# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a decode backend and NVENC as an encode backend. The decode path is specifically designed to be used by Firefox for accelerated decode of web content, and may not operate correctly in other applications. This fork additionally implements hardware **video encoding** (H.264, HEVC and AV1) through NVENC.

# Table of contents

- [nvidia-vaapi-driver](#nvidia-vaapi-driver)
- [Table of contents](#table-of-contents)
- [Codec Support](#codec-support)
  - [Decode Support](#decode-support)
  - [Encode Support](#encode-support)
- [Installation](#installation)
  - [Quick install from this fork](#quick-install-from-this-fork)
  - [Packaging status](#packaging-status)
  - [Building](#building)
  - [Removal](#removal)
- [Configuration](#configuration)
  - [Upstream regressions](#upstream-regressions)
  - [Kernel parameters](#kernel-parameters)
  - [Environment Variables](#environment-variables)
  - [Firefox](#firefox)
  - [Chrome](#chrome)
  - [MPV](#mpv)
  - [NVENC encode helper](#nvenc-encode-helper)
  - [Direct Backend](#direct-backend)
- [Testing](#testing)

# Codec Support

This fork supports both hardware **decoding** (NVDEC) and hardware **encoding** (NVENC).

## Decode Support

| Codec | Supported | Comments |
|---|---|---|
|AV1|:heavy_check_mark:|Firefox 98+ is required.|
|H.264|:heavy_check_mark:||
|HEVC|:heavy_check_mark:|Some distros are shipping Firefox and/or FFMPEG with HEVC support disabled due to patent concerns.|
|VP8|:heavy_check_mark:||
|VP9|:heavy_check_mark:|Requires being compiled with `gstreamer-codecparsers-1.0`|
|MPEG-2|:heavy_check_mark:||
|VC-1|:heavy_check_mark:||
|MPEG-4|:x:|VA-API does not supply enough of the original bitstream to allow NVDEC to decode it.|
|JPEG|:x:|This is unlikely to ever work, the two APIs are too different.|

YUV444 is supported but requires:

* \>= Turing (20XX/16XX)
* HEVC
* Direct backend

To view which codecs your card is capable of decoding you can use the `vainfo` command with this driver installed, or visit the NVIDIA website [here](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new#geforce).

## Encode Support

Hardware encoding is exposed through the VA-API `VAEntrypointEncSlice` entrypoint and is backed by NVENC. It becomes available when a usable NVENC engine is detected (either directly or via the [NVENC encode helper](#nvenc-encode-helper)).

| Codec | Supported | Profiles | Comments |
|---|---|---|---|
|H.264|:heavy_check_mark:|Constrained Baseline, Main, High||
|HEVC|:heavy_check_mark:|Main, Main10|Main10 enables 10-bit encoding.|
|AV1|:heavy_check_mark:|Profile0|Requires an NVENC engine with AV1 encode support (Ada/Lovelace 40XX or newer).|
|VP8 / VP9|:x:||Not supported by NVENC.|

Actual encode capabilities depend on your GPU's NVENC generation. To view which codecs your card is capable of encoding you can use the `vainfo` command with this driver installed, or visit the NVIDIA [encode/decode support matrix](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new#geforce).

# Installation

To install and use `nvidia-vaapi-driver`, follow the steps in installation and configuration. It is recommended to follow testing as well to verify hardware acceleration is working as intended.

**Requirements**

* NVIDIA driver series 470 or 500+

## Quick install from this fork

This fork's `main` branch is intended to match the locally tested AoTofu driver build. If the repository is private, clone it with a GitHub account that has access:

```sh
git clone git@github.com:AoTofu/nvidia-vaapi-driver.git
cd nvidia-vaapi-driver
./install.sh --deps --clean
```

The installer builds the driver, backs up any existing `nvidia_drv_video.so`, installs the new driver into libva's driver directory, and runs a `vainfo` smoke test when possible. To skip dependency installation:

```sh
./install.sh --clean
```

The installer prints a rollback command if it replaced an existing driver.

## Packaging status

<p align="top"><a href="https://repology.org/project/nvidia-vaapi-driver/versions"><img src="https://repology.org/badge/vertical-allrepos/nvidia-vaapi-driver.svg" alt="repology"><a href="https://repology.org/project/libva-nvidia-driver/versions"><img src="https://repology.org/badge/vertical-allrepos/libva-nvidia-driver.svg" alt="repology" align="top" width="%"></p>

[pkgs.org/nvidia-vaapi-driver](https://pkgs.org/search/?q=nvidia-vaapi-driver) [pkgs.org/libva-nvidia-driver](https://pkgs.org/search/?q=libva-nvidia-driver)

openSUSE: [1](https://software.opensuse.org/package/nvidia-vaapi-driver), [2](https://software.opensuse.org/package/libva-nvidia-driver).

Feel free to add your distributions package in an issue/PR, if it isn't on these websites.

## Building

You'll need `meson`, the `gstreamer-plugins-bad` library, and [`nv-codec-headers`](https://git.videolan.org/?p=ffmpeg/nv-codec-headers.git) installed.

| Package manager | Packages                                        | Optional packages for additional codec support |
|-----------------|-------------------------------------------------|------------------------------------------------|
| pacman          | meson gst-plugins-bad ffnvcodec-headers         |                                                |
| apt             | meson gstreamer1.0-plugins-bad libffmpeg-nvenc-dev libva-dev libegl-dev libdrm-dev | libgstreamer-plugins-bad1.0-dev   |
| yum/dnf         | meson libva-devel gstreamer1-plugins-bad-freeworld nv-codec-headers libdrm-devel | gstreamer1-plugins-bad-free-devel |

Then run the following commands:

```sh
meson setup build
meson install -C build
```

## Removal

By default the driver installs itself as `/usr/lib64/dri/nvidia_drv_video.so` (this might be `/usr/lib/x86_64-linux-gnu/dri/nvidia_drv_video.so` on some distros). To uninstall the driver, simply remove this file. In addition, this file is usually symlinked to `/usr/lib64/dri/vdpau_drv_video.so` (or `/usr/lib/x86_64-linux-gnu/dri/vdpau_drv_video.so`) if the VDPAU to VA-API driver is installed, so this symlink will need to be restored for that driver to work normally again.

# Configuration

## Upstream regressions

The EGL backend is broken on driver versions 525 or later due to a regression. Users running these drivers should use the [direct backend](#direct-backend) instead.

For more information read the [upstream bug report](https://forums.developer.nvidia.com/t/cueglstreamproducerconnect-returns-error-801-on-525-53-driver/233610) or [issue #126](/../../issues/126).

## Kernel parameters

This library requires that the `nvidia_drm` kernel module is [configured with the parameter](https://wiki.archlinux.org/title/Kernel_parameters) `nvidia-drm.modeset=1`

## Environment Variables

Environment variables used to control the behavior of this library.

| Variable | Purpose |
|---|---|
| `NVD_LOG` | Used to control logging. `1` to log to stdout, anything else to append to the given file. |
| `NVD_MAX_INSTANCES` | Controls the maximum concurrent instances of the driver will be allowed per-process. This option is only really useful for older GPUs with not much VRAM, especially with Firefox on video heavy websites. |
| `NVD_BACKEND` | Controls which backend this library uses. Either `egl`, or `direct` (default). See [direct backend](#direct-backend) for more details. |
| `NVD_MAX_DETACHED_BACKING_IMAGE_BYTES` | Upper bound (in bytes) on the size of the detached backing-image cache used by the direct backend to recycle decode surfaces across stream switches. Lower this on low-VRAM GPUs to reduce memory usage at the cost of more re-allocation when streams change. Set to `0` to disable detached caching. Default: `134217728` (128 MiB). |
| `NVD_MAX_DETACHED_BACKING_IMAGES` | Upper bound on the number of cached detached backing images. Set to `0` to disable detached caching. Default: `16`. |

## Firefox

Due to license, Firefox on Linux does not support HEVC till now.
To use the driver with firefox you will need at least Firefox 96, `ffmpeg` compiled with vaapi support (`ffmpeg -hwaccels` output should include vaapi), and the following config options need to be set in the `about:config` page:

| Option | Value | Reason |
|---|---|---|
| media.ffmpeg.vaapi.enabled | true | Required until Firefox 137, enables the use of VA-API. |
| media.hardware-video-decoding.force-enabled | true | Required since Firefox 137, enables hardware acceleration. |
| media.rdd-ffmpeg.enabled | true | Required, default on FF97. Forces ffmpeg usage into the RDD process, rather than the content process. |
| media.av1.enabled | false | Optional, disables AV1. If your GPU doesn't support AV1, this will prevent sites using it and falling back to software decoding. |
| gfx.x11-egl.force-enabled | true | Required, this driver requires that Firefox use the EGL backend. It may be enabled by default. It is recommended to test it with the `MOZ_X11_EGL=1` environment variable before enabling it in the Firefox configuration. |
| widget.dmabuf.force-enabled | true | Required on NVIDIA 470 series drivers. Note that Firefox isn't coded to allow DMA-BUF support without GBM support, so it may not function completely correctly when it's forced on. |

In addition the following environment variables need to be set. For permanent configuration `/etc/environment` may suffice.

| Variable | Value | Reason |
|---|---|---|
| MOZ_DISABLE_RDD_SANDBOX | 1 | Disables the sandbox for the RDD process that the decoder runs in. |
| LIBVA_DRIVER_NAME | nvidia | Required for libva 2.20+, forces libva to load this driver. |
| __EGL_VENDOR_LIBRARY_FILENAMES | /usr/share/glvnd/egl_vendor.d/10_nvidia.json | Required for the 470 driver series only. It overrides the list of drivers the glvnd library can use to prevent Firefox from using the MESA driver by mistake. |
| CUDA_DISABLE_PERF_BOOST | 1 | Optional. Requires NVIDIA driver >= 580.105.08. Disables the forced power boost the GPU gets when CUDA is activated. This should reduce the power usage when decoding video. This setting is the equivilent of the 'CUDA Force P2' NVIDIA Profile Inspector setting on Windows. |

When libva is used it will log out some information, which can be excessive when Firefox initalises it multiple times per page. This logging can be suppressed by adding the following line to the `/etc/libva.conf` file:
```
LIBVA_MESSAGING_LEVEL=1
```

If you're using the Snap version of Firefox, it will be unable to access the host version of the driver that is installed.

## Chrome

This fork includes the Chromium-compatible single-buffer export path. For Chrome / Chromium based browsers, set `LIBVA_DRIVER_NAME=nvidia` and start the browser with flags similar to:

```sh
LIBVA_DRIVER_NAME=nvidia google-chrome \
  --enable-features=AcceleratedVideoDecodeLinuxGL,AcceleratedVideoEncodeLinuxGL,VaapiOnNvidiaGPUs,VaapiVideoEncoder,VaapiIgnoreDriverChecks \
  --ignore-gpu-blocklist \
  --use-gl=angle --use-angle=gl
```

To use hardware AV1 encoding you must pass the encode-related features
(`AcceleratedVideoEncodeLinuxGL` / `VaapiVideoEncoder`) in addition to the decode
ones — without them Chrome never probes the VA-API encoder and silently falls back
to software `libaom`.

On Wayland, also try `--ozone-platform=wayland` or `--ozone-platform-hint=auto`.

### WebRTC temporal scalability (screenshare)

WebRTC screenshare uses temporal SVC (e.g. `scalabilityMode=L1T2`). This driver
advertises temporal-layer support for AV1 (`VAConfigAttribEncRateControlExt`) and
programs NVENC's temporal SVC from the layer structure supplied by the browser, so
Chrome will use the hardware AV1 encoder instead of falling back to software. If you
still see `encoderImplementation` reporting `libaom` in `chrome://webrtc-internals`,
disable Chrome's software-fallback with `--disable-features=WebRtcAllowsSvcHardwareFallback`
to surface the real encoder-selection result.

> **Note:** if AV1 *decode* also regresses to software after enabling the encode
> features, make sure you are running a driver build that resolves an ambiguous
> `VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10` request to 8-bit. An earlier
> build treated that combined mask as a 10-bit request, so an 8-bit AV1 encode
> aborted mid-frame; that hard NVENC failure can crash the browser GPU process
> and drop *all* hardware video (decode included) to software. Rebuild/reinstall
> the driver (e.g. `update-nvenc.sh`) and restart the browser.

### `NVD_DESCRIPTOR_MODE` (auto by default)

By default (`NVD_DESCRIPTOR_MODE` unset, or explicitly `auto`) the driver picks
the DMA-BUF export layout **per surface**, automatically, based on whether the
surface belongs to an encode context or a decode context:

- Encode-context surfaces (local screen/camera capture that Chrome's compositor
  renders *into* via its WebGL/canvas "video-processing" worker path before
  it's handed to NVENC) are exported as a single combined-fourcc layer (e.g.
  NV12 with 2 planes).
- Decode-context surfaces (the normal remote/received-video display path) are
  exported as one split single-channel layer per plane (`R8`/`GR88`), which is
  what Chromium's decode-display zero-copy importer expects.
- A decode-context surface whose resolution exactly matches a currently-active
  local encode context's resolution is also exported with the combined layer.
  This covers Chrome re-decoding its own just-encoded stream to render a local
  self-preview/thumbnail, which goes through the same WebGL/canvas worker
  importer as an encode surface even though it is technically a decode
  context — a genuine remote peer's video is essentially never encoded at the
  exact same pixel dimensions as your own outgoing capture, so this heuristic
  does not affect real inbound video.

This matters because Chrome's two consumers of an exported DMA-BUF want
different layouts on the same GPU/driver/ANGLE combination:

> **EGL_BAD_MATCH / "requested LINUX_DRM_FORMAT is not supported":** if Chrome's
> log is full of `eglCreateImageKHR: EGL_BAD_MATCH` errors together with
> `OzoneImageBacking::ProduceSkiaGanesh failed to create GL representation` and
> `CopySharedImage: unknown mailbox` (typically from a `RendererBlinkWorker`
> raster/WebGL/canvas import, not the normal video display path), that's ANGLE's
> NVIDIA DMA-BUF importer on that worker path rejecting the split per-plane
> layout and only accepting the *combined* fourcc. The `auto` default handles
> this for you on encode-context surfaces.

If you need to force one layout for *every* surface (e.g. to test the
traditional per-plane behavior, or because the automatic per-surface decision
doesn't cover your specific workflow — see the caveat below), set
`NVD_DESCRIPTOR_MODE` explicitly to `single` (split layer, single DMA-BUF
object), `multi` (split layer, one DMA-BUF object per plane), or `combined`
(single combined-fourcc layer, for every surface regardless of encode/decode).
Check `NVD_LOG=1` for the `Descriptor mode: ...` line to confirm which mode is
active.

> **Known caveat:** the automatic decision is based on VA-API context type
> (encode vs. decode) plus the resolution-matching heuristic above, which are
> the only signals the driver has access to — there is no VA-API flag that
> says "this decode is a local self-preview". In the unlikely case a decode
> surface's resolution *happens* to coincide with an unrelated remote peer's
> resolution, or a self-preview is rendered at a resolution that doesn't
> exactly match the local encode context, this can still misclassify a
> surface. If you find a specific decode surface still needs the combined
> layout (or the opposite), forcing `NVD_DESCRIPTOR_MODE=combined`/`single`/
> `multi` remains available as a manual override, at the cost of that mode's
> known trade-off for the other surface type.

### Encoder restart delay after stopping/switching screenshare

If you stop sharing a screen (or switch the active shared window/source) and
the outgoing video appears frozen on one frame for a short period afterwards,
this is expected and not a driver issue. Chrome tears down the old encode
context and DMA-BUF surfaces (visible in `NVD_LOG=1` as `nvDestroyContext` /
`nvenc_close_session` / `nvDestroySurfaces`) and has to negotiate/create a new
encode context for the new source before frames start flowing again; this
teardown/recreate cycle takes Chrome some time on its own. Similarly, a
`SharedImageManager::ProduceSkia: ... non-existent mailbox` message logged a
while after such a stop/switch (with no adjacent driver `EGL_BAD_MATCH` or
`Exporting surface descriptor` line) is Chrome's own GPU-process mailbox
bookkeeping settling after that teardown, not a surface/format problem from
this driver.

## MPV

Currently this only works with a recent MPV version (at least 0.36.0).

There's no real reason to run it with mpv except for testing, as mpv already supports using nvdec directly. The `test.sh` script will run mpv with the file provided and various environment variables set to use the newly built driver

## NVENC encode helper

Hardware encoding uses NVENC. When the driver can initialise CUDA/NVENC in-process, encoding works directly with no extra setup. In environments where the process that loads the driver cannot talk to NVENC directly (for example sandboxed browser processes), the driver falls back to an out-of-process helper that performs the encode over an IPC channel.

The helper is shipped as a user systemd service (`nvenc-helper.service`) and is installed to `/usr/libexec/nvenc-helper`. Enable and start it with:

```sh
systemctl --user enable --now nvenc-helper.service
```

After rebuilding the driver you can update and restart the helper via the provided `update-nvenc.sh` script (it builds the 64-bit and 32-bit driver, installs them, and restarts the helper service).

The helper honours the `NVENC_HELPER_IDR_INTERVAL` environment variable to control the IDR/keyframe interval.

## Direct Backend

The direct backend is a experimental backend that accesses the NVIDIA kernel driver directly, rather than using EGL to share the buffers. This allows us
a greater degree of control over buffer allocation and freeing.

The direct backend has been tested on a variety of hardware from the Kepler to Lovelace generations, and seems to be working fine. If you find any compatibility issues, please leave a comment [here](/../../issues/126).

Given this backend accesses the NVIDIA driver directly, via NVIDIA's unstable API, this module is likely to break often with new versions of the kernel driver. If you encounter issues using this backend raise an issue and including logs generated by `NVD_LOG=1`.

This backend uses headers files from the NVIDIA [open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
project. The `extract_headers.sh` script, along with the `headers.in` file list which files we need, and will copy them from a checked out version of the NVIDIA project to the `nvidia-include` directory. This is done to prevent everyone needing to checkout that project.

# Testing

To verify that the driver is being used to decode video, you can use nvidia-settings or nvidia-smi.

- nvidia-settings

  By selecting the relevant GPU on the left of the nvidia-settings window, it will show `Video Engine Utilization` on the right. While playing a video this value should be non-zero.

- nvidia-smi

  Running `nvidia-smi` while decoding a video should show a Firefox process with `C` in the `Type` column. In addition `nvidia-smi pmon` will show the usage of the decode engine per-process, and `nvidia-smi dmon` will show the usage per-GPU. When using nvidia open gpu kernel modules, the usage of the decode engine may not be displayed correctly.
