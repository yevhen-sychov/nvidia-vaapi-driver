#!/bin/sh
#
# Generate synthetic sample media files used by tests/test_ffmpeg.sh and for
# manual smoke-testing. Outputs land next to this script, in samples/, no
# matter where the script is invoked from -- previously it wrote into $PWD
# and polluted the repo root when run as `./samples/gensamples.sh`.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

FFMPEG=${FFMPEG:-ffmpeg}
LAVFI="-f lavfi -i smptebars=duration=10:size=640x360:rate=30"

# Baseline H.264 fixture: tests/test_ffmpeg.sh uses this as its default input.
# Keep it as the first line so a partial gensamples run still produces the
# file the meson `ffmpeg` test needs.
$FFMPEG -y $LAVFI smptebars_h264.mp4

$FFMPEG -y $LAVFI -c:v libx265 -crf 26 -preset fast smptebars_hevc_8bit.mp4
$FFMPEG -y $LAVFI -c:v libx265 -crf 26 -preset fast -pix_fmt yuv420p10le smptebars_hevc_10bit.mp4
$FFMPEG -y $LAVFI -c:v libx265 -crf 26 -preset fast -pix_fmt yuv420p12le smptebars_hevc_12bit.mp4
$FFMPEG -y $LAVFI -c:v libx265 -crf 26 -preset fast -pix_fmt yuv422 smptebars_hevc_422_8bit.mp4
$FFMPEG -y $LAVFI -c:v libx265 -crf 26 -preset fast -pix_fmt yuv422p10le smptebars_hevc_422_10bit.mp4
$FFMPEG -y $LAVFI -c:v libx265 -crf 26 -preset fast -pix_fmt yuv422p12le smptebars_hevc_422_12bit.mp4
$FFMPEG -y $LAVFI -c:v mpeg4 smptebars_mpeg4.mp4
$FFMPEG -y $LAVFI -c:v vp9 smptebars_vp9.mp4
# this one didn't work on all ffmpeg builds, complained about a mismatched ABI
#$FFMPEG -y $LAVFI -c:v av1 smptebars_av1.mp4
$FFMPEG -y $LAVFI -c:v libsvtav1 smptebars_av1.mp4

# TODO need mpeg2, vc1, vp8, perhaps h264 with different levels
