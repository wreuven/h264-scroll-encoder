#ifndef COMPOSER_H
#define COMPOSER_H

#include <stdint.h>
#include <stddef.h>
#include "h264_writer.h"
#include "nal.h"

/*
 * Composer v0.1 - UI-Aware Hybrid H.264 Encoder
 *
 * Takes two externally-encoded I-frames and generates P-frames with
 * scroll motion vectors. The I-frames are rewritten with long-term
 * reference marking.
 *
 * Usage:
 *   1. Call composer_init() with paths to ref_a.h264 and ref_b.h264
 *   2. Call composer_write_header() to output SPS + PPS + I-frames
 *   3. Call composer_write_scroll_frame() for each P-frame
 *   4. Call composer_finish() to clean up
 */

typedef struct {
    /* Configuration */
    ComposerConfig cfg;         /* H.264 encoding config */
    ComposerConfig parse_cfg;   /* Config for parsing external encoder's headers */

    /* Parsed reference frames */
    uint8_t *ref_a_rbsp;        /* RefA IDR RBSP data */
    size_t ref_a_size;
    uint8_t *ref_b_rbsp;        /* RefB IDR RBSP data */
    size_t ref_b_size;

    /* Original SPS/PPS from external encoder */
    uint8_t *orig_sps;
    size_t orig_sps_size;
    uint8_t *orig_pps;
    size_t orig_pps_size;

    /* Output state */
    NALWriter nw;
    uint8_t *output_buffer;
    size_t output_capacity;
    uint8_t *rbsp_temp;
    size_t rbsp_capacity;

    /* Frame tracking */
    int frames_written;
} Composer;

/*
 * Initialize composer from two reference H.264 files
 *
 * ref_a_path: Path to first reference I-frame (H.264 file with single IDR)
 * ref_b_path: Path to second reference I-frame
 *
 * Returns 0 on success, -1 on error
 */
int composer_init(Composer *c, const char *ref_a_path, const char *ref_b_path);

/*
 * Get video dimensions (after init)
 */
int composer_get_width(Composer *c);
int composer_get_height(Composer *c);

/*
 * Write stream header (SPS + PPS + rewritten I-frames)
 *
 * Must be called before any P-frames
 */
void composer_write_header(Composer *c);

/*
 * Write a scroll P-frame at the given offset
 *
 * offset_px: Scroll offset in pixels (0 = full A, height = full B)
 */
void composer_write_scroll_frame(Composer *c, int offset_px);

/*
 * Get current output size in bytes
 */
size_t composer_get_output_size(Composer *c);

/*
 * Get pointer to output buffer
 */
uint8_t *composer_get_output(Composer *c);

/*
 * Write output to file
 *
 * Returns 0 on success, -1 on error
 */
int composer_write_to_file(Composer *c, const char *path);

/*
 * Clean up resources
 */
void composer_finish(Composer *c);

#endif /* COMPOSER_H */
