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
    - [VP8 bitstream reconstruction](#vp8-bitstream-reconstruction)
    - [Picture-index recycling (upstream #397)](#picture-index-recycling-upstream-397)
    - [Surface / context destruction order](#surface--context-destruction-order)
  - [Encode Support](#encode-support)
    - [Packed headers](#packed-headers)
    - [Encode logging](#encode-logging)
    - [Rate control](#rate-control)
    - [Format-plumbing status for the newer profiles](#format-plumbing-status-for-the-newer-profiles)
    - [Live rate-control / framerate updates (WebRTC BWE)](#live-rate-control--framerate-updates-webrtc-bwe)
    - [Long-running session stability](#long-running-session-stability)
    - [AV1 VUI defaults (WebRTC-tuned)](#av1-vui-defaults-webrtc-tuned)
    - [Client-driven IDR (no NVENC auto-refresh)](#client-driven-idr-no-nvenc-auto-refresh)
    - [Known limitations](#known-limitations)
  - [Video post-processing (`VAEntrypointVideoProc`)](#video-post-processing-vaentrypointvideoproc)
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
    - [What Firefox actually requires from the driver](#what-firefox-actually-requires-from-the-driver)
  - [Chrome](#chrome)
    - [What Chromium actually requires from the driver](#what-chromium-actually-requires-from-the-driver)
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
|VP8|:heavy_check_mark:|Fixed in this fork — see [VP8 bitstream reconstruction](#vp8-bitstream-reconstruction). Upstream decodes VP8 to garbage.|
|VP9|:heavy_check_mark:|Requires being compiled with `gstreamer-codecparsers-1.0`|
|MPEG-2|:heavy_check_mark:||
|VC-1|:heavy_check_mark:||
|MPEG-4|:x:|VA-API does not supply enough of the original bitstream to allow NVDEC to decode it.|
|JPEG|:x:|This is unlikely to ever work, the two APIs are too different.|

YUV444 is supported but requires:

* \>= Turing (20XX/16XX)
* HEVC
* Direct backend

### VP8 bitstream reconstruction

VP8 is the one decode path where VA-API does not hand the driver everything
NVDEC needs. The slice data buffer starts at the first partition, *after* the
VP8 "uncompressed data chunk" (RFC 6386 §9.1) — the 3-byte frame tag, plus a
3-byte sync code and the coded dimensions on a keyframe — but NVDEC still
expects that chunk at the head of the bitstream it is given.

Upstream recovered it by rewinding the client's slice-data pointer to the
previous 16-byte boundary (`ptr & 0xf`) and using whatever bytes were there.
That reads behind a buffer the driver does not own, and the rewind distance
comes from *pointer alignment* rather than from the format, so it only lands on
the real header by luck. In practice it did not: with FFmpeg the rewind
overshot the 10-byte keyframe chunk by 4 bytes, the sync-code check then failed,
and the fallback prepended ten zero bytes instead — so **every frame decoded to
garbage**, measured at ~8.7 dB PSNR against libvpx.

This fork reconstructs the chunk from the VA-API parameters it is actually
given: `frame_type`, `version` and the dimensions from
`VAPictureParameterBufferVP8`, and `first_part_size` from
`VASliceParameterBufferVP8` (`partition_size[0] + ceil(macroblock_offset / 8)`,
which reproduces the real frame tag's field exactly). No out-of-bounds read is
involved and the result no longer depends on how the client allocated its
buffers — which is also what made this fail in Chromium but not (visibly) in
some other clients. VP8 hardware decode is now **bit-exact** with libvpx, pinned
by `tests/test_vp8_decode.sh`.

The one field VA-API does not carry is `show_frame`; it is set to 1, matching
the value the driver already reports to NVDEC through
`CUVIDPICPARAMS.CodecSpecific.vp8.vp8_frame_tag`.

### Picture-index recycling (upstream #397)

Every surface a context decodes into is assigned a picture index, which is how
NVDEC addresses its decode surface array. Upstream took that index from a
counter that only ever incremented and was never released when a surface was
destroyed — so a context could service at most `surfaceCount` **distinct
surfaces over its entire lifetime**, not that many concurrently. Past that,
every `vaBeginPicture` returned `VA_STATUS_ERROR_MAX_NUM_EXCEEDED`
("list argument exceeds maximum number").

FFmpeg never hits this: it passes its whole surface pool as render targets up
front and then reuses it. Chromium calls `vaCreateContext` with **zero** render
targets — so the limit defaults to 32 — and then allocates surfaces on demand
from a `DmabufVideoFramePool` that churns them across resolution changes and
pool resizes. That is why the failure was reported as Chromium-only, and why it
showed up sooner at higher resolutions.

Indices are now a per-context pool: the lowest free slot is claimed on first
use and returned when the surface is destroyed or moves to another context.
Pinned by the `Picture indices are recycled across surface churn` case in
`tests/test_decode.c`, which pushes 96 surfaces through a single context.

### Surface / context destruction order

A surface keeps a raw back-pointer to the context it was last used on, and
VA-API does not require a client to destroy its surfaces before the context.
`vaDestroyContext` used to free the context and leave those back-pointers
dangling, so anything reading `surface->context` afterwards — `vaSyncSurface`
checking whether it is an encode context, `vaGetImage`, the VideoProc blit,
picture-index release on `vaDestroySurfaces` — was touching freed memory. It
failed intermittently, depending on whether the allocator had reused the block,
which is exactly the kind of fault that only surfaces under load.

`vaDestroyContext` now clears the back-pointer on every surface that referenced
it, so "the context is gone" is representable as `NULL` instead of as a stale
pointer. It also releases each of those surfaces' resolve flags: the resolve
thread has already been joined by that point, so anything still queued for it
would never complete, and a later `vaSyncSurface` or `vaExportSurfaceHandle`
would block forever waiting for a resolution that can no longer happen.

Pinned by `Surfaces stay usable after their context is destroyed` in
`tests/test_decode.c`, which destroys the context first and then syncs and
destroys the surfaces.

To view which codecs your card is capable of decoding you can use the `vainfo` command with this driver installed, or visit the NVIDIA website [here](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new#geforce).

## Encode Support

Hardware encoding is exposed through the VA-API `VAEntrypointEncSlice` entrypoint and is backed by NVENC. It becomes available when a usable NVENC engine is detected (either directly or via the [NVENC encode helper](#nvenc-encode-helper)).

Encode entrypoints are **capability-gated at runtime** via NVENC GUID / profile
/ input-format / caps probes at driver init — the profile list below is
the fork's implementation ceiling; the actual set exposed to `vainfo`
depends on what your card admits. `NVD_LOG=1` shows an `NVENC caps: ...`
line at driver init with the probe result.

| Codec | Supported | Profiles | Comments |
|---|---|---|---|
|H.264|:heavy_check_mark:|Constrained Baseline, Main, High, High10|High10 requires 10-bit encode capability + `NV_ENC_H264_PROFILE_HIGH_10_GUID` (probed).|
|HEVC|:heavy_check_mark:|Main, Main10, Main422_10, Main444, Main444_10|Main422_10 / Main444 / Main444_10 all live behind the FREXT profile GUID + the corresponding NVENC input format (P210 / YUV444 / YUV444_10BIT); each is gated independently.|
|AV1|:heavy_check_mark:|Profile0|10-bit 4:2:0 input exposed when NVENC advertises `SUPPORT_10BIT_ENCODE`. Requires an NVENC engine with AV1 encode support (Ada/Lovelace 40XX or newer).|
|VP8 / VP9 / MPEG-2 / VC-1 / MPEG-4 / JPEG / MJPEG|:x:||Not implemented — NVENC's public API does not expose these encode profiles.|
|HEVC Main12 / Main444_12|:x:||Decode-only. NVENC public API does not expose 12-bit HEVC encode.|

Actual encode capabilities depend on your GPU's NVENC generation. `vainfo` is the source of truth.

Two things about how encode capability is *reported*, both of which used to be
wrong in ways clients could not work around:

* **Maximum encode dimensions are probed per codec**, from
  `NV_ENC_CAPS_WIDTH_MAX` / `HEIGHT_MAX`, rather than being a single constant.
  They are not uniform — on an RTX 5080 H.264 reports 4096x4096 while HEVC and
  AV1 report 8192x8192 — so the previous hardcoded 4096 hid half of each axis of
  8K HEVC/AV1 capability. `NVD_LOG=1` prints an `NVENC max encode size:` line at
  init.
* **Encode-only profiles are unioned into `vaQueryConfigProfiles`.** That list
  is built from the NVDEC *decode* probe, and both Chromium and GStreamer
  enumerate profiles first and only then ask for entrypoints — so a profile
  missing from it is unreachable no matter what the entrypoint query says. This
  hid H.264 High10 (which has no decode branch at all) and HEVC Main422_10
  (whose decode path is compiled out, since NVDEC has no 4:2:2 support) on
  hardware whose encoder handles both.

### Packed headers

NVENC authors its own SPS/PPS/VPS and slice headers and offers no API to inject
a client-built slice header. So `VAConfigAttribEncPackedHeaders` advertises
`SEQUENCE | MISC | RAW_DATA` — deliberately **not** `SLICE` or `PICTURE`.

That incomplete set is the point. Chromium's `H264VaapiVideoEncoderDelegate`
treats packed headers as all-or-nothing: only when `SEQUENCE|PICTURE|SLICE` are
*all* advertised does it build them, and it then expects the driver to use them.
Since we cannot, advertising the full set made Chromium build SPS/PPS and
per-slice headers (with `has_emulation_bytes = 0`, expecting the driver to
insert emulation-prevention bytes) that were then silently discarded — wasted
work, and a standing risk that its `frame_num`/ref-list assumptions diverge from
the headers NVENC actually wrote. With the set incomplete, Chromium configures
`VA_ENC_PACKED_HEADER_NONE` and leaves header authoring to the driver, which is
what NVENC does anyway. FFmpeg and GStreamer both tolerate the missing `SLICE`
bit and fall back the same way; they only need `MISC` / `RAW_DATA` to gate SEI
and AUD insertion.

### Encode logging

The encode path re-runs its entire parameter plumbing on every frame — Chromium
resends the sequence, picture and misc parameter buffers each time — so anything
logged unconditionally in there comes out several times per frame. At 4K60 that
is hundreds of identical lines a second, and it buries the things worth seeing:
a resolution change, a bitrate change, a reconfigure, an error.

So the per-frame sites are quiet unless something actually changes. Each one
keeps a snapshot of what it last printed and only logs on a difference —
geometry (surface/encoder/copy dimensions, format, bit depth), sequence
parameters, rate control, framerate, and temporal-layer structure. A steady
stream therefore logs each of these exactly once, at the start; a mid-session
change still prints immediately. What NVENC actually programmed is reported
separately by `nvenc_reconfigure_if_needed`, which was already change-driven.

Pure per-frame progress — `frame N encoded, X bytes` and `frame N buffered` —
has no meaningful "changed" state, so it moved behind `NVD_LOG_VERBOSE=1`.
Use `NVD_STATS=1` for throughput instead.

Measured on a 240-frame 3840x2112 H.264 encode: **560 log lines before, 79
after**, with the remainder being one-time session setup.
`NVD_LOG_VERBOSE=1` restores the full per-frame detail. Pinned by
`tests/test_log_verbosity.sh`, which asserts the log stays bounded by session
setup rather than growing with frame count.

**Deciding whether to log costs nothing when logging is off.** `LOG()` cannot
be made a guarded macro — 76 call sites rely on it supplying its own trailing
semicolon — so it always calls `logger()`, which early-returns when there is no
destination. That is fine for once-per-session sites, but a change-detecting
site has to *build and compare a value* before it can decide, and doing that on
every frame regardless of whether anyone is listening is exactly the kind of
cost that should not exist on a hot path. Every such site is therefore wrapped
in `LOG_ENABLED()` — a plain global bool, so it compiles to a single load and
an easily-predicted branch, with the comparison laid out as a cold path the
compiler jumps over. `nvenc_log_state_changed()` also returns `false` outright
when logging is disabled, so a call site that forgets the guard costs one
branch rather than a `memcmp` plus `memcpy` per frame. `NVD_LOG_VERBOSE` now
also requires a destination, so verbose without `NVD_LOG` is inert instead of
formatting output that gets dropped.

In practice the saving is below measurement noise at 4K (a ~20-byte `memcmp`
against a 4K NVENC encode is nothing) — this is about the hot path not carrying
avoidable work, not about a throughput win.

### Rate control

Modes: CQP, CBR, VBR. `VAEncMiscParameterRateControl` parses
`bits_per_second`, `target_percentage`, `initial_qp`, `min_qp`, `max_qp`.
`initial_qp` seeds `NV_ENC_CONFIG.rcParams.initialRCQP` (CBR/VBR) or
`constQP` (CONSTQP); `min_qp` / `max_qp` set NVENC's adaptive-QP bounds
via `enableMinQP` / `enableMaxQP`. For CONSTQP, `VAEncPictureParameterBuffer`'s
`pic_init_qp` (H.264) and equivalent (HEVC) can override QP per picture —
picked up by `nvenc_reconfigure_if_needed` before the next frame.

For **AV1** the per-picture quantizer arrives as
`VAEncPictureParameterBufferAV1::base_qindex` instead. This matters because it
is the *entire* rate-control channel under Chromium: its AV1 encoder delegate
runs the config in CQP mode and does rate control itself in software, pushing
the result as a fresh `base_qindex` on every frame — it sends no
`VAEncMiscParameterTypeRateControl` for AV1 at all. AV1's qindex is 0-255 and is
exactly what NVENC's AV1 encoder takes as its QP, so it maps across without
rescaling; `min_base_qindex` / `max_base_qindex` feed the adaptive-QP bounds.
Pinned by the `AV1 base_qindex drives coded size` case in `tests/test_encode.c`.

The client-side format-negotiation caveat still applies: some ffmpeg paths
may renegotiate HEVC rext + `yuv444p10le` down to `yuv420p10le` before
frames reach the driver — verify the effective output with `ffprobe`.

### Format-plumbing status for the newer profiles

**Config advertisement and NVENC init** for H.264 High10, HEVC Main422_10 /
Main444 / Main444_10 are wired end-to-end (probe → advertise correct
`RTFormat` → allocate config → configure NVENC with the right
`chromaFormatIDC` + `pixelBitDepth`). **Actual encode data-path** for YUV444
and YUV422 surfaces reuses the existing NV12/P010 plumbing where the CUDA
copy assumes 4:2:0 subsampled chroma; running these profiles end-to-end
with real YUV444/422 pixel data may need additional CUDA copy tuning for
the correct chroma plane layout. Intended for transcoding-style workloads
where the source is already in the target chroma format — a full data-path
audit is not done. If you hit issues, please file with an NVD_LOG=1 trace.

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

### AV1 VUI defaults (WebRTC-tuned)

For AV1 encode we explicitly pin `NV_ENC_CONFIG_AV1.chromaFormatIDC = 1`
(4:2:0; NVENC's bit-packed default of 0 produces mis-parsed chroma at the
receive side) and set `colorRange = pc`, `colorPrimaries = BT.709`,
`transferCharacteristics = BT.709`, `matrixCoefficients = BT.709`. These
match Chrome / WebRTC's default capture pipeline (full-range BT.709 YUV
from `getUserMedia` and `getDisplayMedia`), so a same-tab loopback decodes
with consistent hue/gamma. Verified end-to-end with an external `libdav1d`
decode of a real Chrome loopback capture — every frame renders as the
original content, no chroma corruption.

### Client-driven IDR (no NVENC auto-refresh)

`NV_ENC_CONFIG.gopLength = NVENC_INFINITE_GOPLENGTH` for all three codecs.
The client (ffmpeg, Chrome, Steam) already drives IDR insertion via
`VAEncPictureParameterBuffer.idr_pic_flag`, which nvenc_dispatch translates
to `NV_ENC_PIC_FLAG_FORCEIDR`. Letting NVENC also emit its own IDR every
`intraPeriod` frames created a race between two IDR schedulers that
produced conflicting RPS state at GOP boundaries — encoded streams
referencing POCs the decoder had already evicted. Client-only IDR is the
canonical shape for WebRTC anyway (BWE and packet-loss recovery request
keyframes explicitly).

### Known limitations

- **HEVC cross-GOP RPS on high-detail content.** `hevc_vaapi` on
  content with fine detail and motion decodes with
  `Could not find ref with POC X / Error constructing frame RPS`
  starting at the second GOP boundary. Single-GOP encodes score ~59 dB
  PSNR (perfect); multi-GOP high-detail scores ~27 dB with visible
  corruption. Config overrides tried (`repeatSPSPPS=1`,
  `idrPeriod=infinite`, `maxNumRefFramesInDPB=4`, `gopLength=infinite`,
  POC-reset-on-IDR) all produced bit-identical output — NVENC's preset
  config is authoritative for these fields on the low-latency P4 preset
  we currently use. Root cause deferred; regression is pinned by the
  `hevc_vaapi` case in `tests/test_encode_roundtrip.sh` so a future fix
  is verifiable. Workaround: use H.264 or AV1 for the affected paths;
  smpte-bars-shaped content (large flat regions) remains fine on HEVC.
- **YUV444 / YUV422 encode data path.** Config advertisement and NVENC
  init are wired end-to-end for the H.264 High10 / HEVC Main422_10 /
  Main444 / Main444_10 profiles (see [Format-plumbing status](#format-plumbing-status-for-the-newer-profiles)),
  but the shared CUDA input-copy path was written for NV12/P010 4:2:0
  and hasn't been audited for correct chroma-plane addressing on 4:2:2
  and 4:4:4 sources. Intended for transcoding-style workloads where
  the source is already in the target chroma format.
- **Decode surfaces shorter than 176px fall back to software.** The
  driver advertises `VASurfaceAttribMinHeight = 176` for decode configs
  even when NVDEC reports a smaller minimum (typically 64). NVIDIA's
  block-linear DRM format modifier encodes a GOB block height chosen
  from each *plane's* height, and a 4:2:0 chroma plane is half the luma
  height — so below a threshold chroma lands in a smaller block-height
  bucket than luma and the two planes carry genuinely different
  modifiers. Measured on this driver the two converge at exactly luma
  height **172**, independent of width and identical for NV12 and P010.
  Below that there is no `VADRMPRIMESurfaceDescriptor` that is both
  correct and safe: a descriptor object carries exactly one modifier, so
  a single-object export (SINGLE/COMBINED) describes chroma with luma's
  tiling and renders green macroblocks, while a one-object-per-plane
  export (MULTI) reports the true modifiers and trips Chromium's
  `CHECK_EQ(objects[0].modifier, objects[i].modifier)` in
  `ExportVASurfaceAsNativePixmapDmaBufUnwrapped` — a GPU-process abort,
  not a recoverable error. Advertising the floor makes clients
  transparently use software for sub-QCIF content instead. Pinned by
  `tests/test_descriptor_mode.c` (both the AUTO and `descriptor_mode_multi`
  runs). This matches the mitigation in upstream issue #440.

## Video post-processing (`VAEntrypointVideoProc`)

Advertised for `VAProfileNone`. Supports NV12/P010/P012 → RGB colour conversion
(BT.601/709/2020, limited and full range) and same-format plane copies, with
**crop and scale** on both.

Geometry matters more than it looks. Chromium reaches this path through
`VaapiImageProcessorBackend` whenever a decoded surface cannot be imported into
EGL directly — it then asks for AR24/BGR4 output *with* a source rectangle and a
different output size. The driver used to reject any request whose source and
destination regions were not identical and origin-aligned, and Chromium's
response to that failure is not to retry differently: it drops the entire stream
to software decode. The same geometry shows up on the encode side, where
Chromium blits camera/screen-capture frames to the encoder's input size.

How it is implemented:

* **1:1 origin-aligned blits** keep the existing GPU path — a PTX kernel for
  YUV→RGB, `cuMemcpy2D` for same-format copies. This is the common case and is
  unchanged.
* **Pure crops** at the same scale are done on the GPU with `cuMemcpy2D`
  source/destination offsets: exact, and free.
* **Anything that changes size** is resampled bilinearly on the CPU. The PTX
  kernels sample the source at the destination coordinate, so they can only
  express an identity blit; adding a GPU scaler would mean hand-writing another
  PTX kernel. Since this is a fallback that only runs when a client explicitly
  asks for scaling, correctness was worth more than throughput here — a working
  CPU scale beats `VA_STATUS_ERROR_OPERATION_FAILED` and a whole stream on
  software decode.

Not supported: deinterlacing, rotation, mirroring, blending, or any
`VAProcFilter`. `vaQueryVideoProcFilters` / `vaQueryVideoProcPipelineCaps` are
unavailable (the driver does not install a `VADriverVTableVPP`); no browser
calls them, but GStreamer's `vapostproc` does.

Pinned by `tests/test_vpp.c`.

> **Note:** fixing this surfaced a related bug in `vaCreateImage`, which
> reported `pitches[i] = width * bppc` and ignored the channel count. That is
> right by coincidence for NV12/P010 (the UV plane's horizontal subsampling
> cancels its two channels) but 4x too small for the packed RGB formats, so
> `vaGetImage`/`vaPutImage` on an RGB image failed with
> `CUDA_ERROR_INVALID_VALUE`. Relatedly, `vaGetImage` required the surface to
> have a decode context, which VA-API does not ask for and which made VideoProc
> outputs unreadable; it now requires only a realised backing image.

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
| `NVD_LOG_VERBOSE` | Set to anything other than `0` to include the per-frame debug logging that is suppressed by default — see [Encode logging](#encode-logging). Noisy: expect several lines per frame. |
| `NVD_MAX_INSTANCES` | Controls the maximum concurrent instances of the driver will be allowed per-process. This option is only really useful for older GPUs with not much VRAM, especially with Firefox on video heavy websites. |
| `NVD_BACKEND` | Controls which backend this library uses. Either `egl`, or `direct` (default). See [direct backend](#direct-backend) for more details. |
| `NVD_MAX_DETACHED_BACKING_IMAGE_BYTES` | Upper bound (in bytes) on the size of the detached backing-image cache used by the direct backend to recycle decode surfaces across stream switches. Lower this on low-VRAM GPUs to reduce memory usage at the cost of more re-allocation when streams change. Set to `0` to disable detached caching. Default: scales with the GPU — total VRAM / 64 (~1.6%), clamped to 64 MiB–512 MiB; falls back to `134217728` (128 MiB) if the VRAM size cannot be queried. |
| `NVD_MAX_DETACHED_BACKING_IMAGES` | Upper bound on the number of cached detached backing images. Set to `0` to disable detached caching. Default: `16`. |

## Firefox

Due to license, Firefox on Linux does not support HEVC till now.
To use the driver with firefox you will need at least Firefox 96, `ffmpeg` compiled with vaapi support (`ffmpeg -hwaccels` output should include vaapi), and the following config options need to be set in the `about:config` page:

> **H.264 and HEVC need a *system* ffmpeg.** Firefox's bundled ffvpx is built
> with only `vp9_vaapi,vp8_vaapi,av1_vaapi` hwaccels, and its H.264/HEVC
> decoders are compiled in only when a system libavcodec is present. If
> `vainfo` shows the H.264 profiles but Firefox still decodes H.264 in
> software, this — not the driver — is usually why.

| Option | Value | Reason |
|---|---|---|
| media.ffmpeg.vaapi.enabled | true | Required until Firefox 137, enables the use of VA-API. Removed entirely in later builds ([bug 1748862](https://bugzilla.mozilla.org/show_bug.cgi?id=1748862)) — harmless to leave set. |
| media.hardware-video-decoding.force-enabled | true | Required since Firefox 137. **`force-enabled`, not `enabled`** — NVIDIA is blocklisted unconditionally for `FEATURE_HARDWARE_VIDEO_DECODING` in `widget/gtk/GfxInfo.cpp` ("Disable on all NVIDIA hardware"), and only the force pref outranks a blocklist entry. It does *not* override a failed probe. |
| media.rdd-ffmpeg.enabled | true | Required, default on FF97. Forces ffmpeg usage into the RDD process, rather than the content process. |
| media.av1.enabled | false | Optional, disables AV1. If your GPU doesn't support AV1, this will prevent sites using it and falling back to software decoding. |
| gfx.x11-egl.force-enabled | true | Required, this driver requires that Firefox use the EGL backend. It may be enabled by default. It is recommended to test it with the `MOZ_X11_EGL=1` environment variable before enabling it in the Firefox configuration. |
| widget.dmabuf.force-enabled | true | Required on NVIDIA 470 series drivers. Note that Firefox isn't coded to allow DMA-BUF support without GBM support, so it may not function completely correctly when it's forced on. |

In addition the following environment variables need to be set. For permanent configuration `/etc/environment` may suffice.

| Variable | Value | Reason |
|---|---|---|
| MOZ_DISABLE_RDD_SANDBOX | 1 | Disables the sandbox for the RDD process that the decoder runs in. Needed for two independent reasons: this driver skips CUDA init when it cannot read `/proc/version` (bypassable on its own with `NVD_FORCE_INIT=1`), and `RDDSandboxPolicy` returns `ENOTTY` for ioctl type `'F'` — the NVIDIA RM magic — while the file broker denies `/dev/nvidiactl` and `/dev/nvidia*`. |
| LIBVA_DRIVER_NAME | nvidia | Required for libva 2.20+, forces libva to load this driver. |
| __EGL_VENDOR_LIBRARY_FILENAMES | /usr/share/glvnd/egl_vendor.d/10_nvidia.json | Required for the 470 driver series only. It overrides the list of drivers the glvnd library can use to prevent Firefox from using the MESA driver by mistake. |
| CUDA_DISABLE_PERF_BOOST | 1 | Optional. Requires NVIDIA driver >= 580.105.08. Disables the forced power boost the GPU gets when CUDA is activated. This should reduce the power usage when decoding video. This setting is the equivilent of the 'CUDA Force P2' NVIDIA Profile Inspector setting on Windows. |

When libva is used it will log out some information, which can be excessive when Firefox initalises it multiple times per page. This logging can be suppressed by adding the following line to the `/etc/libva.conf` file:
```
LIBVA_MESSAGING_LEVEL=1
```

If you're using the Snap version of Firefox, it will be unable to access the host version of the driver that is installed.

### What Firefox actually requires from the driver

Firefox does not go through FFmpeg's VA-API surface export — it wraps libva
itself (`VALibWrapper.cpp`) and binds exactly **two** symbols,
`vaExportSurfaceHandle` and `vaSyncSurface`. It refuses to use libva at all if
`vaExportSurfaceHandle` is missing. Two consequences worth knowing, both pinned
by `tests/test_client_contracts.c`:

* **Export happens before sync, and a failing sync is only logged.** Firefox
  calls `vaExportSurfaceHandle` first and `vaSyncSurface` afterwards, so the
  export itself has to return a fully resolved surface. Any driver logic that
  finalized a frame inside `vaSyncSurface` would race here. This driver resolves
  inside the export, so the ordering is safe.
* **Only NV12, YV12, P010 and P016 are importable.**
  `DMABufSurfaceYUV::ImportPRIMESurfaceDescriptor` understands nothing else and
  mis-imports silently rather than failing. 12-bit surfaces are therefore
  exported with the descriptor fourcc `P016` rather than `P012` — the two are
  the same two-plane 16-bit-container layout, differing only in how many of the
  container bits are significant, and the samples are left-aligned so the unused
  low bits read as zero. The surface is still reported as `P012` through
  `vaQueryImageFormats` and the surface attributes, so clients that understand
  the distinction are unaffected.

Note also that Firefox has **no VA-API encode path at all** — see
[Encode Support](#encode-support).

## Chrome

This fork includes the Chromium-compatible single-buffer export path. For Chrome / Chromium based browsers, set `LIBVA_DRIVER_NAME=nvidia` and start the browser with flags similar to:

```sh
LIBVA_DRIVER_NAME=nvidia google-chrome \
  --enable-features=VaapiOnNvidiaGPUs,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL,AcceleratedVideoEncoder,VaapiIgnoreDriverChecks \
  --ignore-gpu-blocklist \
  --use-gl=angle --use-angle=gl
```

**`VaapiOnNvidiaGPUs` is the flag that actually matters.** Without it Chromium
skips any render node whose DRM driver name is `nvidia-drm` outright — decode
*and* encode, before any driver code runs (`media/gpu/vaapi/vaapi_wrapper.cc`,
citing `crbug.com/1492880`). No other flag substitutes for it.

The rest, and what they are really for:

| Flag | Why |
|---|---|
| `AcceleratedVideoDecodeLinuxGL` | Enabled by default, but harmless to state explicitly. |
| `AcceleratedVideoDecodeLinuxZeroCopyGL` | Enabled by default. Lets Chromium use the exported NV12/P010 dma-buf directly instead of running it through a VPP conversion to ARGB. |
| `AcceleratedVideoEncoder` | **Disabled by default.** Without it Chrome never probes the VA-API encoder and silently falls back to software (`libaom` for AV1, OpenH264 for H.264). |
| `VaapiIgnoreDriverChecks` | Only consulted on the ANGLE-**Vulkan** path, where Chromium otherwise refuses a non-Intel Vulkan vendor. On `--use-angle=gl` it is a no-op; harmless to leave on. |
| `--ignore-gpu-blocklist` | Widely recommended, but the only Linux/NVIDIA video entry in `software_rendering_list.json` is scoped to driver versions older than 331.38, so modern drivers are not blocklisted. Not what is blocking you. |

> **Flag names changed around M131.** `VaapiVideoDecodeLinuxGL`,
> `VaapiVideoEncoder` and `AcceleratedVideoEncodeLinuxGL` are the old spellings;
> the current registered names are `AcceleratedVideoDecoder` /
> `AcceleratedVideoEncoder` and the `...LinuxGL` variants above. There are no
> `chrome://flags` entries for any of these — `--enable-features=` only.

On Wayland, also try `--ozone-platform=wayland` or `--ozone-platform-hint=auto`.

### What Chromium actually requires from the driver

Chromium dlopens libva and binds exactly 44 symbols (`media/gpu/vaapi/va.sigs`).
`vaSyncBuffer`, `vaQuerySurfaceStatus`, `vaAcquireBufferHandle`, `vaCopy` and
`vaMapBuffer2` are **not** among them, and the protected-content entry points
are ChromeOS-only — so none of those matter here. What does:

* **Exported objects must all report the same `drm_format_modifier`.** Chromium
  `CHECK_EQ`s them, which aborts the GPU process rather than falling back. See
  [Known limitations](#known-limitations) for the 176px height floor this forces.
* **`vaCreateContext` is called with zero render targets**, so the driver sizes
  its decode surface pool itself — and must recycle picture indices, see
  [Picture-index recycling](#picture-index-recycling-upstream-397).
* **`vaDeriveImage` must fail with exactly `VA_STATUS_ERROR_OPERATION_FAILED`.**
  Chromium's encode upload path only falls back to `vaCreateImage` +
  `vaPutImage` on that specific status; any other error is fatal.
* **Encode input dma-bufs are imported with the legacy
  `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`** plus `VASurfaceAttribExternalBuffers`,
  not PRIME_2 — Chromium only uses PRIME_2 import for drivers it recognises by
  vendor string (iHD / Mesa Gallium), and ours is classified as "other".
* **A VideoProc blit may carry a source rectangle and a different output size** —
  see [Video post-processing](#video-post-processing-vaentrypointvideoproc).

Being classified as "other" is not all bad: Chromium takes the global VA-API
lock around every call for unrecognised drivers, so it never exercises this
driver's thread-safety on its own.

Two things Chromium cannot use no matter what the driver advertises: **HEVC
encode** (its encodable-profile allowlist is H.264 CBP/Main/High, VP8, VP9
Profile0, AV1 Profile0 and JPEG) and **hardware temporal SVC** on Linux
(`GetSupportedScalabilityModes` returns `{kL1T1}`; the L1T2/L1T3 block is
`#if BUILDFLAG(IS_CHROMEOS)`, and Chromium implements H.264 temporal layers
itself via reference-list manipulation).

### WebRTC temporal scalability (screenshare)

WebRTC screenshare uses temporal SVC (e.g. `scalabilityMode=L1T2`). This driver
advertises temporal-layer support for AV1 (`VAConfigAttribEncRateControlExt`,
up to 4 layers) and programs NVENC's temporal SVC from the layer structure
supplied by the client.

> **Chrome on Linux will not use this.** Its
> `VaapiVideoEncodeAccelerator::GetSupportedScalabilityModes` returns `{kL1T1}`
> off ChromeOS — the whole L1T2/L1T3/L2T2Key block is inside
> `#if BUILDFLAG(IS_CHROMEOS)` — so hardware SVC is never negotiated, and
> `VAConfigAttribEncRateControlExt` is never even read. Where Chrome does use
> H.264 temporal layers it builds them itself, by manipulating `frame_num` and
> the reference lists and emitting a prefix NALU as raw packed-header data; no
> VA-API SVC attribute is involved. The driver's AV1 SVC path is therefore
> reachable from GStreamer, OBS and other non-browser clients, not from Chrome.
>
> If `chrome://webrtc-internals` reports `encoderImplementation: libaom`,
> hardware encode is not being selected at all — check `AcceleratedVideoEncoder`
> and `VaapiOnNvidiaGPUs` are set, rather than looking for an SVC problem.
> `--disable-features=WebRtcAllowsSvcHardwareFallback` will surface the real
> encoder-selection result instead of a silent fallback.

What Chrome's WebRTC *does* need from the encoder is narrower than it looks:
`VA_RC_CBR` honoured with mid-stream `VAEncMiscParameterTypeRateControl` /
`FrameRate` / `HRD` updates applied without rebuilding the context (see
[Live rate-control / framerate updates](#live-rate-control--framerate-updates-webrtc-bwe)),
`idr_pic_flag` honoured on demand, and no silent frame dropping —
`rc_flags.bits.disable_frame_skip` is set on every frame. WebRTC always requests
`Bitrate::ConstantBitrate`, so the VBR path is never exercised by the browser.

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

That's the whole rule: it uses the deterministic `isEncode` flag on the
surface's VA-API context and nothing else. No resolution guessing, no
inference from other live contexts.

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

> **Known caveat:** encode-vs-decode is the only signal we have. There's no
> VA-API flag that says "this decode is a local self-preview," so a workflow
> that relies on Chrome's decode-back self-preview path (Chrome decoding its
> own just-encoded stream back to render the local thumbnail) will import
> that surface as SINGLE and hit `EGL_BAD_MATCH` in the WebGL/canvas worker.
> If that describes your setup, either force `NVD_DESCRIPTOR_MODE=combined`
> globally (at the cost of remote decode display), or set
> `NVD_SELF_PREVIEW_COMBINED=1` (see below).

#### `NVD_SELF_PREVIEW_COMBINED=1` (opt-in escape hatch)

Previously the AUTO default *also* forced COMBINED on any decode surface
whose resolution matched a currently-active local encode context, on the
theory that this was always Chrome's decode-back self-preview thumbnail.
That assumption turned out to be wrong for real WebRTC meetings: video
platforms (Meet, Zoom, Slack, corporate tools) negotiate every peer to
common resolutions (720p / 540p / 360p simulcast rungs) — so every remote
peer whose incoming stream happens to be at your camera's resolution was
mis-classified as a self-preview and rendered with **green macroblock
corruption** in Chrome's normal decode-display importer.

The resolution-match heuristic is therefore off by default. Set
`NVD_SELF_PREVIEW_COMBINED=1` to re-enable it if you rely specifically on
Chrome's decode-back self-preview path (rare — most WebRTC apps render the
local thumbnail directly from `getUserMedia` without touching the encoder).
When set, an `NVD_LOG=1` line at driver init confirms it's active. Leave
unset (the default) for any normal video-conferencing setup.

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
| `test_decode` | AV1 (8-bit + 10-bit) decode init, combined-RTFormat resolution, DMA-BUF export, auto descriptor mode (asserts decode surfaces stay SPLIT even when a same-res encode context is live — the WebRTC-peer green-macroblock regression guard). |
| `test_descriptor_mode` | Standalone regression for the AUTO descriptor-mode contract: decode-only surface → SPLIT; decode + concurrent encode @ same res → SPLIT (no green macroblocks on peers); with `NVD_SELF_PREVIEW_COMBINED=1` → same case flips to COMBINED (opt-in fallback). |
| `test_encode` | Encode entrypoints, config attributes, single-frame encode for H.264 / HEVC / HEVC Main10 / AV1 / AV1 Main10, rate control + quality-level params, AV1 temporal SVC and combined-RTFormat encode, dynamic resolution, sequential encodes, coded-buffer reuse, long-running single session, live bitrate/framerate reconfigure, auto-combined encode export, decode-still-works co-existence, dimension-mismatch, H.264 B-frames. |
| `test_encode_config` | Config-side coverage: entrypoints, RTFormat, rate control, packed headers, ref frames, max dimensions, quality range, surface allocation (NV12 / P010 / small / 4K), export descriptor. |
| `test_ipc_fuzz` | Fuzz surface for the NVENC out-of-process IPC helper (invalid commands, truncated inits, oversized payloads, rapid connect/disconnect, double-init, encode-without-init). |
| `test_concurrent_sessions` | Multi-session stress covering the WebRTC "camera + screenshare" flow: two H.264 encoders in parallel at the same resolution, staggered encoder-B-added-mid-stream (mimics `getDisplayMedia` while camera is live), encoder+decoder co-existence at the same resolution, secondary-encoder create/destroy churn, and a descriptor-shape stability probe that asserts a decode surface exports a bit-identical `VADRMPRIMESurfaceDescriptor` whether captured cold or during a concurrent live encoder (fourcc, dimensions, num_objects, num_layers, per-object size + modifier, per-layer format, per-plane offset + pitch all field-diffed). Hitting the NVENC concurrent-session cap reports SKIP, not FAIL. |
| `tests/test_ffmpeg.sh` | End-to-end ffmpeg + VA-API smoke test. Defaults to `samples/smptebars_h264.mp4` (produced by `samples/gensamples.sh`); override with a positional path argument. |
| `tests/test_encode_roundtrip.sh` | Encode → software-decode → PSNR roundtrip for each hardware codec (H.264, HEVC, AV1). Reference and decoded output are both dumped to raw yuv420p so no container colorspace-label mismatch pollutes the comparison. Threshold 30 dB — sits solidly between "legit lossy encode at 20 Mbps" (typically 40-70 dB on the fixtures) and "bitstream garbage" (typically low-20s or worse). Two fixtures: the shipped smpte bars, plus a high-frequency testsrc2 stress source generated at test time. Current known failure: `hevc_vaapi` on the stress fixture (RPS reconstruction on the second GOP boundary — see [Known limitations](#known-limitations)). |
| `tests/test_gstreamer.sh` | End-to-end GStreamer VA-API smoke test. |
| `tests/test_log_verbosity.sh` | Asserts the encode log stays bounded by session setup rather than growing with frame count, and that `NVD_LOG_VERBOSE=1` still restores the full per-frame detail. See [Encode logging](#encode-logging). |
| `tests/test_vp8_decode.sh` | VP8 hardware decode vs libvpx, bit-exactness required. Guards the reconstructed uncompressed data chunk — see [VP8 bitstream reconstruction](#vp8-bitstream-reconstruction). Generates its own fixture. |
| `test_vpp` | `VAEntrypointVideoProc` blit geometry: 1:1, crop, scale, and crop+scale, verified by painting a luma ramp and reading back known pixels. |
| `test_client_contracts` | The low-level behaviours Chromium/Firefox/FFmpeg depend on and that are easy to break silently: unknown config attributes reported as `VA_ATTRIB_NOT_SUPPORTED`, `vaCreateConfig` with zero attributes, `vaDeriveImage` failing with exactly `VA_STATUS_ERROR_OPERATION_FAILED`, export-before-sync resolving correctly, and the exported descriptor fourcc staying within the set Firefox can import. |

Every code change to this fork lands with a test — see [Development workflow](#development-workflow) below.

> **GStreamer needs `GST_VA_ALL_DRIVERS=1`.** The `va` plugin allow-lists driver
> vendor strings (`gstvadisplay.c` prefix-matches "Mesa Gallium driver",
> "Intel i965 driver", "Intel iHD driver"); anything else is
> `GST_VA_IMPLEMENTATION_OTHER` and display creation fails unless that variable
> is set — no elements register at all. Note the older `gstreamer-vaapi` plugin
> (`GST_VAAPI_ALL_DRIVERS`) had no such filter, but was removed in GStreamer 1.28.

> **`vaSyncBuffer` is deliberately left unimplemented** (a NULL vtable slot, so
> libva returns `VA_STATUS_ERROR_UNIMPLEMENTED`). FFmpeg decides whether to use
> asynchronous encode with a single probe call —
> `vaSyncBuffer(display, VA_INVALID_ID, 0)` — and treats **any** return other
> than `VA_STATUS_ERROR_UNIMPLEMENTED` as "async supported". A well-meaning stub
> returning `VA_STATUS_SUCCESS` would silently promote FFmpeg to a mode this
> driver does not implement. Neither Chromium nor Firefox binds the symbol at all.

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
  (Constrained Baseline / Main / High / **High10**), HEVC (Main, Main10,
  **Main422_10**, **Main444**, **Main444_10**) and AV1 (Profile0). Exposed
  via `VAConfigAttribRateControl`, packed headers, quality level, ref
  frames, max dimensions, and (AV1) temporal SVC. Profile advertisement is
  **runtime capability-gated** via NVENC `nvEncGetEncodeGUIDs` /
  `nvEncGetEncodeProfileGUIDs` / `nvEncGetInputFormats` — a probed
  `NVENC caps:` line lands in `NVD_LOG=1` at driver init showing the
  actual bitset. High-tier profiles land only when the card exposes both
  the profile GUID and the matching input format (P210 for 422_10,
  YUV444/YUV444_10BIT for the 444 variants, H264 High10 GUID for High10).
- **Rate-control QP hints** — `VAEncMiscParameterRateControl` now parses
  `initial_qp` / `min_qp` / `max_qp` and programs them into NVENC
  (`constQP` for CONSTQP mode, `initialRCQP` for CBR/VBR, `enableMinQP` /
  `enableMaxQP` bounds always). Per-picture `pic_init_qp` in
  `VAEncPictureParameterBufferH264`/`HEVC` is picked up too — in CONSTQP
  mode `nvenc_reconfigure_if_needed` rewrites `constQP` before the next
  encode when the requested QP moves.
- **AV1 VUI + chroma pinning** — `NV_ENC_CONFIG_AV1.chromaFormatIDC = 1`
  (bit-packed field, defaults to 0 from `memset` which mis-parses
  chroma at the receive side), plus explicit `colorRange = pc`,
  BT.709 primaries / transfer / matrix. Matches Chrome / WebRTC's
  default full-range BT.709 capture pipeline so a same-tab AV1
  loopback decodes with correct hue and range. Verified externally
  via `libdav1d` on a captured Chrome loopback bytestream.
- **Client-driven IDR (`gopLength = infinite` for all codecs)** — the
  client (ffmpeg / Chrome / Steam) is the sole IDR scheduler via
  `VAEncPictureParameterBuffer.idr_pic_flag → NV_ENC_PIC_FLAG_FORCEIDR`.
  NVENC no longer auto-inserts its own IDR at `intraPeriod`
  boundaries, removing the two-scheduler race that produced
  RPS-inconsistent bitstreams at GOP boundaries. WebRTC-canonical
  shape anyway (BWE + PLI drive keyframes explicitly).
- **HEVC defensive config** — `repeatSPSPPS = 1` (re-emit VPS/SPS/PPS
  on every IDR so mid-stream joiners and packet-loss recoverers can
  resync), `maxNumRefFramesInDPB = 4` (pin DPB size to what the
  low-latency preset actually uses), explicit `idrPeriod = infinite`
  (belt-and-suspenders with `gopLength = infinite` above). Also
  routes `inputBitDepth` / `outputBitDepth` and `chromaFormatIDC`
  (1=4:2:0, 2=4:2:2, 3=4:4:4) from the actual NVENC input format
  for the new HEVC profile variants.
- **Encode roundtrip PSNR test** — `tests/test_encode_roundtrip.sh`
  encodes via our VA-API driver, decodes with a software decoder
  (`libdav1d` for AV1, ffmpeg native for H.264/HEVC), and measures
  PSNR against raw-YUV reference. Threshold 30 dB. Both sides are
  raw yuv420p so container colorspace-label mismatches don't
  pollute the comparison. Catches bitstream-correctness bugs that
  "encode succeeded and produced bytes" tests happily miss (the
  original AV1 chromaFormatIDC regression scored 22-25 dB before
  the fix; correctly-configured encoders score 40-70 dB).
- **Multi-session concurrency test** — `tests/test_concurrent_sessions.c`
  covers the WebRTC "camera + screenshare" scenario: parallel
  encoders at the same resolution, staggered second-encoder-added-
  mid-stream, encoder + decoder co-existence at the same resolution,
  rapid create/destroy churn, and a descriptor-shape stability probe
  that asserts our DMA-BUF exports are bit-identical whether taken
  cold or during a concurrent live encoder. Consumer-card NVENC
  session cap is detected and reported as SKIP, not FAIL.
- **NVENC out-of-process helper** — `nvenc-helper.service` (user systemd unit)
  + `/usr/libexec/nvenc-helper` binary + IPC channel for sandboxed browser
  processes that can't init CUDA/NVENC directly. Rebuild + restart with
  `update-nvenc.sh`.
- **Chrome-compatible DMA-BUF descriptor mode** — `NVD_DESCRIPTOR_MODE` env
  var (default `auto`), picks per-surface layout for encode-context vs.
  decode-context surfaces using the deterministic `isEncode` flag on the
  VA-API context. **AUTO no longer uses the resolution-matching self-preview
  heuristic** (was disproven by real WebRTC calls: peers negotiate to the
  local camera's resolution and the heuristic false-triggered on every
  remote peer, producing green macroblock corruption in Chrome's normal
  decode-display importer). The legacy behavior is available behind
  `NVD_SELF_PREVIEW_COMBINED=1` for anyone who specifically depends on
  Chrome's decode-back self-preview path. Regression pinned by
  `tests/test_descriptor_mode.c`.
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
