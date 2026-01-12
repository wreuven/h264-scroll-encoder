# Trans-resizer Experiment

**Status: Learning experiment (archived)**

## What it attempted

Trans-resizer tried to widen an H.264 video from 320x320 to 720x320 by:
1. Adding I_PCM padding for I-frames
2. Adding P_Skip padding for P-frames
3. Copying content MBs with CAVLC context translation

## What we learned

### CAVLC Context Dependencies
- `coeff_token` encoding depends on neighboring blocks' `total_coeff` (nC)
- When repositioning MBs, input and output neighbors differ
- Must decode with input nC, re-encode with output nC
- I_PCM neighbors contribute nC=16 (not 0) per H.264 spec

### Intra Prediction Problem
- I_16x16 and I_4x4 prediction uses neighboring *samples*, not just coefficients
- When edges become I_PCM (black), prediction context changes
- MBs with `cbp_luma=0` (no AC residual) decode purely from prediction
- Result: content becomes black because prediction references black padding

### Key Insight
This approach is fundamentally flawed for I-frames because intra prediction
depends on edge samples. The real solution (per MASTER_DESIGN.md) is:
- Reference I-frames are created at full resolution from the start
- Only P-frames need padding, using inter prediction (P_Skip)

## Files

- `trans_resizer.c` - Main implementation with CAVLC context translation
- `bitwriter.c/h` - Bit-level I/O utilities

## Building

```bash
make
./trans_resizer input.h264 output.h264
```
