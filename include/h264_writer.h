#ifndef H264_WRITER_H
#define H264_WRITER_H

#include <stdint.h>
#include <stddef.h>
#include "bitwriter.h"
#include "nal.h"

/*
 * H.264 Writer Module for Composer v0.1
 *
 * Provides:
 * - SPS/PPS generation (minimal Baseline profile)
 * - I-frame rewriting with long-term reference marking
 * - P-frame generation with motion vectors for scrolling
 */

/* Slice types (H.264 Table 7-6) */
#define SLICE_TYPE_P        0
#define SLICE_TYPE_I        2
#define SLICE_TYPE_I_ALL    7

/* Hardware MV limit: 496 pixels (safely under 512 for NVDEC) */
#define MV_LIMIT_PX 496

/* Maximum number of waypoint references (for extended scroll range) */
#define MAX_WAYPOINTS 8

/* Waypoint info for intermediate reference frames */
typedef struct {
    int offset_px;          /* Scroll offset in pixels where created */
    int long_term_idx;      /* Long-term frame index (2, 3, ...) */
    int valid;              /* Whether this waypoint is active */
} WaypointInfo;

/* Encoder configuration */
typedef struct {
    int width;              /* Frame width in pixels (multiple of 16) */
    int height;             /* Frame height in pixels (multiple of 16) */
    int mb_width;           /* Width in macroblocks */
    int mb_height;          /* Height in macroblocks */

    /* Parsed SPS values */
    int log2_max_frame_num;
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb;

    /* Parsed PPS values */
    int num_ref_idx_l0_default_minus1;
    int deblocking_filter_control_present_flag;

    /* Frame tracking */
    int frame_num;
    int idr_pic_id;

    /* Waypoint support */
    WaypointInfo waypoints[MAX_WAYPOINTS];
    int num_waypoints;
} ComposerConfig;

/*
 * Initialize config from frame dimensions
 */
void composer_config_init(ComposerConfig *cfg, int width, int height);

/*
 * Set SPS parameters (parsed from external encoder)
 */
void composer_config_set_sps_params(ComposerConfig *cfg,
                                     int log2_max_frame_num,
                                     int pic_order_cnt_type,
                                     int log2_max_pic_order_cnt_lsb);

/*
 * Set PPS parameters (parsed from external encoder)
 */
void composer_config_set_pps_params(ComposerConfig *cfg,
                                     int num_ref_idx_l0_default_minus1,
                                     int deblocking_filter_control_present_flag);

/*
 * Generate minimal SPS for Baseline profile
 * Returns RBSP size
 */
size_t h264_generate_sps(uint8_t *rbsp, size_t capacity, int width, int height);

/*
 * Generate minimal PPS for Baseline profile
 * Returns RBSP size
 */
size_t h264_generate_pps(uint8_t *rbsp, size_t capacity);

/*
 * Rewrite externally-encoded IDR frame with long-term reference flag
 *
 * parse_cfg: Config with external encoder's SPS params (for parsing)
 * write_cfg: Config with our SPS params (for writing)
 * rbsp: External encoder's IDR RBSP data
 */
size_t h264_rewrite_idr_frame(NALWriter *nw, ComposerConfig *write_cfg,
                               ComposerConfig *parse_cfg,
                               const uint8_t *rbsp, size_t rbsp_size);

/*
 * Rewrite externally-encoded IDR as non-IDR I-frame with MMCO
 *
 * Marks frame as long_term_frame_idx=1 (frame B)
 */
size_t h264_rewrite_as_non_idr_i_frame(NALWriter *nw, ComposerConfig *write_cfg,
                                        ComposerConfig *parse_cfg,
                                        const uint8_t *rbsp, size_t rbsp_size,
                                        int frame_num);

/*
 * Write a P-frame with scroll motion vectors
 *
 * offset_px: Scroll offset in pixels (0 = show all of A, height = show all of B)
 *
 * Composition:
 *   - A region (mb_y < boundary): ref=0, mv_y = offset_px
 *   - B region (mb_y >= boundary): ref=1, mv_y = offset_px - height
 *   - boundary = (height - offset_px) / 16
 */
size_t h264_write_scroll_p_frame(NALWriter *nw, ComposerConfig *cfg, int offset_px);

/*
 * Check if a waypoint is needed at the given scroll offset
 * Returns 1 if waypoint needed, 0 otherwise
 */
int h264_needs_waypoint(ComposerConfig *cfg, int offset_px);

/*
 * Write a waypoint P-frame (intermediate reference for extended scroll)
 */
size_t h264_write_waypoint_p_frame(NALWriter *nw, ComposerConfig *cfg, int offset_px);

#endif /* H264_WRITER_H */
