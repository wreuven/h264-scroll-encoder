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
 *   offset_mb   - Scroll offset in macroblock rows (0 to mb_height)
 *
 * The scroll composition:
 *   - For mb_y in [0, mb_height - offset_mb): use ref A with MV (0, -offset_mb*16)
 *   - For mb_y in [mb_height - offset_mb, mb_height): use ref B with appropriate MV
 */
size_t h264_write_scroll_p_frame(NALWriter *nw, H264EncoderConfig *cfg, int offset_mb);

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
 * Uses I16x16 DC prediction with zero residual (gray output)
 */
size_t h264_write_idr_frame(NALWriter *nw, H264EncoderConfig *cfg);

/*
 * Write a non-IDR I-frame (reference picture B)
 * Uses I16x16 DC prediction with zero residual
 * frame_num should be 1 (following the IDR which has frame_num=0)
 */
size_t h264_write_non_idr_i_frame(NALWriter *nw, H264EncoderConfig *cfg);

#endif /* H264_ENCODER_H */
