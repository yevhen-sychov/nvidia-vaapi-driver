#!/bin/bash
#
# test_gstreamer.sh — GStreamer VA-API encode integration tests
#
# Requires: gstreamer1-vaapi (Fedora) or gstreamer1.0-vaapi (Ubuntu)
#
# Exit code: 0 = all pass, 1 = failure

set -u

export GST_VAAPI_ALL_DRIVERS=1
export LIBVA_DRIVER_NAME=nvidia

# Ensure we can find the driver if not already in LIBVA_DRIVERS_PATH
if [ -z "${LIBVA_DRIVERS_PATH:-}" ]; then
    if [ -f "./nvidia_drv_video.so" ]; then
        export LIBVA_DRIVERS_PATH=$(pwd)
    elif [ -f "../nvidia_drv_video.so" ]; then
        export LIBVA_DRIVERS_PATH=$(pwd)/..
    elif [ -f "./build/nvidia_drv_video.so" ]; then
        export LIBVA_DRIVERS_PATH=$(pwd)/build
    fi
fi

PASS=0
FAIL=0
SKIP=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

pass() { printf "  %-55s \033[32mPASS\033[0m\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  %-55s \033[31mFAIL\033[0m (%s)\n" "$1" "$2"; FAIL=$((FAIL+1)); }
skip() { printf "  %-55s \033[33mSKIP\033[0m (%s)\n" "$1" "$2"; SKIP=$((SKIP+1)); }

has_element() { gst-inspect-1.0 "$1" >/dev/null 2>&1; }

echo ""
echo "=== nvidia-vaapi-driver GStreamer tests ==="
echo ""

# Detect available plugin and elements
H264_ENC=""; H265_ENC=""; AV1_ENC=""; H264_DEC=""; H265_DEC=""; POSTPROC=""
PLUGIN=""

if has_element vaapih264enc; then
    H264_ENC="vaapih264enc"; H265_ENC="vaapih265enc"; AV1_ENC="vaapiav1enc";
    H264_DEC="vaapih264dec"; H265_DEC="vaapih265dec"; POSTPROC="vaapipostproc"
    PLUGIN="gstreamer-vaapi"
elif has_element vah264enc; then
    H264_ENC="vah264enc"; H265_ENC="vah265enc"; AV1_ENC="vaav1enc";
    H264_DEC="vah264dec"; H265_DEC="vah265dec"; POSTPROC="vapostproc"
    PLUGIN="va"
fi

echo "Prerequisites:"

if [ -z "$H264_ENC" ]; then
    skip "H.264 encoder available" "Neither gstreamer-vaapi nor va plugin found"
    echo ""
    echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
    exit 77
fi
pass "H.264 encoder available ($H264_ENC via $PLUGIN)"

if ! has_element "$H265_ENC"; then
    skip "HEVC encoder available" "element $H265_ENC not found"
else
    pass "HEVC encoder available ($H265_ENC)"
fi

# --- H.264 encode tests ---

echo ""
echo "H.264 Encode:"

# Basic encode to fakesink
if gst-launch-1.0 -e videotestsrc num-buffers=30 \
    ! video/x-raw,width=320,height=240,framerate=30/1 \
    ! $H264_ENC ! h264parse ! fakesink 2>&1 | grep -q "EOS"; then
    pass "H.264 320x240 30 frames → fakesink"
else
    fail "H.264 320x240 30 frames → fakesink" "pipeline error"
fi

# Encode to file and validate
OUT="$TMPDIR/h264.mp4"
if gst-launch-1.0 -e videotestsrc num-buffers=60 \
    ! video/x-raw,width=1920,height=1080,framerate=30/1 \
    ! $H264_ENC bitrate=5000 ! h264parse \
    ! mp4mux ! filesink location="$OUT" 2>&1 | grep -q "EOS"; then
    SIZE=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 1000 ]; then
        pass "H.264 1080p 60 frames → mp4 (${SIZE} bytes)"
    else
        fail "H.264 1080p 60 frames → mp4" "file too small: ${SIZE} bytes"
    fi
else
    fail "H.264 1080p 60 frames → mp4" "pipeline error"
fi

# CBR bitrate control
OUT="$TMPDIR/h264_cbr.mp4"
if gst-launch-1.0 -e videotestsrc num-buffers=90 \
    ! video/x-raw,width=1280,height=720,framerate=30/1 \
    ! $H264_ENC rate-control=cbr bitrate=2000 ! h264parse \
    ! mp4mux ! filesink location="$OUT" 2>&1 | grep -q "EOS"; then
    SIZE=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 1000 ]; then
        pass "H.264 720p CBR 2Mbps 90 frames"
    else
        fail "H.264 720p CBR 2Mbps 90 frames" "file too small"
    fi
else
    fail "H.264 720p CBR 2Mbps 90 frames" "pipeline error"
fi

# Small resolution (GStreamer vaapi requires ~256x256 minimum)
if gst-launch-1.0 -e videotestsrc num-buffers=10 \
    ! video/x-raw,width=256,height=256,framerate=30/1 \
    ! $H264_ENC ! h264parse ! fakesink 2>&1 | grep -q "EOS"; then
    pass "H.264 256x256 small resolution"
else
    fail "H.264 256x256 small resolution" "pipeline error"
fi

# 4K resolution
if gst-launch-1.0 -e videotestsrc num-buffers=5 \
    ! video/x-raw,width=3840,height=2160,framerate=30/1 \
    ! $H264_ENC ! h264parse ! fakesink 2>&1 | grep -q "EOS"; then
    pass "H.264 4K 5 frames"
