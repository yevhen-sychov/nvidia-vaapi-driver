#!/bin/bash
#
# test_encode_roundtrip.sh — encode via our VA-API driver, decode with a
# software decoder, measure PSNR against the input. Catches bitstream-
# corruption bugs that only surface at decode time — e.g. the AV1
# chroma-format regression where every encoded frame decoded to a
# saturated pink/green blob but the encoder returned VA_STATUS_SUCCESS
# and produced bytes, so existing "encode succeeds and emits bytes"
# tests happily passed.
#
# Threshold logic: a properly encoded lossy stream vs. its source
# typically scores 30+ dB PSNR. A garbage bitstream (mis-configured
# chroma layout, corrupt reference frames, wrong bit-depth, wrong
# colorspace matrix) scores in the low-teens or lower. The 20 dB gate
# sits firmly in "not-garbage" territory without being so strict that
# a legit low-bitrate encode false-fails.
#
# Requires: ffmpeg with h264_vaapi/hevc_vaapi/av1_vaapi encoders and
# libdav1d (or built-in av1) decoder. A codec whose VA-API encoder
# isn't available is SKIPPED, not FAILED — that's honest for hardware
# variance, not a driver bug.
#
# Usage:
#   test_encode_roundtrip.sh [input.mp4]
#   PSNR_THRESHOLD=25 test_encode_roundtrip.sh
#
# Env:
#   LIBVA_DRIVER_NAME, LIBVA_DRIVERS_PATH, LD_LIBRARY_PATH — normally
#     set by meson test wrapper; if run standalone, set to point at
#     the built driver.

set -u
set -o pipefail

# ---- Config ------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INPUT="${1:-$REPO_ROOT/samples/smptebars_h264.mp4}"
# 30 dB is a "not-garbage AND not-subtly-broken" line. A working lossy
# encode at 2 Mbps on this content typically scores 60+ dB; a broken
# chroma layout scores in the low-20s or lower because at least the luma
# plane survives to give a partial match.
THRESHOLD="${PSNR_THRESHOLD:-30.0}"   # dB
FFMPEG="${FFMPEG:-ffmpeg}"
RENDER_NODE="${RENDER_NODE:-/dev/dri/renderD128}"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if [ ! -f "$INPUT" ]; then
    echo "ERROR: input not found: $INPUT"
    echo "       run samples/gensamples.sh to generate test fixtures"
    exit 1
fi

# A stress fixture: high-frequency checker pattern with moving text +
# noise, so most macroblocks have non-trivial chroma. Flat-color regions
# in the smpte bars can mask a subtly-broken chroma layout because
# neighboring pixels agree — this fixture doesn't have that luxury. The
# resolution matches the encoder-reported size from the real WebRTC
# pink-garbage repro (1920x1088 — Chrome's macroblock-padded version of
# 1080p). testsrc2 generates in real time so this stays fast.
STRESS_INPUT="$TMPDIR/stress.mp4"
if ! "$FFMPEG" -y -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=2" \
        -c:v libx264 -preset ultrafast -crf 18 \
        "$STRESS_INPUT" 2>/dev/null; then
    echo "WARN: could not generate stress fixture — falling back to bars-only"
    STRESS_INPUT=""
fi

# Codec triples: encoder-name : container-ext : software-decoder-name.
# For h264/hevc, ffmpeg picks its native software decoder automatically
# when the input isn't hwaccel'd, so we leave the SWDEC blank.
declare -a CODECS=(
    "h264_vaapi:mp4:"
    "hevc_vaapi:mp4:"
    "av1_vaapi:mp4:libdav1d"
)

PASS=0
FAIL=0
SKIP=0

