#ifndef H264_ENCODER_H
#define H264_ENCODER_H

#include <stdint.h>
#include <stddef.h>
#include "bitwriter.h"
#include "nal.h"

/*
 * H.264 Baseline Profile Scroll Encoder
 *
 * This encoder produces a minimal H.264 stream for scrolling between
 * two reference images A and B. Loop frames use P16x16 macroblocks
 * with no residual coding for maximum efficiency.
 */

/* Slice types (H.264 Table 7-6) */
#define SLICE_TYPE_P    0
#define SLICE_TYPE_B    1
#define SLICE_TYPE_I    2
#define SLICE_TYPE_SP   3
#define SLICE_TYPE_SI   4
/* Types 5-9 are the same but indicate all slices in picture are same type */
#define SLICE_TYPE_P_ALL    5
#define SLICE_TYPE_I_ALL    7

/* Macroblock types for P-slices (H.264 Table 7-13) */
#define P_MB_L0_16x16   0   /* P_L0_16x16: single 16x16 partition, list 0 */
#define P_MB_SKIP       -1  /* P_Skip (special, not written as mb_type) */

/* Maximum number of waypoint references */
#define MAX_WAYPOINTS 8

/* Hardware MV limit: 496 pixels (safely under 512) */
#define MV_LIMIT_PX 496
#define WAYPOINT_INTERVAL_MB 31  /* Legacy, kept for compatibility */

/* Waypoint info for intermediate reference frames */
typedef struct {
    int offset_px;          /* Scroll offset in pixels at which this waypoint was created */
    int long_term_idx;      /* Long-term frame index (2, 3, 4, ...) */
    int valid;              /* Whether this waypoint is active */
} WaypointInfo;

/* Encoder configuration */
typedef struct {
    int width;              /* Frame width in pixels (must be multiple of 16) */
    int height;             /* Frame height in pixels (must be multiple of 16) */
    int mb_width;           /* Width in macroblocks */
    int mb_height;          /* Height in macroblocks */

    /* Reference frame info (parsed from setup stream) */
    uint8_t *sps_rbsp;      /* SPS RBSP data */
    size_t sps_size;
    uint8_t *pps_rbsp;      /* PPS RBSP data */
    size_t pps_size;

    /* Parsed SPS values needed for slice headers */
    int log2_max_frame_num;
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb;   /* Only if poc_type == 0 */

    /* Parsed PPS values needed for slice headers */
    int num_ref_idx_l0_default_minus1;
    int deblocking_filter_control_present_flag;

    /* Frame tracking */
    int frame_num;          /* Current frame_num (wraps at max_frame_num) */
    int idr_pic_id;         /* IDR picture ID */

    /* Waypoint support for extended scroll range */
    WaypointInfo waypoints[MAX_WAYPOINTS];
    int num_waypoints;      /* Number of active waypoints */
} H264EncoderConfig;

/* Initialize encoder config from frame dimensions */
void h264_encoder_init(H264EncoderConfig *cfg, int width, int height);

/* Set SPS data (parsed from external encoder) */
void h264_encoder_set_sps(H264EncoderConfig *cfg, const uint8_t *sps, size_t size,
                          int log2_max_frame_num, int poc_type, int log2_max_poc_lsb);

/* Set PPS data (parsed from external encoder) */
void h264_encoder_set_pps(H264EncoderConfig *cfg, const uint8_t *pps, size_t size,
                          int num_ref_idx_l0_default_minus1,
                          int deblocking_filter_control_present_flag);

/*
 * Write a P16x16 macroblock with no residual
 *
 * Parameters:
 *   bw          - BitWriter for output
 *   ref_idx     - Reference index (0=IMG_A, 1=IMG_B)
 *   mvd_x       - Motion vector delta X (in quarter-pel units for syntax,
 *                 but we use full-pel so multiply by 4)
 *   mvd_y       - Motion vector delta Y
 *   pred_mvx    - Predicted MV X (for delta calculation)
 *   pred_mvy    - Predicted MV Y (for delta calculation)
 */
void h264_write_p16x16_mb(BitWriter *bw, int ref_idx, int mvd_x, int mvd_y);

/*
 * Write a complete P-frame for scrolling
 *
 * Parameters:
 *   nw          - NAL writer for output
 *   cfg         - Encoder config
 *   offset_px   - Scroll offset in pixels (0 to height)
 *
 * The scroll composition:
 *   - A region: MV.y = offset_px (smooth per-pixel scrolling)
 *   - B region: MV.y = offset_px - height (smooth per-pixel scrolling)
 *   - A/B boundary is at MB row (height - offset_px) / 16
 */
size_t h264_write_scroll_p_frame(NALWriter *nw, H264EncoderConfig *cfg, int offset_px);

/*
 * Write a waypoint P-frame (reference P-frame for extended scroll range)
 *
 * This creates a P-frame at the given scroll offset and marks it as a
 * long-term reference using MMCO commands. Subsequent P-frames can
 * reference this waypoint with smaller MVs.
 *
 * Parameters:
 *   nw          - NAL writer for output
 *   cfg         - Encoder config
 *   offset_px   - Scroll offset in pixels for this waypoint
 *
 * Returns: bytes written
 */
size_t h264_write_waypoint_p_frame(NALWriter *nw, H264EncoderConfig *cfg, int offset_px);

/*
 * Check if a waypoint is needed at the given scroll offset
 *
 * Returns 1 if a waypoint should be inserted, 0 otherwise.
 * Waypoints are needed every 496 pixels to keep MVs within hardware limits.
 */
int h264_needs_waypoint(H264EncoderConfig *cfg, int offset_px);

