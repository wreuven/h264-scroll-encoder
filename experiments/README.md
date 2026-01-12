# Experiments

Learning experiments that informed the design of the UI-aware hybrid encoder.

## scroll-encoder

The original H.264 scroll encoder that generates scrolling animations between
two reference images using only motion vectors.

**Key learnings:**
- H.264 NAL unit structure, SPS/PPS generation
- Long-term reference picture management
- P_Skip optimization for MV-only frames
- Motion vector prediction and encoding

## trans-resizer

Attempted to widen H.264 streams by adding padding without full re-encoding.

**Key learnings:**
- CAVLC entropy coding context (nC) dependencies
- Intra prediction depends on neighboring samples, not just coefficients
- Bitstream surgery has fundamental limits for intra-coded content
- Inter-coded (P-frame) manipulation is more tractable

## Lessons Applied to Main Design

1. **Reference frames at full resolution**: Don't try to resize I-frames.
   Create atlas/reference frames at target resolution from the start.

2. **P-frame composition**: Use P_Skip for padding regions. Inter prediction
   references the atlas, avoiding intra prediction problems.

3. **Dynamic region splicing**: Pre-encoded dynamic content can be inserted
   at specific MB positions with header rewriting.
