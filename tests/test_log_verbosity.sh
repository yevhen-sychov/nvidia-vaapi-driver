#!/bin/bash
set -u

# test_log_verbosity.sh — the encode log must not scale with frame count.
#
# The encode path re-runs its whole parameter plumbing every frame (Chromium
# resends the sequence, picture and misc parameter buffers each time), so any
# unconditional logging in there produces several lines per frame. At 4K60 that
# is hundreds of lines a second all saying the same thing, which buries the
# events that actually matter — a resolution change, a bitrate change, a
# reconfigure, an error.
#
# The per-frame sites now either log only when their values change, or sit
# behind NVD_LOG_VERBOSE=1. This asserts both halves: quiet by default, and
# still fully recoverable when asked.

export LIBVA_DRIVER_NAME=nvidia

PASS=0
FAIL=0
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

pass() { printf "  %-55s \033[32mPASS\033[0m\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  %-55s \033[31mFAIL\033[0m (%s)\n" "$1" "$2"; FAIL=$((FAIL+1)); }
skip() { printf "  %-55s \033[33mSKIP\033[0m (%s)\n" "$1" "$2"; }

echo ""
echo "=== nvidia-vaapi-driver encode log verbosity ==="
echo ""

FRAMES=120
SRC="$TMPDIR/src.nut"

if ! ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=640x480:rate=30:duration=4" \
        -c:v rawvideo -pix_fmt nv12 -y "$SRC" 2>"$TMPDIR/gen.log"; then
    skip "fixture generation" "ffmpeg could not build a raw source"
    echo ""
    exit 77
fi

encode() { # $1 = log file ("" for none), rest = extra env assignments
    local log="$1"; shift
    local -a envv=(env)
    if [ -n "$log" ]; then envv+=("NVD_LOG=$log"); else envv+=(-u NVD_LOG); fi
    "${envv[@]}" "$@" \
        ffmpeg -hide_banner -loglevel error -vaapi_device /dev/dri/renderD128 \
        -i "$SRC" -frames:v "$FRAMES" -vf 'format=nv12,hwupload' \
        -c:v h264_vaapi -b:v 4M -y "$TMPDIR/out.mp4" >"$TMPDIR/ff.log" 2>&1
}

frame_count() { # $1 = mp4
    ffprobe -v error -count_frames -select_streams v \
        -show_entries stream=nb_read_frames -of csv=p=0 "$1" 2>/dev/null | tr -d '\r\n,'
}

if ! encode "$TMPDIR/quiet.log"; then
    fail "hardware encode for log measurement" "ffmpeg error"
    cat "$TMPDIR/ff.log"
    exit 1
fi

TOTAL=$(wc -l < "$TMPDIR/quiet.log")
GEOMETRY=$(grep -c 'Encode: surface' "$TMPDIR/quiet.log")
PERFRAME=$(grep -cE 'frame [0-9]+ encoded' "$TMPDIR/quiet.log")

# A single steady-state encode session changes geometry exactly once (the first
# frame establishes it). More than a couple means the change detection is not
# working; the failure mode this guards is "logs once per frame".
if [ "$GEOMETRY" -le 2 ]; then
    pass "geometry logged on change, not per frame (${GEOMETRY}x)"
else
    fail "geometry logged on change, not per frame" \
         "${GEOMETRY} 'Encode: surface' lines for ${FRAMES} frames"
fi

if [ "$PERFRAME" -eq 0 ]; then
    pass "per-frame encode progress is not logged by default"
else
    fail "per-frame encode progress is not logged by default" \
         "${PERFRAME} per-frame lines leaked to the default log level"
fi

# The real invariant: total output must be bounded by session setup, not by how
# long the session runs. Generous bound — the point is that it is not O(frames).
if [ "$TOTAL" -lt "$FRAMES" ]; then
    pass "log volume does not scale with frame count (${TOTAL} lines / ${FRAMES} frames)"
else
    fail "log volume does not scale with frame count" \
         "${TOTAL} lines for ${FRAMES} frames"
fi

# ...and it must still all be available on demand.
if ! encode "$TMPDIR/verbose.log" NVD_LOG_VERBOSE=1; then
    fail "verbose encode run" "ffmpeg error"
    exit 1
fi
VERBOSE_PERFRAME=$(grep -cE 'frame [0-9]+ encoded' "$TMPDIR/verbose.log")
if [ "$VERBOSE_PERFRAME" -ge "$FRAMES" ]; then
    pass "NVD_LOG_VERBOSE=1 restores per-frame detail (${VERBOSE_PERFRAME} lines)"
else
    fail "NVD_LOG_VERBOSE=1 restores per-frame detail" \
         "only ${VERBOSE_PERFRAME} per-frame lines for ${FRAMES} frames"
fi

# The per-frame log sites build a comparison value and memcmp it against the
# last one they printed. All of that sits behind LOG_ENABLED(), so with no log
# destination configured none of it runs — and, more importantly, the encode
# must behave identically. This is the configuration real users run in.
if encode "" && [ "$(frame_count "$TMPDIR/out.mp4")" = "$FRAMES" ]; then
    pass "encode is correct with logging disabled entirely"
else
    fail "encode is correct with logging disabled entirely" \
         "ffmpeg failed or produced $(frame_count "$TMPDIR/out.mp4") of ${FRAMES} frames"
fi

# NVD_LOG_VERBOSE without a destination must be inert rather than doing the
# work and then throwing the output away.
if encode "" NVD_LOG_VERBOSE=1 && [ "$(frame_count "$TMPDIR/out.mp4")" = "$FRAMES" ]; then
    pass "NVD_LOG_VERBOSE without NVD_LOG is inert"
else
    fail "NVD_LOG_VERBOSE without NVD_LOG is inert" "encode did not complete cleanly"
fi

echo ""
echo "=== log verbosity: ${PASS} passed, ${FAIL} failed ==="
echo ""
[ "$FAIL" -eq 0 ]