/*
 * Generate a minimal SPS for Baseline profile
 * (Used if not loading from external encoder)
 */
size_t h264_generate_sps(uint8_t *rbsp, size_t capacity, int width, int height);

/*
 * Generate a minimal PPS for Baseline profile
 */
size_t h264_generate_pps(uint8_t *rbsp, size_t capacity);

/*
 * Write an IDR I-frame (reference picture A)
 * Uses I_PCM macroblocks (gray output by default)
 */
size_t h264_write_idr_frame(NALWriter *nw, H264EncoderConfig *cfg);

/*
 * Write an IDR I-frame with specified YCbCr color
 *
 * Common colors (BT.601):
 *   Red:   y=81,  cb=90,  cr=240
 *   Blue:  y=41,  cb=240, cr=110
 *   Green: y=145, cb=54,  cr=34
 *   Gray:  y=128, cb=128, cr=128
 */
size_t h264_write_idr_frame_color(NALWriter *nw, H264EncoderConfig *cfg,
                                   uint8_t y, uint8_t cb, uint8_t cr);

/*
 * Write a non-IDR I-frame (reference picture B)
 * Uses I_PCM macroblocks
 * frame_num should be 1 (following the IDR which has frame_num=0)
 */
size_t h264_write_non_idr_i_frame(NALWriter *nw, H264EncoderConfig *cfg);

/*
 * Write a non-IDR I-frame with specified YCbCr color
 */
size_t h264_write_non_idr_i_frame_color(NALWriter *nw, H264EncoderConfig *cfg,
                                         uint8_t y, uint8_t cb, uint8_t cr);

/*
 * Write an IDR I-frame with 3 horizontal color stripes
 * Useful for verifying scroll behavior (distinguishes scroll from wipe)
 *
 * y1/cb1/cr1: Top third color
 * y2/cb2/cr2: Middle third color
 * y3/cb3/cr3: Bottom third color
 */
size_t h264_write_idr_frame_striped(NALWriter *nw, H264EncoderConfig *cfg,
                                     uint8_t y1, uint8_t cb1, uint8_t cr1,
                                     uint8_t y2, uint8_t cb2, uint8_t cr2,
                                     uint8_t y3, uint8_t cb3, uint8_t cr3);

/*
 * Write a non-IDR I-frame with 3 horizontal color stripes
 */
size_t h264_write_non_idr_i_frame_striped(NALWriter *nw, H264EncoderConfig *cfg,
                                           uint8_t y1, uint8_t cb1, uint8_t cr1,
                                           uint8_t y2, uint8_t cb2, uint8_t cr2,
                                           uint8_t y3, uint8_t cb3, uint8_t cr3);

/*
 * Write an IDR I-frame from raw YUV420p data
 *
 * yuv_data: raw YUV420p frame (Y plane, then U/Cb plane, then V/Cr plane)
 * Layout: Y[width*height], U[width*height/4], V[width*height/4]
 */
size_t h264_write_idr_frame_yuv(NALWriter *nw, H264EncoderConfig *cfg,
                                 const uint8_t *yuv_data);

/*
 * Write a non-IDR I-frame from raw YUV420p data
 */
size_t h264_write_non_idr_i_frame_yuv(NALWriter *nw, H264EncoderConfig *cfg,
                                       const uint8_t *yuv_data);

/*
 * Rewrite x264 IDR frame with our slice header (for long-term ref marking)
 *
 * This extracts the compressed MB data from x264's IDR frame and wraps it
 * with our own slice header that sets long_term_reference_flag=1.
 *
 * Parameters:
 *   nw          - NAL writer for output
 *   cfg         - Encoder config (must have SPS params set)
 *   rbsp        - x264's IDR RBSP data (after EBSP-to-RBSP conversion)
 *   rbsp_size   - Size of RBSP data
 *
 * Returns: bytes written, or 0 on error
 */
size_t h264_rewrite_idr_frame(NALWriter *nw, H264EncoderConfig *cfg,
                               const uint8_t *rbsp, size_t rbsp_size);

/*
 * Rewrite x264 IDR frame as non-IDR I-frame with MMCO commands
 *
 * This extracts the compressed MB data from x264's IDR frame and wraps it
 * with a non-IDR I-frame slice header that uses MMCO to mark as long-term.
 *
 * Parameters:
 *   nw          - NAL writer for output
 *   cfg         - Encoder config
 *   rbsp        - x264's IDR RBSP data
 *   rbsp_size   - Size of RBSP data
 *   frame_num   - frame_num for this frame
 *
 * Returns: bytes written, or 0 on error
 */
size_t h264_rewrite_as_non_idr_i_frame(NALWriter *nw, H264EncoderConfig *cfg,
                                        const uint8_t *rbsp, size_t rbsp_size,
                                        int frame_num);

/*
 * Extended rewrite functions that use separate configs for parsing and writing.
 *
 * parse_cfg: Config with x264's SPS params (for parsing the input slice header)
 * write_cfg: Config with our SPS params (for writing the output slice header)
 */
size_t h264_rewrite_idr_frame_ex(NALWriter *nw, H264EncoderConfig *write_cfg,
                                  H264EncoderConfig *parse_cfg,
                                  const uint8_t *rbsp, size_t rbsp_size);

size_t h264_rewrite_as_non_idr_i_frame_ex(NALWriter *nw, H264EncoderConfig *write_cfg,
                                           H264EncoderConfig *parse_cfg,
                                           const uint8_t *rbsp, size_t rbsp_size,
                                           int frame_num);

#endif /* H264_ENCODER_H */
