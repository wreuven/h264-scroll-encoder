#!/bin/bash
# Test script for H.264 scroll encoder
set -e

cd "$(dirname "$0")/.."

WIDTH=640
HEIGHT=480
FRAMES=60

echo "=== H.264 Scroll Encoder Test ==="
echo ""

# Check for FFmpeg
if ! command -v ffmpeg &> /dev/null; then
    echo "ERROR: ffmpeg is required but not installed"
    exit 1
fi

# Create test directory
mkdir -p test_output

echo "Step 1: Generate two test images using FFmpeg..."

# Generate image A: Red gradient
ffmpeg -y -f lavfi -i "color=c=red:s=${WIDTH}x${HEIGHT}:d=1,format=yuv420p" \
    -vf "drawtext=text='IMAGE A':fontsize=72:fontcolor=white:x=(w-tw)/2:y=(h-th)/2" \
    -frames:v 1 test_output/img_a.png 2>/dev/null

# Generate image B: Blue gradient
ffmpeg -y -f lavfi -i "color=c=blue:s=${WIDTH}x${HEIGHT}:d=1,format=yuv420p" \
    -vf "drawtext=text='IMAGE B':fontsize=72:fontcolor=white:x=(w-tw)/2:y=(h-th)/2" \
    -frames:v 1 test_output/img_b.png 2>/dev/null

echo "  Created test_output/img_a.png and test_output/img_b.png"

echo ""
echo "Step 2: Encode both images as I-frames using x264 (via FFmpeg)..."

# Create setup.h264 with exactly 2 I-frames
# We use -g 1 to force every frame to be an I-frame
# We use baseline profile and CAVLC
ffmpeg -y \
    -loop 1 -i test_output/img_a.png -t 0.1 \
    -loop 1 -i test_output/img_b.png -t 0.1 \
    -filter_complex "[0:v][1:v]concat=n=2:v=1:a=0[out]" \
    -map "[out]" \
    -c:v libx264 \
    -profile:v baseline \
    -level 4.0 \
    -preset ultrafast \
    -g 1 \
    -bf 0 \
    -refs 2 \
    -crf 18 \
    -pix_fmt yuv420p \
    -frames:v 2 \
    test_output/setup.h264 2>/dev/null

echo "  Created test_output/setup.h264"

# Show info about the setup file
echo ""
echo "Step 3: Analyze setup.h264..."
ffprobe -show_frames -select_streams v test_output/setup.h264 2>/dev/null | grep -E "^(pict_type|key_frame)=" | head -8

echo ""
echo "Step 4: Run our scroll encoder..."
./h264_scroll_encoder -i test_output/setup.h264 -o test_output/scroll.h264 -n $FRAMES

echo ""
echo "Step 5: Verify output with FFmpeg..."
if ffprobe -v error test_output/scroll.h264 2>&1; then
    echo "  FFprobe: stream is valid"
else
    echo "  FFprobe: ERROR - stream may be invalid"
fi

# Try to decode
echo ""
echo "Step 6: Decode output to verify..."
if ffmpeg -y -i test_output/scroll.h264 -f null - 2>&1 | tail -5; then
    echo "  Decode: SUCCESS"
else
    echo "  Decode: FAILED"
fi

echo ""
echo "Step 7: Extract frames for visual inspection..."
ffmpeg -y -i test_output/scroll.h264 -vf "select=eq(n\,0)+eq(n\,15)+eq(n\,30)+eq(n\,45)" \
    -vsync vfr test_output/frame_%02d.png 2>/dev/null || echo "  (frame extraction may have issues)"

echo ""
echo "=== Test Complete ==="
echo "Output files in test_output/"
ls -la test_output/
