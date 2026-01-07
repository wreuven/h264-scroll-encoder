#!/bin/bash
# H.264 Scroll Encoder - Full Pipeline Script
# Creates a scrolling animation from two reference images

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
mkdir -p test_input test_output

# Step 1: Build the encoder
echo "[1/5] Building encoder..."
make -j$(nproc) > /dev/null 2>&1
echo "  Done."

# Step 2: Create test images
echo "[2/5] Creating ${WIDTH}x${HEIGHT} test images..."
ffmpeg -y -f lavfi -i "color=c=${COLOR_A}:s=${WIDTH}x${HEIGHT}:d=1" -frames:v 1 -update 1 test_input/image_a.png 2>/dev/null
ffmpeg -y -f lavfi -i "color=c=${COLOR_B}:s=${WIDTH}x${HEIGHT}:d=1" -frames:v 1 -update 1 test_input/image_b.png 2>/dev/null
echo "  Created test_input/image_a.png (${COLOR_A})"
echo "  Created test_input/image_b.png (${COLOR_B})"

# Step 3: Encode reference I-frames
echo "[3/5] Encoding reference I-frames with x264..."
# Create concat file with absolute paths
cat > /tmp/frames_$$.txt << EOF
file '$(pwd)/test_input/image_a.png'
duration 0.04
file '$(pwd)/test_input/image_b.png'
duration 0.04
EOF

# Use large keyint so only first frame is IDR, scenecut ensures second is I-frame
ffmpeg -y -f concat -safe 0 -i /tmp/frames_$$.txt \
  -frames:v 2 -c:v libx264 -profile:v baseline -level 3.1 \
  -x264-params "keyint=10000:min-keyint=10000:scenecut=100:bframes=0:ref=2:cabac=0" \
  -g 10000 -pix_fmt yuv420p test_input/two_iframes.h264 2>/dev/null

rm -f /tmp/frames_$$.txt
echo "  Created test_input/two_iframes.h264 (2 I-frames)"

# Step 4: Generate scroll animation
echo "[4/5] Generating ${SCROLL_FRAMES} scroll P-frames..."
./h264_scroll_encoder -i test_input/two_iframes.h264 -o test_output/scroll.h264 \
  --width ${WIDTH} --height ${HEIGHT} --frames ${SCROLL_FRAMES} 2>&1 | grep -E "^(Generating|Output|Written)" || true

# Step 5: Create MP4 container
echo "[5/5] Creating MP4 container..."
ffmpeg -y -framerate ${FPS} -i test_output/scroll.h264 -c:v copy test_output/${OUTPUT} 2>/dev/null
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
