#!/bin/bash
set -u

# test_vp8_decode.sh — VP8 hardware decode correctness.
#
# VP8 is the one decode path where VA-API does not hand the driver everything
# NVDEC needs: the slice data buffer starts at the first partition, *after* the
# VP8 "uncompressed data chunk" (the 3-byte frame tag, plus 7 more bytes of sync
# code and dimensions on a keyframe). NVDEC still wants that chunk at the head of
# the bitstream it is given.
#
# Upstream recovered it by rewinding the client's slice-data pointer to the
# previous 16-byte boundary (`ptr & 0xf`) and hoping the header happened to sit
# there. That is a read behind a buffer we do not own, and the rewind distance
# comes from pointer alignment rather than from the format, so it lands on the
# real header only by luck — with FFmpeg it overshot by 4 bytes, the keyframe
# sync-code check then failed, and every frame decoded to garbage (~8.7 dB).
#
# The driver now synthesizes the chunk from the VA-API parameters it is actually
# given. This test pins that: decode the same VP8 stream through the driver and
# through FFmpeg's software decoder and require them to agree. A correct
# hardware decode is bit-exact with the software one, so anything below a very
# high PSNR means the bitstream we handed NVDEC was malformed.

export LIBVA_DRIVER_NAME=nvidia

# VP8 has no checked-in fixture (samples/gensamples.sh does not emit one), so
# generate it here — libvpx ships with essentially every ffmpeg build.
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

pass() { printf "  %-55s \033[32mPASS\033[0m\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  %-55s \033[31mFAIL\033[0m (%s)\n" "$1" "$2"; FAIL=$((FAIL+1)); }
skip() { printf "  %-55s \033[33mSKIP\033[0m (%s)\n" "$1" "$2"; }

echo ""
echo "=== nvidia-vaapi-driver VP8 decode tests ==="
echo ""

WIDTH=640
HEIGHT=480
FRAMES=20
SRC="$TMPDIR/vp8.webm"

if ! ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=${WIDTH}x${HEIGHT}:rate=30:duration=2" \
        -c:v libvpx -b:v 1M -y "$SRC" 2>"$TMPDIR/gen.log"; then
    skip "VP8 fixture generation" "no libvpx encoder in this ffmpeg"
    echo ""
    echo "=== VP8 decode tests: 0 passed, 0 failed, 1 skipped ==="
    exit 77
fi

# Software reference.
if ! ffmpeg -hide_banner -loglevel error -i "$SRC" -frames:v "$FRAMES" \
        -pix_fmt yuv420p -y "$TMPDIR/sw.yuv" 2>"$TMPDIR/sw.log"; then
    fail "VP8 software reference decode" "ffmpeg error"
    cat "$TMPDIR/sw.log"
    exit 1
fi

# Hardware decode through the driver under test.
if ! ffmpeg -hide_banner -loglevel error \
        -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
        -hwaccel_output_format vaapi -i "$SRC" -frames:v "$FRAMES" \
        -vf 'hwdownload,format=nv12' -pix_fmt yuv420p \
        -y "$TMPDIR/hw.yuv" 2>"$TMPDIR/hw.log"; then
    fail "VP8 hardware decode" "ffmpeg error"
    cat "$TMPDIR/hw.log"
    exit 1
fi

SW_SIZE=$(stat -c%s "$TMPDIR/sw.yuv" 2>/dev/null || echo 0)
HW_SIZE=$(stat -c%s "$TMPDIR/hw.yuv" 2>/dev/null || echo 0)
if [ "$HW_SIZE" -eq 0 ] || [ "$HW_SIZE" -ne "$SW_SIZE" ]; then
    fail "VP8 hardware decode frame count" "hw ${HW_SIZE}B vs sw ${SW_SIZE}B"
else
    pass "VP8 hardware decode frame count matches software"
fi

PSNR=$(ffmpeg -hide_banner \
        -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -i "$TMPDIR/hw.yuv" \
        -s "${WIDTH}x${HEIGHT}" -pix_fmt yuv420p -i "$TMPDIR/sw.yuv" \
        -lavfi psnr -f null - 2>&1 | grep -oP 'average:\K[0-9.]+|average:\Kinf' | tail -1)

if [ -z "${PSNR:-}" ]; then
    fail "VP8 decode matches software reference" "could not measure PSNR"
elif [ "$PSNR" = "inf" ]; then
    pass "VP8 decode matches software reference (bit-exact)"
else
    # A correct VP8 hardware decode is bit-exact with libvpx. Allow a little
    # headroom for the NV12->YUV420P chroma round-trip, but a malformed
    # bitstream lands around 8-10 dB, nowhere near this.
    if awk "BEGIN{exit !($PSNR >= 40.0)}"; then
        pass "VP8 decode matches software reference (${PSNR} dB)"
    else
        fail "VP8 decode matches software reference" \
             "PSNR=${PSNR} dB < 40.0 dB -> malformed bitstream handed to NVDEC"
    fi
fi

echo ""
echo "=== VP8 decode tests: ${PASS} passed, ${FAIL} failed ==="
echo ""
[ "$FAIL" -eq 0 ]
