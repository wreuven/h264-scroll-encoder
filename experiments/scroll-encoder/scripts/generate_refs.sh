#!/bin/bash
# Generate reference frames A and B using x264/FFmpeg
# Output: setup.h264 containing SPS, PPS, IDR(A), non-IDR I-frame(B)
set -e

WIDTH=${1:-128}
HEIGHT=${2:-128}
OUTPUT=${3:-setup.h264}

echo "Generating reference frames: ${WIDTH}x${HEIGHT}"

# Create temporary directory
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Generate image A (dark gray)
ffmpeg -y -f lavfi -i "color=c=0x404040:s=${WIDTH}x${HEIGHT}" \
    -frames:v 1 -pix_fmt yuv420p "$TMPDIR/a.yuv" 2>/dev/null

# Generate image B (light gray)
ffmpeg -y -f lavfi -i "color=c=0xC0C0C0:s=${WIDTH}x${HEIGHT}" \
    -frames:v 1 -pix_fmt yuv420p "$TMPDIR/b.yuv" 2>/dev/null

# Concatenate into single YUV file (2 frames)
cat "$TMPDIR/a.yuv" "$TMPDIR/b.yuv" > "$TMPDIR/ab.yuv"

# Encode with x264 via FFmpeg
# Key settings:
# - baseline profile, CAVLC
# - 2 reference frames
# - keyint=250 with intra-refresh to avoid second IDR
# - force-cfr and refs=2 to keep both frames as references
ffmpeg -y -f rawvideo -pix_fmt yuv420p -s:v ${WIDTH}x${HEIGHT} -r 1 \
    -i "$TMPDIR/ab.yuv" \
    -c:v libx264 \
    -profile:v baseline \
    -level 4.0 \
    -g 250 \
    -bf 0 \
    -refs 2 \
    -sc_threshold 0 \
    -frames:v 2 \
    "$OUTPUT" 2>/dev/null

echo "Output: $OUTPUT"
ffprobe -show_frames -select_streams v "$OUTPUT" 2>/dev/null | grep -E "^(pict_type|key_frame)="
