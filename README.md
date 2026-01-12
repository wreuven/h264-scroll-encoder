# UI-Aware Hybrid H.264 Encoder

A specialized H.264 encoder for Netflix-like scrolling UI streams where most pixels are deterministic motion or static, with only small dynamic regions requiring full encoding.

## Concept

Traditional video encoders process every pixel of every frame. For UI video streams, this is wasteful because:

- **Scrolling content** can be expressed as motion vectors referencing a UI atlas
- **Static UI chrome** requires no re-encoding
- **Dynamic content** (e.g., preview videos) is bounded to small regions

This encoder composes frames at the bitstream level:

1. **Motion-only regions**: Emit P_Skip macroblocks with motion vectors pointing to reference frames
2. **Dynamic regions**: Splice in pre-encoded macroblocks from a conventional encoder
3. **Result**: Full-frame H.264 without full-frame pixel encoding

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Composer Encoder                        │
│  - Consumes UI hints (motion regions, dynamic rect)         │
│  - Emits inter-coded MBs for motion-only regions            │
│  - Splices dynamic-region macroblocks into output           │
│  - Maintains reference frames (UI atlases)                  │
└─────────────────────────────────────────────────────────────┘
         │                              ▲
         │ UI hints                     │ Encoded MBs
         ▼                              │
┌─────────────────┐           ┌─────────────────────┐
│   Application   │           │   Dynamic Encoder   │
│   (renderer)    │──────────>│   (conventional)    │
│                 │  cropped  │   ≤392×392 region   │
└─────────────────┘  pixels   └─────────────────────┘
```

## Resource Savings

For a 1280×720 frame with 360×360 dynamic region:

- Full frame: 921,600 pixels
- Dynamic region: ~153,664 pixels (16.7%)
- **83% reduction** in pixels requiring conventional encoding

## Project Structure

```
h264-scroll-encoder/
├── src/                    # Main encoder (WIP)
│   └── bitwriter.c         # Bit-level I/O utilities
├── include/
│   └── bitwriter.h
├── docs/
│   └── MASTER_DESIGN.md    # Full architecture specification
└── experiments/            # Learning experiments
    ├── scroll-encoder/     # MV-only scroll animation generator
    └── trans-resizer/      # Bitstream width modification attempt
```

## Experiments

The `experiments/` folder contains learning experiments that informed the design:

### scroll-encoder

Generates scrolling animations between two reference images using only motion vectors. Demonstrated:
- H.264 NAL unit structure and SPS/PPS generation
- Long-term reference picture management
- P_Skip optimization for motion-vector-only frames

### trans-resizer

Attempted to widen H.264 streams by adding padding without re-encoding. Revealed:
- CAVLC entropy coding context (nC) dependencies
- Intra prediction fundamentally depends on edge samples
- I-frames cannot be safely modified; P-frames are more tractable

**Key insight**: Reference frames must be created at full resolution from the start. P-frame composition using inter prediction (P_Skip) avoids intra prediction issues.

## Building

```bash
make
```

## Status

The main Composer encoder is under development. See `docs/MASTER_DESIGN.md` for the full specification.

## License

MIT License
