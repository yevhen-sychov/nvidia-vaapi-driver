#!/bin/bash
set -u

export LIBVA_DRIVER_NAME=nvidia

# INPUT_FILE can be passed as argument, default to samples/input.mp4
INPUT_FILE=${1:-samples/input.mp4}

if [ ! -f "$INPUT_FILE" ]; then
    echo "Input file $INPUT_FILE not found"
    exit 1
fi

PASS=0
FAIL=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

pass() { printf "  %-55s \033[32mPASS\033[0m\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  %-55s \033[31mFAIL\033[0m (%s)\n" "$1" "$2"; FAIL=$((FAIL+1)); }

echo ""
echo "=== nvidia-vaapi-driver FFmpeg tests ==="
echo ""

# --- H.264 encode ---
OUT_H264="$TMPDIR/out_h264.mp4"
echo "Running H.264 encode..."
if ffmpeg -y -vaapi_device /dev/dri/renderD128 -i "$INPUT_FILE" \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -qp 23 -profile:v high \
  -c:a copy "$OUT_H264" > "$TMPDIR/ffmpeg_h264.log" 2>&1; then
    SIZE=$(stat -c%s "$OUT_H264" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 1000 ]; then
        pass "H.264 high profile conversion (encode)"
    else
        fail "H.264 high profile conversion (encode)" "file too small: ${SIZE} bytes"
    fi
else
    fail "H.264 high profile conversion (encode)" "ffmpeg error"
    cat "$TMPDIR/ffmpeg_h264.log"
fi

# --- HEVC encode ---
OUT_HEVC="$TMPDIR/out_hevc.mp4"
echo "Running HEVC encode..."
if ffmpeg -y -vaapi_device /dev/dri/renderD128 -i "$INPUT_FILE" \
  -vf 'format=nv12,hwupload' \
  -c:v hevc_vaapi -qp 25 -profile:v main \
  -c:a copy "$OUT_HEVC" > "$TMPDIR/ffmpeg_hevc.log" 2>&1; then
    SIZE=$(stat -c%s "$OUT_HEVC" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 1000 ]; then
        pass "HEVC main profile conversion (encode)"
    else
        fail "HEVC main profile conversion (encode)" "file too small: ${SIZE} bytes"
    fi
else
    fail "HEVC main profile conversion (encode)" "ffmpeg error"
    cat "$TMPDIR/ffmpeg_hevc.log"
fi

# --- AV1 encode ---
OUT_AV1="$TMPDIR/out_av1.mp4"
echo "Running AV1 encode..."
if ffmpeg -y -vaapi_device /dev/dri/renderD128 -i "$INPUT_FILE" \
  -vf 'format=nv12,hwupload' \
  -c:v av1_vaapi -qp 28 \
  -c:a copy "$OUT_AV1" > "$TMPDIR/ffmpeg_av1_enc.log" 2>&1; then
    SIZE=$(stat -c%s "$OUT_AV1" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 1000 ]; then
        pass "AV1 conversion (encode)"
    else
        fail "AV1 conversion (encode)" "file too small: ${SIZE} bytes"
    fi
else
    fail "AV1 conversion (encode)" "ffmpeg error"
    cat "$TMPDIR/ffmpeg_av1_enc.log"
fi

# --- H.264 decode ---
echo "Running H.264 decode..."
if ffmpeg -y -vaapi_device /dev/dri/renderD128 -hwaccel vaapi -i "$OUT_H264" \
  -f null - > "$TMPDIR/ffmpeg_h264_dec.log" 2>&1; then
    pass "H.264 decode"
else
    fail "H.264 decode" "ffmpeg error"
    cat "$TMPDIR/ffmpeg_h264_dec.log"
fi

# --- HEVC decode ---
echo "Running HEVC decode..."
if ffmpeg -y -vaapi_device /dev/dri/renderD128 -hwaccel vaapi -i "$OUT_HEVC" \
  -f null - > "$TMPDIR/ffmpeg_hevc_dec.log" 2>&1; then
    pass "HEVC decode"
else
    fail "HEVC decode" "ffmpeg error"
    cat "$TMPDIR/ffmpeg_hevc_dec.log"
fi

# --- AV1 decode ---
echo "Running AV1 decode..."
if ffmpeg -y -vaapi_device /dev/dri/renderD128 -hwaccel vaapi -i "$OUT_AV1" \
  -f null - > "$TMPDIR/ffmpeg_av1_dec.log" 2>&1; then
    pass "AV1 decode"
else
    fail "AV1 decode" "ffmpeg error"
    cat "$TMPDIR/ffmpeg_av1_dec.log"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
echo ""

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