else
    fail "H.264 4K 5 frames" "pipeline error"
fi

# --- HEVC encode tests ---

echo ""
echo "HEVC Encode:"

if has_element "$H265_ENC"; then
    # Basic encode
    if gst-launch-1.0 -e videotestsrc num-buffers=30 \
        ! video/x-raw,width=320,height=240,framerate=30/1 \
        ! $H265_ENC ! h265parse ! fakesink 2>&1 | grep -q "EOS"; then
        pass "HEVC 320x240 30 frames → fakesink"
    else
        fail "HEVC 320x240 30 frames → fakesink" "pipeline error"
    fi

    # Encode to file
    OUT="$TMPDIR/hevc.mp4"
    if gst-launch-1.0 -e videotestsrc num-buffers=60 \
        ! video/x-raw,width=1920,height=1080,framerate=30/1 \
        ! $H265_ENC bitrate=5000 ! h265parse \
        ! mp4mux ! filesink location="$OUT" 2>&1 | grep -q "EOS"; then
        SIZE=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
        if [ "$SIZE" -gt 1000 ]; then
            pass "HEVC 1080p 60 frames → mp4 (${SIZE} bytes)"
        else
            fail "HEVC 1080p 60 frames → mp4" "file too small: ${SIZE} bytes"
        fi
    else
        fail "HEVC 1080p 60 frames → mp4" "pipeline error"
    fi

    # 4K
    if gst-launch-1.0 -e videotestsrc num-buffers=5 \
        ! video/x-raw,width=3840,height=2160,framerate=30/1 \
        ! $H265_ENC ! h265parse ! fakesink 2>&1 | grep -q "EOS"; then
        pass "HEVC 4K 5 frames"
    else
        fail "HEVC 4K 5 frames" "pipeline error"
    fi
else
    skip "HEVC tests" "$H265_ENC not available"
fi

# --- AV1 encode tests ---

echo ""
echo "AV1 Encode:"

if [ -n "$AV1_ENC" ] && has_element "$AV1_ENC"; then
    # Basic encode
    if gst-launch-1.0 -e videotestsrc num-buffers=30 \
        ! video/x-raw,width=320,height=240,framerate=30/1 \
        ! $AV1_ENC ! av1parse ! fakesink 2>&1 | grep -q "EOS"; then
        pass "AV1 320x240 30 frames → fakesink (using $AV1_ENC)"
    else
        fail "AV1 320x240 30 frames → fakesink" "pipeline error"
    fi

    # Encode to file
    OUT="$TMPDIR/av1.mp4"
    if gst-launch-1.0 -e videotestsrc num-buffers=60 \
        ! video/x-raw,width=1920,height=1080,framerate=30/1 \
        ! $AV1_ENC ! av1parse \
        ! mp4mux ! filesink location="$OUT" 2>&1 | grep -q "EOS"; then
        SIZE=$(stat -c%s "$OUT" 2>/dev/null || echo 0)
        if [ "$SIZE" -gt 1000 ]; then
            pass "AV1 1080p 60 frames → mp4 (${SIZE} bytes)"
        else
            fail "AV1 1080p 60 frames → mp4" "file too small: ${SIZE} bytes"
        fi
    else
        fail "AV1 1080p 60 frames → mp4" "pipeline error"
    fi
else
    skip "AV1 tests" "AV1 encoder not available"
fi

# --- Decode regression ---

echo ""
echo "Decode regression:"

if has_element "$H264_DEC"; then
    pass "$H264_DEC available"
else
    fail "$H264_DEC available" "element missing"
fi

if has_element "$H265_DEC"; then
    pass "$H265_DEC available"
else
    fail "$H265_DEC available" "element missing"
fi

# Decode an encoded file (round-trip)
if [ -f "$TMPDIR/h264.mp4" ]; then
    if gst-launch-1.0 -e filesrc location="$TMPDIR/h264.mp4" \
        ! qtdemux ! h264parse ! $H264_DEC ! fakesink 2>&1 | grep -q "EOS"; then
        pass "H.264 encode → decode round-trip"
    else
        fail "H.264 encode → decode round-trip" "decode pipeline error"
    fi
fi

# --- Stress ---

echo ""
echo "Stress:"

# Sequential pipeline restarts (leak check)
ALL_OK=1
for i in $(seq 1 10); do
    if ! gst-launch-1.0 -e videotestsrc num-buffers=10 \
        ! video/x-raw,width=320,height=240,framerate=30/1 \
        ! $H264_ENC ! fakesink 2>&1 | grep -q "EOS"; then
        ALL_OK=0
        break
    fi
done
if [ "$ALL_OK" = "1" ]; then
    pass "10 sequential H.264 pipeline restarts"
else
    fail "10 sequential H.264 pipeline restarts" "failed at iteration $i"
fi

# Long encode (300 frames)
if gst-launch-1.0 -e videotestsrc num-buffers=300 \
    ! video/x-raw,width=1920,height=1080,framerate=60/1 \
    ! $H264_ENC bitrate=8000 ! h264parse ! fakesink 2>&1 | grep -q "EOS"; then
    pass "H.264 1080p60 300 frames sustained"
else
    fail "H.264 1080p60 300 frames sustained" "pipeline error"
fi

# --- Summary ---

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
echo ""
exit $FAIL