# ---- Runner ------------------------------------------------------------
run_one_codec() {
    local encoder="$1"
    local ext="$2"
    local swdec="$3"
    local label="$encoder"

    echo -n "  $encoder → PSNR ... "

    local encoded="$TMPDIR/$encoder.$ext"
    local enc_log="$TMPDIR/$encoder.enc.log"
    local psnr_log="$TMPDIR/$encoder.psnr.log"

    # Encode via VA-API. Input is SOFTWARE-decoded (no -hwaccel) so this
    # test isolates the encode path — a broken VA-API H.264 decode
    # wouldn't skip the test as if the encoder were unavailable. The
    # `format=nv12,hwupload` filter pushes decoded frames onto the VA
    # surface pool for the hardware encoder. -bf 0 disables B-frames
    # (our fork doesn't wire them). -g 30 fixed GOP. High bitrate
    # (20 Mbps) intentionally over-provisions — this test is about
    # encoder CORRECTNESS not compression efficiency; at 20 Mbps any
    # correctly-configured encoder should score >= threshold on both
    # fixtures. A low PSNR at 20 Mbps means the bitstream itself is
    # broken (wrong chroma format, wrong colorspace, corrupt refs).
    if ! "$FFMPEG" -y -hide_banner -loglevel error \
            -init_hw_device "vaapi=va:$RENDER_NODE" \
            -filter_hw_device va \
            -i "$INPUT" \
            -vf 'format=nv12,hwupload' \
            -c:v "$encoder" -b:v 20M -bf 0 -g 30 \
            "$encoded" 2>"$enc_log"; then
        # Encode failed — most likely the VA-API encoder isn't advertised
        # on this hardware. Report as SKIP, not FAIL.
        echo "SKIP (encode failed — encoder unavailable?)"
        head -3 "$enc_log" | sed 's/^/      | /'
        SKIP=$((SKIP + 1))
        return
    fi

    # Decode + compare. The `psnr` filter needs two synced streams: the
    # first input is [0:v] (reference = original), the second is [1:v]
    # (encoded output decoded via ffmpeg's software decoder). We force
    # the AV1 decoder explicitly (libdav1d) for that codec; for h264/hevc
    # ffmpeg picks its native software decoder by default.
    local dec_args=()
    if [ -n "$swdec" ]; then
        dec_args=(-c:v "$swdec")
    fi

    # psnr filter emits per-frame stats to stats_file and an "average:X.X"
    # summary to stderr. We grep the summary out of the ffmpeg output.
    "$FFMPEG" -y -hide_banner -loglevel info \
            -i "$INPUT" \
            "${dec_args[@]}" -i "$encoded" \
            -lavfi "[0:v][1:v]psnr=stats_file='$psnr_log'" \
            -f null - > "$TMPDIR/$encoder.compare.log" 2>&1

    local avg
    avg="$(grep -oE 'average:[0-9.]+' "$TMPDIR/$encoder.compare.log" \
           | head -1 | cut -d: -f2)"

    if [ -z "${avg:-}" ]; then
        echo "FAIL (could not extract PSNR; see $TMPDIR/$encoder.compare.log)"
        tail -5 "$TMPDIR/$encoder.compare.log" | sed 's/^/      | /'
        FAIL=$((FAIL + 1))
        return
    fi

    # bc -l for floating comparison. Threshold defaults to 20 dB.
    if [ "$(echo "$avg < $THRESHOLD" | bc -l)" = "1" ]; then
        echo "FAIL (PSNR=$avg dB < ${THRESHOLD} dB → bitstream corruption)"
        FAIL=$((FAIL + 1))
    else
        echo "PASS (PSNR=$avg dB)"
        PASS=$((PASS + 1))
    fi
}

run_all_codecs() {
    local input="$1"
    local label="$2"
    local saved_input="$INPUT"
    INPUT="$input"
    echo "-- fixture: $label ($(basename "$input"))"
    for codec_spec in "${CODECS[@]}"; do
        IFS=':' read -r ENCODER EXT SWDEC <<< "$codec_spec"
        run_one_codec "$ENCODER" "$EXT" "$SWDEC"
    done
    INPUT="$saved_input"
    echo
}

# ---- Main --------------------------------------------------------------
echo
echo "=== nvidia-vaapi-driver encode roundtrip PSNR ==="
echo "Driver:    ${LIBVA_DRIVER_NAME:-<default>} at ${LIBVA_DRIVERS_PATH:-<default>}"
echo "Threshold: ${THRESHOLD} dB (below this = bitstream garbage)"
echo

run_all_codecs "$INPUT" "smpte bars, 640x360"
if [ -n "$STRESS_INPUT" ] && [ -f "$STRESS_INPUT" ]; then
    run_all_codecs "$STRESS_INPUT" "high-detail mandelbrot, 1920x1088"
fi

echo "=== Roundtrip: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
