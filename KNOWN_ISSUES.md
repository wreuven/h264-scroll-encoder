# Known Issues

## Composer v0.1

### Scroll appears to jump in 16-pixel increments

**Status**: Open
**Observed**: The green top portion of frame B appears to scroll in jumps of approximately 16 pixels at a time, rather than smooth per-pixel scrolling.

**Likely cause**: Motion vectors are applied at the macroblock level (16x16 pixels). While sub-pixel motion compensation exists in H.264, the current implementation may not be generating the correct motion vector residuals for smooth inter-macroblock transitions.

**Impact**: Visual stuttering during scroll, especially noticeable on solid color regions where the eye can easily detect discontinuities.
