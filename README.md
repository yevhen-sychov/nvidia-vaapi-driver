# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a decode backend and NVENC as an encode backend. The decode path is specifically designed to be used by Firefox for accelerated decode of web content, and may not operate correctly in other applications. This fork additionally implements hardware **video encoding** (H.264, HEVC and AV1) through NVENC.

The NVENC encode work in this fork is directly built on top of
[@efortin](https://github.com/efortin)'s upstream PR
[elFarto/nvidia-vaapi-driver#427 — *feat(core): Add NVENC Encoding Support via VA-API*](https://github.com/elFarto/nvidia-vaapi-driver/pull/427).
That PR contributes the entire foundation — the `VAEntrypointEncSlice` wiring,
the 32-bit → 64-bit shared-memory bridge / `nvenc-helper` daemon, `vaDeriveImage`,
the encode/encode-config/IPC-fuzz test harnesses, and the H.264 + HEVC encoders
including the Steam Remote Play integration path. Everything below labelled as
"this fork" is a downstream on top of that work. See
[Credits](#credits--upstream) for the full attribution.

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
  - [Test suite](#test-suite)
  - [Sample media](#sample-media)
- [Development workflow](#development-workflow)
- [Credits & upstream](#credits--upstream)
- [Fork changelog](#fork-changelog)

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

### Live rate-control / framerate updates (WebRTC BWE)

Mid-session `VAEncMiscParameterTypeRateControl` and `VAEncMiscParameterTypeFrameRate`
buffers — which is how Chrome's WebRTC bandwidth estimator (BWE) throttles the
encoder up and down as network conditions change, and how camera-rate switches
are propagated — are applied to the running NVENC session via
`nvEncReconfigureEncoder`. Without this the encoder silently kept emitting at
its initial bitrate and framerate, saturated the uplink on BWE reductions and
caused frozen frames on the peer. Test coverage lives in
`tests/test_encode.c` (`test_bitrate_reconfigure_mid_session`,
`test_bitrate_reconfigure_ramp_down_and_up`,
`test_framerate_reconfigure_mid_session`).

### Long-running session stability

The per-frame CUDA staging buffer and NVENC-registered input resource are now
persistent across frames for the lifetime of the encode session — allocated on
the first frame at the session's dimensions and reused, re-registered only when
input dimensions actually change. Repeatedly allocating device memory and
registering/unregistering NVENC resources every frame was found to gradually
destabilize long-running encode sessions (hard crash after a few minutes on
sustained WebRTC calls). Regression is guarded by
`test_long_running_single_session` in `tests/test_encode.c`.

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
| `NVD_MAX_DETACHED_BACKING_IMAGE_BYTES` | Upper bound (in bytes) on the size of the detached backing-image cache used by the direct backend to recycle decode surfaces across stream switches. Lower this on low-VRAM GPUs to reduce memory usage at the cost of more re-allocation when streams change. Set to `0` to disable detached caching. Default: scales with the GPU — total VRAM / 64 (~1.6%), clamped to 64 MiB–512 MiB; falls back to `134217728` (128 MiB) if the VRAM size cannot be queried. |
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

## Test suite

An in-tree test suite exercises both the decode and encode paths against a real
NVIDIA GPU and the installed VA-API loader. It requires the driver to be built
against a working CUDA/NVENC stack and, for the ffmpeg smoke test, a working
`ffmpeg` binary on `PATH`.

```sh
meson setup build
meson test -C build
```

Individual harnesses:

| Binary / script | What it covers |
|---|---|
| `test_decode` | AV1 (8-bit + 10-bit) decode init, combined-RTFormat resolution, DMA-BUF export, auto descriptor mode / self-preview heuristic. |
| `test_encode` | Encode entrypoints, config attributes, single-frame encode for H.264 / HEVC / HEVC Main10 / AV1 / AV1 Main10, rate control + quality-level params, AV1 temporal SVC and combined-RTFormat encode, dynamic resolution, sequential encodes, coded-buffer reuse, long-running single session, live bitrate/framerate reconfigure, auto-combined encode export, decode-still-works co-existence, dimension-mismatch, H.264 B-frames. |
| `test_encode_config` | Config-side coverage: entrypoints, RTFormat, rate control, packed headers, ref frames, max dimensions, quality range, surface allocation (NV12 / P010 / small / 4K), export descriptor. |
| `test_ipc_fuzz` | Fuzz surface for the NVENC out-of-process IPC helper (invalid commands, truncated inits, oversized payloads, rapid connect/disconnect, double-init, encode-without-init). |
| `tests/test_ffmpeg.sh` | End-to-end ffmpeg + VA-API smoke test. Defaults to `samples/smptebars_h264.mp4` (produced by `samples/gensamples.sh`); override with a positional path argument. |
| `tests/test_gstreamer.sh` | End-to-end GStreamer VA-API smoke test. |

Every code change to this fork lands with a test — see [Development workflow](#development-workflow) below.

## Sample media

Test media lives under `samples/` and is **not tracked in git** (see
`.gitignore` — `samples/*.mp4`). Generate the synthetic fixtures used by the
ffmpeg smoke test with:

```sh
./samples/gensamples.sh
```

The script writes SMPTE-bars test clips (H.264, HEVC 8/10/12-bit, HEVC 4:2:2
8/10/12-bit, MPEG-4, VP9, AV1) into the `samples/` directory regardless of
the current working directory. `tests/test_ffmpeg.sh` and the meson `ffmpeg`
test default to `samples/smptebars_h264.mp4` — the first file produced by
`gensamples.sh` — so a partial run of the generator is enough to exercise
the smoke test. Any other container/codec supported by your ffmpeg build
works too; pass it as a positional argument to `test_ffmpeg.sh`.

# Development workflow

This fork carries a substantial delta from upstream (see
[Fork changelog](#fork-changelog)). Two rules keep that delta maintainable:

1. **Tests come with every code change.** New code paths get new coverage in
   `tests/`, changed code paths get their existing test re-exercised. Prefer
   extending an existing `test_*` function to adding a whole new one when the
   change fits. Truly untestable changes (e.g. purely defensive tightening in a
   path we cannot drive from the harness) must be called out explicitly rather
   than merged bare.
2. **README tracks user-visible surface.** Anything a downstream user might
   configure, set as an env var, see in `NVD_LOG=1`, or trip over at runtime
   goes into this file in the same task as the code change. Internal refactors
   don't need README updates; behavior changes always do.

The [Fork changelog](#fork-changelog) below is the running record of what this
fork has added on top of upstream — update it when you land a notable change.

# Credits & upstream

This fork stands on two upstream shoulders and should be read as a downstream
patch series on top of them:

- **[elFarto/nvidia-vaapi-driver](https://github.com/elFarto/nvidia-vaapi-driver)**
  — the original VA-API implementation over NVDEC (decode). Everything under
  the decode entrypoints, the EGL + direct backends, the DMA-BUF export
  plumbing, and the Firefox integration story comes from there.

- **[@efortin](https://github.com/efortin)**, via upstream PR
  [elFarto/nvidia-vaapi-driver#427](https://github.com/elFarto/nvidia-vaapi-driver/pull/427)
  — ***feat(core): Add NVENC Encoding Support via VA-API***. This is where
  hardware encoding in this driver comes from. The PR (49 commits, +5,362
  −64 across 21 files, opened April 2026, branch `efortin:feat/nvenc-support`
  — the very branch name this fork carries) contributes:
    - `VAEntrypointEncSlice` end-to-end for H.264 (Constrained Baseline /
      Main / High) and HEVC (Main + Main10).
    - The 32-bit → 64-bit shared-memory bridge, `nvenc-helper` daemon,
      `nvenc-helper.service` systemd unit, and the Unix-socket + memfd IPC
      protocol — the mechanism that unlocks Steam Remote Play on Blackwell
      (RTX 50xx) where NVIDIA dropped 32-bit CUDA support.
    - `vaDeriveImage` for zero-copy capture, DRM-backed surface allocation
      without CUDA, NV12 pitch/height alignment for MB-aligned encoders,
      periodic IDR + client-triggered IDR forwarding, dead-client detection
      via `poll()` timeout, NVIDIA opaque-fd vs DMA-BUF-fd handling.
    - The initial test-suite scaffolding — `test_encode`, `test_encode_config`,
      `test_ipc_fuzz`, `test_gstreamer` — including the IPC fuzz-safety cases,
      ASAN/UBSAN sweep, and 71-test acceptance gate.
    - Steam Remote Play validation (Mac Steam Link, Legion Go) that
      demonstrates the encode path is real-world usable end-to-end.

  If you file issues about NVENC encode behavior that trace back to those
  foundations, credit belongs upstream on PR #427; if it traces back to the
  additions listed under [Fork changelog](#fork-changelog), that's on this
  fork.

- **Everyone else** whose PRs against elFarto/nvidia-vaapi-driver we merge in
  from upstream master. See `git log --author=... --oneline` for individual
  attribution.

# Fork changelog

Highlights of what `feat/nvenc-support` in *this* fork adds on top of the
efortin PR #427 base and elFarto's upstream master. See
`git log --oneline main..HEAD` for the exhaustive list.

- **NVENC encode entrypoint** — full VA-API `VAEntrypointEncSlice` for H.264
  (Constrained Baseline / Main / High), HEVC (Main, Main10) and AV1 (Profile0).
  Exposed via `VAConfigAttribRateControl`, packed headers, quality level, ref
  frames, max dimensions, and (AV1) temporal SVC.
- **NVENC out-of-process helper** — `nvenc-helper.service` (user systemd unit)
  + `/usr/libexec/nvenc-helper` binary + IPC channel for sandboxed browser
  processes that can't init CUDA/NVENC directly. Rebuild + restart with
  `update-nvenc.sh`.
- **Chrome-compatible DMA-BUF descriptor mode** — `NVD_DESCRIPTOR_MODE` env
  var (default `auto`), picks per-surface layout for encode-context vs.
  decode-context surfaces, with a resolution-matching heuristic that also
  routes a decode surface through the combined-fourcc path when it's a
  self-preview of a currently active local encode.
- **Live rate-control / framerate reconfigure** — mid-session
  `VAEncMiscParameterTypeRateControl` /
  `VAEncMiscParameterTypeFrameRate` are applied through
  `nvEncReconfigureEncoder`, so WebRTC BWE actually throttles the hardware
  encoder up and down instead of being silently ignored.
- **Long-running session stability** — persistent linear staging buffer +
  NVENC-registered input resource per encode session (no per-frame alloc /
  registration churn), fixing hard crashes ~a few minutes into sustained
  WebRTC calls.
- **Resource-release + detach-race + decoder-init hardening** — ported from
  upstream master onto the fork's paths: `direct-export-buf.c` cleanup on the
  bail path uses the real `destroyBackingImage` helper and takes the images
  mutex around detach; `vabackend.c` guards resolve-thread teardown with a
  `resolveThreadStarted` flag to avoid a UAF window; `CUVIDDECODECREATEINFO`
  is no longer initialized with a self-referential expression (previously
  undefined behavior surfaced as spurious CUDA OOM).
- **Encoder test suite** — `tests/test_encode.c` +
  `tests/test_encode_config.c` +  `tests/test_ipc_fuzz.c` cover the surface
  above, including the reconfigure fix (see the three
  `test_*_reconfigure_*` cases).
- **Statistics subsystem picked back up from upstream** — during the
  earlier upstream merge (`51654ee`), upstream's own split of the
  `NVD_STATS` subsystem into `src/stats.{c,h}` (elFarto commit
  `609c6ce`, cherry `eb102da`) was silently dropped by an `--ours`
  resolution, leaving the inline stats copy in `vabackend.c`. Cherry-
  picked upstream's split back in: `NVStatCounter` enum, `nvStatsInit`,
  `nvStatsIncrement`, `nvStatsLog` now live in `src/stats.c`; the tiny
  `nv_gettid` / `nvStatsOutput` helpers stay in `vabackend.c` as
  non-static and are re-exported through `vabackend.h`. Kept the
  fork's `NVD_DESCRIPTOR_MODE` parsing block right next to the new
  `nvStatsInit(drv)` call. Same rationale as the encode-dispatch split
  above: reduce the delta this fork carries in `vabackend.c` so
  upstream merges stop churning it.
- **Encode-dispatch split into `src/nvenc_dispatch.c`** — the
  encode-side entry points that used to live inline in
  `src/vabackend.c` now live in a dedicated translation unit called
  through prototypes in `src/nvenc.h`:
  `nvGetConfigAttributesEncode`, `nvRenderPictureEncode`,
  `nvEndPictureEncode`, `nvEndPictureEncodeIPC`,
  `nvenc_dispatch_create_config`,
  `nvenc_dispatch_query_config_attributes`,
  `nvenc_dispatch_create_context`,
  `nvenc_dispatch_destroy_context`. The shared object-id lookup and
  allocation helpers are exported as `nvGetObjectPtr` and
  `nvAllocateObject` in `src/vabackend.h`. This shrinks
  `vabackend.c`'s diff-versus-upstream by ~1000 lines and moves that
  surface into a file upstream never touches, so future upstream
  merges only conflict on the ~10 remaining thin
  `if (isEncode) return nvenc_dispatch_*(...)` call-sites in
  `vabackend.c` — not on the encode implementation itself. Also
  covered: `nvenc_dispatch_begin_picture` (per-frame render-target
  reset), and the two IPC-encode host-memory paths
  (`nvenc_dispatch_derive_image_hostmem`,
  `nvenc_dispatch_put_image_hostmem`) that back Steam's
  `vaDeriveImage`/`vaMapBuffer` capture-write path when CUDA is
  unavailable.
- **Samples relocation + generator hardening** — the ffmpeg smoke-test
  input moved from `tests/input.mp4` to `samples/` (untracked). The
  `samples/gensamples.sh` fixture generator now writes into its own
  directory instead of `$PWD` (previously polluted the repo root when
  invoked as `./samples/gensamples.sh`), passes `-y` so re-runs don't
  hang on overwrite prompts, and the default input for `test_ffmpeg.sh`
  is now `samples/smptebars_h264.mp4` — the first fixture the generator
  produces.
