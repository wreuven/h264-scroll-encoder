#!/bin/bash
# H.264 Scroll Encoder - Full Pipeline Script
# Creates a scrolling animation from two colored reference frames
#
# Uses native I-frame generation (no x264 dependency for reference frames)
# This avoids max_frame_num compatibility issues with external encoders

set -e

# Configuration
WIDTH=${WIDTH:-1280}
HEIGHT=${HEIGHT:-720}
DURATION=${DURATION:-10}
FPS=${FPS:-25}
COLOR_A=${COLOR_A:-red}
COLOR_B=${COLOR_B:-blue}
OUTPUT=${OUTPUT:-scroll_output.mp4}

# Derived values
TOTAL_FRAMES=$((DURATION * FPS))
SCROLL_FRAMES=$((TOTAL_FRAMES - 2))  # Minus 2 I-frames

echo "=== H.264 Scroll Encoder ==="
echo "Resolution: ${WIDTH}x${HEIGHT}"
echo "Duration: ${DURATION}s at ${FPS}fps (${TOTAL_FRAMES} frames)"
echo "Colors: ${COLOR_A} -> ${COLOR_B}"
echo ""

# Create directories
mkdir -p test_output

# Step 1: Build the encoder
echo "[1/3] Building encoder..."
make -j$(nproc) > /dev/null 2>&1
echo "  Done."

# Step 2: Generate scroll animation with native I-frames
echo "[2/3] Generating ${TOTAL_FRAMES} frames (2 I-frames + ${SCROLL_FRAMES} P-frames)..."
./h264_scroll_encoder -t \
  --width ${WIDTH} --height ${HEIGHT} \
  --frames ${SCROLL_FRAMES} \
  --color-a ${COLOR_A} --color-b ${COLOR_B} \
  -o test_output/scroll.h264

# Step 3: Create MP4 container
echo "[3/3] Creating MP4 container..."
ffmpeg -y -fflags +genpts -r ${FPS} -i test_output/scroll.h264 -c:v copy test_output/${OUTPUT} 2>/dev/null
FILESIZE=$(du -h test_output/${OUTPUT} | cut -f1)
echo "  Created test_output/${OUTPUT} (${FILESIZE})"

# Verify
echo ""
echo "=== Verification ==="
ffmpeg -i test_output/${OUTPUT} -f null - 2>&1 | grep -E "^frame=" | tail -1

echo ""
echo "=== Done! ==="
echo "Output: test_output/${OUTPUT}"
echo ""
echo "To play: ffplay test_output/${OUTPUT}"
echo "Or open: xdg-open test_output/${OUTPUT}"
