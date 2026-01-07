# H.264 Scroll Encoder

A specialized H.264 encoder that generates efficient scrolling animations between two reference images using only motion vectors - no per-frame pixel encoding required.

## Overview

This encoder demonstrates a technique for creating smooth scrolling transitions in H.264 video with minimal bitrate. It works by:

1. Encoding two reference frames (A and B) as I-frames
2. Generating P-frames that compose the output by referencing different regions of A and B using motion vectors
3. Using long-term reference pictures to avoid frame_num wrapping issues

The result is an extremely efficient video where scroll frames require only motion vector data, not actual pixel data.

## How It Works

```
Frame A (Red)     Frame B (Blue)      Output (Scrolling)
+------------+    +------------+      +------------+
|            |    |            |      |   Frame A  | <- MV references A
|   RED      |    |   BLUE     |  =>  +------------+
|            |    |            |      |   Frame B  | <- MV references B
+------------+    +------------+      +------------+
```

Each P-frame uses P16x16 macroblocks with motion vectors pointing to either reference A or B, creating a scrolling wipe effect. The scroll position determines which reference each macroblock row uses.

## Technical Details

- **Profile**: H.264 Baseline (CAVLC entropy coding)
- **Reference Management**: Long-term reference pictures (immune to frame_num wrapping)
- **Macroblock Type**: P_L0_16x16 with no residual (CBP=0)
- **Motion Vectors**: Full-pel precision, vertical scroll only

## Building

```bash
make
```

Requirements:
- GCC or compatible C compiler
- Make

## Usage

### Quick Start

```bash
./run.sh
```

This generates a 10-second 1280x720 scrolling animation between red and blue.

### Custom Parameters

```bash
# Environment variables
WIDTH=1920 HEIGHT=1080 DURATION=30 COLOR_A=green COLOR_B=yellow ./run.sh
```

### Direct Usage

```bash
# Test mode with built-in colors
./h264_scroll_encoder -t --width 1280 --height 720 --frames 248 \
    --color-a red --color-b blue -o output.h264

# Wrap in MP4 container
ffmpeg -fflags +genpts -r 25 -i output.h264 -c:v copy output.mp4
```

### Available Colors

red, blue, green, yellow, cyan, magenta, white, black, gray

### Command Line Options

| Option | Description |
|--------|-------------|
| `-t, --test` | Test mode (generate I-frames internally) |
| `-i, --input FILE` | Input H.264 with 2 I-frames (alternative to test mode) |
| `-o, --output FILE` | Output H.264 file (default: output.h264) |
| `-n, --frames N` | Number of scroll P-frames (default: 60) |
| `-w, --width W` | Frame width in pixels |
| `-h, --height H` | Frame height in pixels |
| `--color-a COLOR` | Color for frame A (default: gray) |
| `--color-b COLOR` | Color for frame B (default: gray) |

## Current Limitations

### 16-Pixel Scroll Granularity

Scrolling occurs in 16-pixel increments (one macroblock row at a time). This is because:
- H.264 macroblocks are 16x16 pixels
- Each macroblock can only reference one location in a reference frame
- Sub-macroblock partitions would add complexity

For smoother scrolling, you would need to either:
- Use smaller partition sizes (8x8, 4x4) with appropriate motion vectors
- Accept the 16-pixel step as a design constraint

### Vertical Scroll Only

Currently only supports vertical scrolling. Horizontal scrolling would require similar logic applied to columns instead of rows.

### Solid Color Reference Frames

Test mode generates solid color I-frames using I_PCM macroblocks. For real images, you would need to either:
- Use an external encoder (x264) to create the reference I-frames
- Implement proper intra prediction with DCT coefficients

### No B-Frames

This encoder uses H.264 Baseline profile which doesn't support B-frames. Only I-frames and P-frames are used. B-frames wouldn't benefit our use case anyway since we only reference two static images rather than temporally adjacent frames.

### No Audio Support

This encoder generates video-only H.264 elementary streams.

## File Structure

```
h264-scroll-encoder/
├── src/
│   ├── main.c           # CLI and orchestration
│   ├── h264_encoder.c   # Core encoder (SPS/PPS/slice generation)
│   ├── bitwriter.c      # Bit-level writing utilities
│   ├── nal.c            # NAL unit packaging with EBSP
│   └── nal_parser.c     # NAL unit parsing (for input mode)
├── include/
│   ├── h264_encoder.h
│   ├── bitwriter.h
│   ├── nal.h
│   └── nal_parser.h
├── run.sh               # Full pipeline script
├── Makefile
└── README.md
```

## How Long-Term References Work

To support arbitrarily long videos without frame_num wrapping issues:

1. **IDR Frame A**: Marked as long-term reference with `long_term_frame_idx=0`
2. **I-Frame B**: Uses MMCO commands to mark itself as long-term with `long_term_frame_idx=1`
3. **P-Frames**: Reference by `LongTermPicNum` (0 or 1), which never changes regardless of how many frames are encoded

This allows infinite-length scrolling videos without the flashing artifacts that would occur with short-term references when `frame_num` wraps.

## P-Frame Optimizations

This encoder implements two key optimizations for minimal P-frame sizes.

### MV Prediction

H.264 encodes motion vector differences (MVD) relative to a predicted MV from neighboring macroblocks. This encoder implements proper median MV prediction:

- **Prediction sources**: Left (A), Above (B), Above-right (C) neighbors
- **Algorithm**: Median of available neighbor MVs

Since all macroblocks in a row have identical motion vectors:
- First MB in row: encodes full MV
- Remaining MBs: `mvd = (0,0)` since prediction matches actual

### P_Skip Macroblocks

When a macroblock meets these conditions, it can be "skipped" entirely:
- `ref_idx = 0` (references first long-term picture)
- `mvd = (0, 0)` (motion vector matches prediction)

Skipped macroblocks only increment `mb_skip_run`—no mb_type, ref_idx, mvd, or cbp is written. In our scroll pattern, consecutive macroblocks in the same row (after the first) are all skippable.

### Size Comparison (1280x720)

| Optimization | Per P-frame | 10s video (248 frames) | Reduction |
|--------------|-------------|------------------------|-----------|
| Naive (no prediction) | ~11.5 KB | ~2.85 MB | — |
| + MV Prediction | ~3.0 KB | ~0.74 MB | 74% |
| + P_Skip | ~1.7 KB | ~0.42 MB | 86% |

### What Gets Encoded

For each P-frame row:
- **First MB**: Full encoding (mb_skip_run, mb_type, ref_idx, mvd, cbp)
- **Remaining MBs**: Either P_Skip (just adds to skip count) or minimal encoding with `mvd=(0,0)`

```
Coded MB:      ~6 bits (mb_skip_run + mb_type + ref_idx + mvd + cbp)
Skipped MB:    0 bits (rolled into next mb_skip_run)
```

## License

MIT License
