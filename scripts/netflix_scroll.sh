#!/bin/bash
# Netflix Scroll Encoder - Full Pipeline Script
# Creates a smooth scrolling animation from two PNG images
#
# Usage: ./scripts/netflix_scroll.sh [image_a.png] [image_b.png] [output.mp4]
#
# IMPORTANT: Uses baseline profile encoding to ensure CAVLC compatibility
# with our generated SPS (profile_idc=66)

set -e

cd "$(dirname "$0")/.."

# Input images (default to Netflix frames if they exist)
IMAGE_A=${1:-netflix_frame_a.png}
IMAGE_B=${2:-netflix_frame_b.png}
OUTPUT=${3:-netflix_scroll_browser.mp4}

# Configuration
FPS=30
DURATION=30  # seconds
SCROLL_FRAMES=$((DURATION * FPS))  # Number of P-frames for scrolling animation

echo "=== Netflix Scroll Encoder ==="
echo "Input A: $IMAGE_A"
echo "Input B: $IMAGE_B"
echo "Output:  $OUTPUT"
echo ""

# Check inputs exist
if [[ ! -f "$IMAGE_A" ]]; then
    echo "ERROR: Image A not found: $IMAGE_A"
    exit 1
fi
if [[ ! -f "$IMAGE_B" ]]; then
    echo "ERROR: Image B not found: $IMAGE_B"
    exit 1
fi

# Get dimensions from first image
DIMS=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$IMAGE_A")
WIDTH=$(echo $DIMS | cut -d',' -f1)
HEIGHT=$(echo $DIMS | cut -d',' -f2)
echo "Resolution: ${WIDTH}x${HEIGHT}"
echo ""

# Create temp directory
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Step 1: Convert PNGs to YUV420
echo "[1/5] Converting images to YUV420..."
ffmpeg -y -i "$IMAGE_A" -pix_fmt yuv420p -f rawvideo "$TMPDIR/a.yuv" 2>/dev/null
ffmpeg -y -i "$IMAGE_B" -pix_fmt yuv420p -f rawvideo "$TMPDIR/b.yuv" 2>/dev/null
cat "$TMPDIR/a.yuv" "$TMPDIR/b.yuv" > "$TMPDIR/both.yuv"
echo "  Done."

# Step 2: Encode with x264 using BASELINE PROFILE (CAVLC, no 8x8dct)
# This is critical - our SPS uses profile_idc=66 (Baseline) which means CAVLC
# If x264 encodes with CABAC (High profile), the decoder will fail
echo "[2/5] Encoding reference frames with x264 (Baseline profile)..."
ffmpeg -y -f rawvideo -pix_fmt yuv420p -s ${WIDTH}x${HEIGHT} -framerate $FPS \
    -i "$TMPDIR/both.yuv" \
    -c:v libx264 \
    -profile:v baseline \
    -g 1 \
    -bf 0 \
    -f h264 \
    "$TMPDIR/two_frames.h264" 2>/dev/null

# Verify baseline profile was used
if ffmpeg -i "$TMPDIR/two_frames.h264" 2>&1 | grep -q "cabac=1"; then
    echo "  WARNING: x264 used CABAC encoding - this may cause decoding issues!"
else
    echo "  Confirmed: Baseline profile with CAVLC"
fi

# Step 3: Build encoder if needed
echo "[3/5] Building encoder..."
make -j$(nproc) > /dev/null 2>&1
echo "  Done."

# Step 4: Generate scroll animation
echo "[4/5] Generating scroll animation (${SCROLL_FRAMES} P-frames)..."
./h264_scroll_encoder -i "$TMPDIR/two_frames.h264" -o "$TMPDIR/scroll.h264" -n $SCROLL_FRAMES

# Step 5: Create browser-compatible MP4
echo "[5/5] Creating MP4 container..."
ffmpeg -y -fflags +genpts -r $FPS -i "$TMPDIR/scroll.h264" \
    -c:v copy \
    -movflags +faststart \
    "$OUTPUT" 2>/dev/null

FILESIZE=$(du -h "$OUTPUT" | cut -f1)
FRAME_COUNT=$(ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=nb_read_frames -of default=nokey=1:noprint_wrappers=1 "$OUTPUT")
echo "  Created: $OUTPUT"
echo "  Size: $FILESIZE"
echo "  Frames: $FRAME_COUNT"

# Verify decode
echo ""
echo "=== Verification ==="
ERRORS=$(ffmpeg -i "$OUTPUT" -f null - 2>&1 | grep -c "error" || true)
if [[ "$ERRORS" -eq 0 ]]; then
    echo "  Decode: SUCCESS (no errors)"
else
    echo "  Decode: WARNING ($ERRORS errors found)"
fi

echo ""
echo "=== Done! ==="
echo "Play: ffplay $OUTPUT"
echo "Open: xdg-open $OUTPUT"
