#!/bin/bash
#
# Generate two-color reference frames for Composer v0.1
#
# RefA: top half red, bottom half blue
# RefB: top half green, bottom half yellow
#
# Usage: ./scripts/generate_refs.sh [width] [height]
#        Default: 1280x720
#

set -e

WIDTH=${1:-1280}
HEIGHT=${2:-720}
HALF_HEIGHT=$((HEIGHT / 2))

echo "Generating reference frames: ${WIDTH}x${HEIGHT}"

# RefA: top red, bottom blue
echo "Creating ref_a.h264 (red/blue)..."
ffmpeg -y -loglevel error \
    -f lavfi -i "color=c=red:s=${WIDTH}x${HALF_HEIGHT}:d=1,format=yuv420p" \
    -f lavfi -i "color=c=blue:s=${WIDTH}x${HALF_HEIGHT}:d=1,format=yuv420p" \
    -filter_complex "[0][1]vstack=inputs=2" \
    -frames:v 1 \
    -c:v libx264 -profile:v baseline \
    -x264-params "keyint=1:bframes=0:ref=1" \
    ref_a.h264

# RefB: top green, bottom yellow
echo "Creating ref_b.h264 (green/yellow)..."
ffmpeg -y -loglevel error \
    -f lavfi -i "color=c=green:s=${WIDTH}x${HALF_HEIGHT}:d=1,format=yuv420p" \
    -f lavfi -i "color=c=yellow:s=${WIDTH}x${HALF_HEIGHT}:d=1,format=yuv420p" \
    -filter_complex "[0][1]vstack=inputs=2" \
    -frames:v 1 \
    -c:v libx264 -profile:v baseline \
    -x264-params "keyint=1:bframes=0:ref=1" \
    ref_b.h264

echo ""
echo "Generated:"
ls -la ref_a.h264 ref_b.h264
echo ""
echo "Visual verification:"
echo "  ref_a.h264: top=RED, bottom=BLUE"
echo "  ref_b.h264: top=GREEN, bottom=YELLOW"
echo ""
echo "To preview:"
echo "  ffplay ref_a.h264"
echo "  ffplay ref_b.h264"
