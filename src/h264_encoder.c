#include "h264_encoder.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

/* Hardware MV limit: 31 MB = 496 pixels = 1984 qpel (safely under 2048) */
#define WAYPOINT_INTERVAL_MB 31

/* Forward declarations for waypoint support */
static void h264_write_p_slice_header_waypoint(BitWriter *bw, H264EncoderConfig *cfg,
                                                int first_mb, int frame_num, int poc_lsb,
                                                int is_reference, int long_term_idx);

void h264_encoder_init(H264EncoderConfig *cfg, int width, int height) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->width = width;
    cfg->height = height;
    cfg->mb_width = width / 16;
    cfg->mb_height = height / 16;
    cfg->frame_num = 0;
    cfg->idr_pic_id = 0;

    /* Defaults - will be overridden if loading from external SPS */
    cfg->log2_max_frame_num = 4;        /* max_frame_num = 16 */
    cfg->pic_order_cnt_type = 2;        /* POC derived from frame_num */
    cfg->log2_max_pic_order_cnt_lsb = 4;

    /* Default PPS values - match our generated PPS */
    cfg->num_ref_idx_l0_default_minus1 = 1;         /* 2 refs default */
    cfg->deblocking_filter_control_present_flag = 1; /* We generate PPS with this set */
}

void h264_encoder_set_sps(H264EncoderConfig *cfg, const uint8_t *sps, size_t size,
                          int log2_max_frame_num, int poc_type, int log2_max_poc_lsb) {
    cfg->sps_rbsp = malloc(size);
    memcpy(cfg->sps_rbsp, sps, size);
    cfg->sps_size = size;
    cfg->log2_max_frame_num = log2_max_frame_num;
    cfg->pic_order_cnt_type = poc_type;
    cfg->log2_max_pic_order_cnt_lsb = log2_max_poc_lsb;
}

void h264_encoder_set_pps(H264EncoderConfig *cfg, const uint8_t *pps, size_t size,
                          int num_ref_idx_l0_default_minus1,
                          int deblocking_filter_control_present_flag) {
    cfg->pps_rbsp = malloc(size);
    memcpy(cfg->pps_rbsp, pps, size);
    cfg->pps_size = size;
    cfg->num_ref_idx_l0_default_minus1 = num_ref_idx_l0_default_minus1;
    cfg->deblocking_filter_control_present_flag = deblocking_filter_control_present_flag;
}

/*
 * Generate minimal SPS for Baseline profile
 *
 * profile_idc = 66 (Baseline)
 * level_idc = 40 (Level 4.0, supports 1080p30)
 */
size_t h264_generate_sps(uint8_t *rbsp, size_t capacity, int width, int height) {
    BitWriter bw;
    bitwriter_init(&bw, rbsp, capacity);

    int mb_width = width / 16;
    int mb_height = height / 16;

    /* profile_idc: Baseline = 66 */
    bitwriter_write_bits(&bw, 66, 8);

    /* constraint_set0_flag through constraint_set5_flag + reserved */
    /* Match x264: constraint_set0=1, constraint_set1=1 */
    bitwriter_write_bits(&bw, 0xc0, 8);

    /* level_idc: 40 = Level 4.0 (required for vertical MV range > 512 pixels) */
    bitwriter_write_bits(&bw, 40, 8);

    /* seq_parameter_set_id: ue(0) */
    bitwriter_write_ue(&bw, 0);

    /* log2_max_frame_num_minus4: ue(0) -> log2=4 -> max_frame_num = 16 */
    bitwriter_write_ue(&bw, 0);

    /* pic_order_cnt_type: ue(2) -> POC = 2*frame_num for frames */
    bitwriter_write_ue(&bw, 2);

    /* max_num_ref_frames: ue(v) -> we need 2 base refs (A and B) + waypoints */
    bitwriter_write_ue(&bw, 2 + MAX_WAYPOINTS);  /* 10 refs max */

    /* gaps_in_frame_num_value_allowed_flag: u(1) = 0 */
    bitwriter_write_bit(&bw, 0);

    /* pic_width_in_mbs_minus1: ue(v) */
    bitwriter_write_ue(&bw, mb_width - 1);

    /* pic_height_in_map_units_minus1: ue(v) */
    bitwriter_write_ue(&bw, mb_height - 1);

    /* frame_mbs_only_flag: u(1) = 1 (frames only, no fields) */
    bitwriter_write_bit(&bw, 1);

    /* direct_8x8_inference_flag: u(1) = 1 */
    bitwriter_write_bit(&bw, 1);

    /* frame_cropping_flag: u(1) = 0 */
    bitwriter_write_bit(&bw, 0);

    /* vui_parameters_present_flag: u(1) = 0 */
    bitwriter_write_bit(&bw, 0);

    bitwriter_write_trailing_bits(&bw);
    return bitwriter_get_size(&bw);
}

/*
 * Generate minimal PPS for Baseline profile
 */
size_t h264_generate_pps(uint8_t *rbsp, size_t capacity) {
    BitWriter bw;
    bitwriter_init(&bw, rbsp, capacity);

    /* pic_parameter_set_id: ue(0) */
    bitwriter_write_ue(&bw, 0);

    /* seq_parameter_set_id: ue(0) */
    bitwriter_write_ue(&bw, 0);

    /* entropy_coding_mode_flag: u(1) = 0 (CAVLC) */
    bitwriter_write_bit(&bw, 0);

    /* bottom_field_pic_order_in_frame_present_flag: u(1) = 0 */
    bitwriter_write_bit(&bw, 0);

    /* num_slice_groups_minus1: ue(0) */
    bitwriter_write_ue(&bw, 0);

    /* num_ref_idx_l0_default_active_minus1: ue(1) -> 2 refs default */
    bitwriter_write_ue(&bw, 1);

    /* num_ref_idx_l1_default_active_minus1: ue(0) */
    bitwriter_write_ue(&bw, 0);

    /* weighted_pred_flag: u(1) = 0 */
    bitwriter_write_bit(&bw, 0);

    /* weighted_bipred_idc: u(2) = 0 */
    bitwriter_write_bits(&bw, 0, 2);

    /* pic_init_qp_minus26: se(0) -> QP = 26 */
    bitwriter_write_se(&bw, 0);

    /* pic_init_qs_minus26: se(0) */
    bitwriter_write_se(&bw, 0);

    /* chroma_qp_index_offset: se(0) */
    bitwriter_write_se(&bw, 0);

    /* deblocking_filter_control_present_flag: u(1) = 1 */
    bitwriter_write_bit(&bw, 1);

    /* constrained_intra_pred_flag: u(1) = 0 */
    bitwriter_write_bit(&bw, 0);

    /* redundant_pic_cnt_present_flag: u(1) = 0 */
    bitwriter_write_bit(&bw, 0);

    bitwriter_write_trailing_bits(&bw);
    return bitwriter_get_size(&bw);
}

/*
 * Write P-slice header
 *
 * This writes the slice header for a P-frame that references both A and B.
 * is_reference: if true, write dec_ref_pic_marking; if false, skip it
 */
static void h264_write_p_slice_header(BitWriter *bw, H264EncoderConfig *cfg,
                               int first_mb, int frame_num, int poc_lsb,
                               int is_reference) {
    /* first_mb_in_slice: ue(v) */
    bitwriter_write_ue(bw, first_mb);

    /* slice_type: ue(v) = 0 (P-slice) or 5 (all P) */
    bitwriter_write_ue(bw, SLICE_TYPE_P);

    /* pic_parameter_set_id: ue(0) */
    bitwriter_write_ue(bw, 0);

    /* frame_num: u(log2_max_frame_num) */
    int frame_num_bits = cfg->log2_max_frame_num;
    bitwriter_write_bits(bw, frame_num & ((1 << frame_num_bits) - 1), frame_num_bits);

    /* If pic_order_cnt_type == 0, write POC LSB */
    if (cfg->pic_order_cnt_type == 0) {
        int poc_bits = cfg->log2_max_pic_order_cnt_lsb;
        bitwriter_write_bits(bw, poc_lsb & ((1 << poc_bits) - 1), poc_bits);
    }
    /* POC type 2 derives POC from frame_num, no extra syntax */

    /* num_ref_idx_active_override_flag: u(1) = 1 */
    /* We want exactly 2 refs in list L0 for scroll */
    bitwriter_write_bit(bw, 1);

    /* num_ref_idx_l0_active_minus1: ue(1) -> 2 active refs */
    bitwriter_write_ue(bw, 1);

    /* ref_pic_list_modification (for P-slices) */
    /* With only long-term references in DPB, default RefPicList0 should be built from
     * long-term pics in ascending order of long_term_pic_num: [A(ltp=0), B(ltp=1)]
     *
     * However, some hardware decoders may not handle long-term-only reference lists
     * correctly. We add EXPLICIT modification to ensure consistent behavior:
     *
     * modification_of_pic_nums_idc=2 means: use long_term_pic_num
     * We specify LongTermPicNum 0 (A) first, then LongTermPicNum 1 (B)
     */
    /* ref_pic_list_modification_flag_l0: u(1) = 1 */
    bitwriter_write_bit(bw, 1);

    /* First entry: LongTermPicNum 0 (A) at refIdxL0 0 */
    bitwriter_write_ue(bw, 2);  /* modification_of_pic_nums_idc = 2 (long_term_pic_num) */
    bitwriter_write_ue(bw, 0);  /* long_term_pic_num = 0 */

    /* Second entry: LongTermPicNum 1 (B) at refIdxL0 1 */
    bitwriter_write_ue(bw, 2);  /* modification_of_pic_nums_idc = 2 (long_term_pic_num) */
    bitwriter_write_ue(bw, 1);  /* long_term_pic_num = 1 */

    /* End of modification */
    bitwriter_write_ue(bw, 3);  /* modification_of_pic_nums_idc = 3 (end) */

    /* dec_ref_pic_marking() - only for reference pictures (nal_ref_idc != 0) */
    if (is_reference) {
        /* For non-IDR reference frames: */
        /* adaptive_ref_pic_marking_mode_flag: u(1) = 0 */
        /* Use sliding window (simple, keeps most recent refs) */
        bitwriter_write_bit(bw, 0);
    }
    /* For non-reference pictures (nal_ref_idc = 0), skip dec_ref_pic_marking */

    /* slice_qp_delta: se(0) -> use PPS QP */
    bitwriter_write_se(bw, 0);

    /* Only write deblocking filter syntax if PPS has the flag set */
    if (cfg->deblocking_filter_control_present_flag) {
        /* disable_deblocking_filter_idc: ue(1) = disable */
        bitwriter_write_ue(bw, 1);  /* 1 = disable deblocking */
    }
}

/*
 * Write P16x16 macroblock with no residual
 *
 * mb_type = 0 (P_L0_16x16)
 * ref_idx_l0 = ref_idx (if more than 1 ref active)
 * mvd_l0 = motion vector delta (in full-pel, will be converted to quarter-pel)
 * coded_block_pattern = 0 (no residual)
 */
void h264_write_p16x16_mb(BitWriter *bw, int ref_idx, int mvd_x, int mvd_y) {
    /* mb_type: ue(0) = P_L0_16x16 */
    bitwriter_write_ue(bw, 0);

    /* ref_idx_l0[0]: te(v) - only written if num_ref_idx_l0_active_minus1 > 0 */
    /* With 2 refs (num_ref_idx_l0_active_minus1 = 1), this is te(1) */
    /* te(1) encodes value v as bit (1-v): value 0 -> bit 1, value 1 -> bit 0 */
    bitwriter_write_bit(bw, 1 - (ref_idx & 1));

    /* mvd_l0[0][0]: se(mvd_x) - horizontal */
    /* MVs are in quarter-pel, but we use full-pel, so multiply input by 4 */
    bitwriter_write_se(bw, mvd_x * 4);

    /* mvd_l0[0][1]: se(mvd_y) - vertical */
    bitwriter_write_se(bw, mvd_y * 4);

    /* coded_block_pattern: me(v) = 0 (no luma or chroma residual) */
    /* For inter MBs, CBP 0 -> codeNum 0 */
    bitwriter_write_ue(bw, 0);  /* CBP = 0, no residual */

    /* No mb_qp_delta since CBP = 0 */
    /* No residual data since CBP = 0 */
}

/*
 * Write P16x16 macroblock with MVD already in quarter-pel units
 * Used when MV prediction is computed in quarter-pel
 *
 * num_refs: number of active references (for te(v) encoding of ref_idx)
 */
static void h264_write_p16x16_mb_qpel(BitWriter *bw, int ref_idx, int mvd_x_qpel, int mvd_y_qpel, int num_refs) {
    /* mb_type: ue(0) = P_L0_16x16 */
    bitwriter_write_ue(bw, 0);

    /* ref_idx_l0[0]: te(v) where v = num_ref_idx_l0_active_minus1
     * te(0) = no bits (only one ref)
     * te(1) = single bit (0->1, 1->0)
     * te(v>1) = ue(v)
     */
    if (num_refs == 1) {
        /* No ref_idx written - only one possible value */
    } else if (num_refs == 2) {
        /* te(1): single bit, inverted */
        bitwriter_write_bit(bw, 1 - (ref_idx & 1));
    } else {
        /* te(v>1): use ue() encoding */
        bitwriter_write_ue(bw, ref_idx);
    }

    /* mvd_l0[0][0]: se(mvd_x) - already in quarter-pel */
    bitwriter_write_se(bw, mvd_x_qpel);

    /* mvd_l0[0][1]: se(mvd_y) - already in quarter-pel */
    bitwriter_write_se(bw, mvd_y_qpel);

    /* coded_block_pattern: ue(0) = no residual */
    bitwriter_write_ue(bw, 0);
}

/*
 * Motion vector prediction for P16x16
 *
 * H.264 uses median prediction from neighbors A (left), B (above), C (above-right).
 * If C is unavailable, use D (above-left) instead.
 *
 * For our scroll encoder, all MBs in a row have the same MV, so:
 * - mb_x > 0: left neighbor has same MV → prediction matches → mvd = (0,0)
 * - mb_x = 0: predict from above row (may differ at A/B region boundary)
 *
 * This dramatically reduces P-frame size since most MBs encode mvd=(0,0).
 */
static int median3(int a, int b, int c) {
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { b = c; }
    if (a > b) { a = b; }
    return b > a ? b : a;
}

typedef struct {
    int mv_x, mv_y;
    int ref_idx;
    int available;
} MVInfo;

static void get_mv_prediction_ex(int mb_x, int mb_y, int mb_width,
                                  const MVInfo *above_row, const MVInfo *left,
                                  int cur_ref_idx,
                                  int *pred_mvx, int *pred_mvy) {
    /*
     * H.264 MV prediction for P16x16 (spec 8.4.1.3.1):
     * A = left, B = above, C = above-right (or D = above-left if C unavailable)
     *
     * Key rule: If EXACTLY ONE neighbor has matching ref_idx, use that MV directly.
     * Otherwise, use median with mismatched ref_idx neighbors treated as MV=(0,0).
     */
    MVInfo a = {0}, b = {0}, c = {0};
    int a_ref_match = 0, b_ref_match = 0, c_ref_match = 0;

    /* A: left neighbor */
    if (mb_x > 0 && left->available) {
        a = *left;
        a.available = 1;
        a_ref_match = (a.ref_idx == cur_ref_idx);
    }

    /* B: above neighbor */
    if (mb_y > 0 && above_row[mb_x].available) {
        b = above_row[mb_x];
        b.available = 1;
        b_ref_match = (b.ref_idx == cur_ref_idx);
    }

    /* C: above-right neighbor (not available for rightmost column) */
    if (mb_y > 0 && mb_x + 1 < mb_width && above_row[mb_x + 1].available) {
        c = above_row[mb_x + 1];
        c.available = 1;
        c_ref_match = (c.ref_idx == cur_ref_idx);
    } else if (mb_y > 0 && mb_x > 0 && above_row[mb_x - 1].available) {
        /* D: above-left as fallback */
        c = above_row[mb_x - 1];
        c.available = 1;
        c_ref_match = (c.ref_idx == cur_ref_idx);
    }

    int num_available = a.available + b.available + c.available;
    int num_ref_match = a_ref_match + b_ref_match + c_ref_match;

    if (num_available == 0) {
        *pred_mvx = 0;
        *pred_mvy = 0;
    } else if (num_available == 1) {
        /* Only one neighbor available - use it (with MV=0 if ref doesn't match) */
        if (a.available) {
            *pred_mvx = a_ref_match ? a.mv_x : 0;
            *pred_mvy = a_ref_match ? a.mv_y : 0;
        } else if (b.available) {
            *pred_mvx = b_ref_match ? b.mv_x : 0;
            *pred_mvy = b_ref_match ? b.mv_y : 0;
        } else {
            *pred_mvx = c_ref_match ? c.mv_x : 0;
            *pred_mvy = c_ref_match ? c.mv_y : 0;
        }
    } else if (num_ref_match == 1) {
        /* EXACTLY one neighbor has matching ref_idx - use that MV directly (spec 8.4.1.3.1) */
        if (a_ref_match) {
            *pred_mvx = a.mv_x;
            *pred_mvy = a.mv_y;
        } else if (b_ref_match) {
            *pred_mvx = b.mv_x;
            *pred_mvy = b.mv_y;
        } else {
            *pred_mvx = c.mv_x;
            *pred_mvy = c.mv_y;
        }
    } else {
        /* Multiple available: use median of ACTUAL neighboring MVs (spec 8.4.1.3.1)
         * Note: MVs from unavailable/intra neighbors are 0, but inter neighbors
         * with different ref_idx still use their actual MV in median calculation */
        int ax = a.available ? a.mv_x : 0;
        int ay = a.available ? a.mv_y : 0;
        int bx = b.available ? b.mv_x : 0;
        int by = b.available ? b.mv_y : 0;
        int cx = c.available ? c.mv_x : 0;
        int cy = c.available ? c.mv_y : 0;

        *pred_mvx = median3(ax, bx, cx);
        *pred_mvy = median3(ay, by, cy);
    }
}

/*
 * Write a complete P-frame for scrolling composition
 *
 * Uses P_Skip optimization: when ref_idx=0 and mvd=(0,0), the macroblock
 * can be skipped entirely - just increment mb_skip_run counter.
 */
size_t h264_write_scroll_p_frame(NALWriter *nw, H264EncoderConfig *cfg, int offset_mb) {
    uint8_t rbsp[1024 * 1024];  /* 1MB should be plenty for slice data */
    BitWriter bw;
    bitwriter_init(&bw, rbsp, sizeof(rbsp));

    /* Increment frame_num */
    int max_frame_num = 1 << cfg->log2_max_frame_num;
    int frame_num = cfg->frame_num % max_frame_num;

    /* Use waypoint-aware slice header if we have waypoints */
    if (cfg->num_waypoints > 0) {
        h264_write_p_slice_header_waypoint(&bw, cfg, 0, frame_num, frame_num * 2, 0, -1);
    } else {
        h264_write_p_slice_header(&bw, cfg, 0, frame_num, frame_num * 2, 0);
    }

    /*
     * Scroll composition logic:
     * - offset_mb = 0: show all of A
     * - offset_mb = mb_height: show all of B
     *
     * Boundary: A region is rows [0, mb_height - offset_mb)
     *           B region is rows [mb_height - offset_mb, mb_height)
     */

    int a_region_end = cfg->mb_height - offset_mb;  /* First row of B region */

    /* Find best waypoint for A region if offset exceeds direct MV limit */
    int wp_idx = -1;
    int wp_offset = 0;
    if (offset_mb > WAYPOINT_INTERVAL_MB && cfg->num_waypoints > 0) {
        for (int i = 0; i < cfg->num_waypoints; i++) {
            if (!cfg->waypoints[i].valid) continue;
            int wo = cfg->waypoints[i].offset_mb;
            if (wo <= offset_mb && wo > wp_offset) {
                int delta = offset_mb - wo;
                if (delta * 16 <= 496) {  /* Within HW limit */
                    wp_idx = i;
                    wp_offset = wo;
                }
            }
        }
    }

    /* Allocate MV tracking arrays for prediction */
    MVInfo *above_row = calloc(cfg->mb_width, sizeof(MVInfo));
    MVInfo *current_row = calloc(cfg->mb_width, sizeof(MVInfo));
    MVInfo left = {0};

    int skip_count = 0;  /* Count of consecutive P_Skip macroblocks */

    for (int mb_y = 0; mb_y < cfg->mb_height; mb_y++) {
        left.available = 0;  /* Reset left at start of each row */

        for (int mb_x = 0; mb_x < cfg->mb_width; mb_x++) {
            int ref_idx;
            int mv_y;
            int mv_x = 0;  /* No horizontal scroll */

            if (mb_y < a_region_end) {
                /* A region */
                if (wp_idx >= 0) {
                    /* Use waypoint instead of A to keep MV within HW limit */
                    ref_idx = 2 + wp_idx;  /* Waypoint ref index */
                    mv_y = (offset_mb - wp_offset) * 16;
                } else {
                    /* Use A directly (offset within limit) */
                    ref_idx = 0;
                    mv_y = offset_mb * 16;  /* Convert MB offset to pixels */
                }
            } else {
                /* B region: reference B (long-term ref with LongTermPicNum=1 = ref_idx 1) */
                ref_idx = 1;
                mv_y = (offset_mb - cfg->mb_height) * 16;  /* Negative offset in pixels */
            }

            /* MVs are in quarter-pel in the bitstream, so multiply by 4 */
            int mv_x_qpel = mv_x * 4;
            int mv_y_qpel = mv_y * 4;

            /* Get MV prediction using neighbor information */
            int pred_mvx, pred_mvy;
            get_mv_prediction_ex(mb_x, mb_y, cfg->mb_width, above_row, &left,
                                 ref_idx, &pred_mvx, &pred_mvy);

            /* Calculate MVD */
            int mvd_x = mv_x_qpel - pred_mvx;
            int mvd_y = mv_y_qpel - pred_mvy;

            /*
             * P_Skip optimization:
             * A macroblock can be skipped if:
             * - ref_idx = 0 (uses first reference)
             * - mvd = (0, 0) (MV equals prediction)
             *
             * Skipped MBs inherit ref_idx=0 and predicted MV, with no residual.
             */
            /* P_Skip disabled - decoder MV derivation differs from our prediction.
             * TODO: Investigate why P_Skip causes artifacts at region boundaries. */
            int can_skip = 0;

            /* Debug disabled - was showing MV values for high offset frames */

            if (can_skip) {
                skip_count++;
            } else {
                /* Write accumulated skip count, then the coded MB */
                bitwriter_write_ue(&bw, skip_count);
                skip_count = 0;

                /* Write the macroblock (mvd is already in quarter-pel) */
                int num_refs = 2 + cfg->num_waypoints;
                h264_write_p16x16_mb_qpel(&bw, ref_idx, mvd_x, mvd_y, num_refs);
            }

            /* Update MV tracking for next MB's prediction */
            current_row[mb_x].mv_x = mv_x_qpel;
            current_row[mb_x].mv_y = mv_y_qpel;
            current_row[mb_x].ref_idx = ref_idx;
            current_row[mb_x].available = 1;

            left = current_row[mb_x];
        }

        /* Swap rows: current becomes above for next row */
        MVInfo *tmp = above_row;
        above_row = current_row;
        current_row = tmp;
    }

    /* Write any remaining skip count at end of slice */
    if (skip_count > 0) {
        bitwriter_write_ue(&bw, skip_count);
        skip_count = 0;
    }

    free(above_row);
    free(current_row);

    bitwriter_write_trailing_bits(&bw);
    size_t rbsp_size = bitwriter_get_size(&bw);

    /* Write as non-IDR slice NAL unit */
    /* nal_ref_idc = 0 (non-reference) - keeps only I-frames in DPB */
    size_t written = nal_write_unit(nw, NAL_REF_IDC_NONE, NAL_TYPE_SLICE,
                                    rbsp, rbsp_size, 1);

    /* Increment frame_num */
    cfg->frame_num++;
    return written;
}

/*
 * Write I-slice header for IDR frame
 */
static void h264_write_idr_slice_header(BitWriter *bw, H264EncoderConfig *cfg) {
    /* first_mb_in_slice: ue(0) */
    bitwriter_write_ue(bw, 0);

    /* slice_type: ue(7) = I_ALL (all MBs in picture are I) */
    bitwriter_write_ue(bw, SLICE_TYPE_I_ALL);

    /* pic_parameter_set_id: ue(0) */
    bitwriter_write_ue(bw, 0);

    /* frame_num: u(log2_max_frame_num) = 0 for IDR */
    int frame_num_bits = cfg->log2_max_frame_num;
    bitwriter_write_bits(bw, 0, frame_num_bits);

    /* idr_pic_id: ue(v) */
    bitwriter_write_ue(bw, cfg->idr_pic_id);

    /* If poc_type == 0, write pic_order_cnt_lsb */
    if (cfg->pic_order_cnt_type == 0) {
        int poc_bits = cfg->log2_max_pic_order_cnt_lsb;
        bitwriter_write_bits(bw, 0, poc_bits);
    }
    /* poc_type 2: POC derived from frame_num, no syntax */

    /* dec_ref_pic_marking() for IDR */
    /* no_output_of_prior_pics_flag: u(1) = 0 */
    bitwriter_write_bit(bw, 0);
    /* long_term_reference_flag: u(1) = 1 (use long-term ref)
     * This marks the IDR as long-term with long_term_frame_idx=0
     * and sets max_long_term_frame_idx=0 */
    bitwriter_write_bit(bw, 1);

    /* slice_qp_delta: se(0) */
    bitwriter_write_se(bw, 0);

    /* Only write deblocking filter syntax if PPS has the flag set */
    if (cfg->deblocking_filter_control_present_flag) {
        /* disable_deblocking_filter_idc: ue(1) = disable */
        bitwriter_write_ue(bw, 1);
    }
}

/*
 * Write I-slice header for non-IDR I-frame
 */
static void h264_write_non_idr_i_slice_header(BitWriter *bw, H264EncoderConfig *cfg,
                                              int frame_num) {
    /* first_mb_in_slice: ue(0) */
    bitwriter_write_ue(bw, 0);

    /* slice_type: ue(7) = I_ALL */
    bitwriter_write_ue(bw, SLICE_TYPE_I_ALL);

    /* pic_parameter_set_id: ue(0) */
    bitwriter_write_ue(bw, 0);

    /* frame_num: u(log2_max_frame_num) */
    int frame_num_bits = cfg->log2_max_frame_num;
    bitwriter_write_bits(bw, frame_num, frame_num_bits);

    /* If poc_type == 0, write pic_order_cnt_lsb */
    if (cfg->pic_order_cnt_type == 0) {
        int poc_bits = cfg->log2_max_pic_order_cnt_lsb;
        bitwriter_write_bits(bw, frame_num * 2, poc_bits);
    }

    /* dec_ref_pic_marking() for non-IDR reference picture
     * Use adaptive mode with MMCO commands to:
     * 1. Increase max_long_term_frame_idx to 1 (MMCO 4)
     * 2. Mark current picture as long-term with idx=1 (MMCO 6)
     */
    /* adaptive_ref_pic_marking_mode_flag: u(1) = 1 */
    bitwriter_write_bit(bw, 1);

    /* MMCO 4: max_long_term_frame_idx_plus1 = 2 (allows indices 0 and 1) */
    bitwriter_write_ue(bw, 4);  /* memory_management_control_operation */
    bitwriter_write_ue(bw, 2);  /* max_long_term_frame_idx_plus1 */

    /* MMCO 6: mark current picture as long-term with long_term_frame_idx=1 */
    bitwriter_write_ue(bw, 6);  /* memory_management_control_operation */
    bitwriter_write_ue(bw, 1);  /* long_term_frame_idx */

    /* MMCO 0: end of MMCO commands */
    bitwriter_write_ue(bw, 0);

    /* slice_qp_delta: se(0) */
    bitwriter_write_se(bw, 0);

    /* Only write deblocking filter syntax if PPS has the flag set */
    if (cfg->deblocking_filter_control_present_flag) {
        /* disable_deblocking_filter_idc: ue(1) = disable */
        bitwriter_write_ue(bw, 1);
    }
}

/*
 * Write I_PCM macroblock (raw samples, no prediction)
 *
 * mb_type = 25 for I-slice (I_PCM)
 * This bypasses all prediction, which is useful for debugging.
 * Writes raw samples: 256 luma Y + 64 Cb + 64 Cr = 384 bytes
 *
 * YUV values for common colors (BT.601):
 *   Red:   Y=81,  Cb=90,  Cr=240
 *   Blue:  Y=41,  Cb=240, Cr=110
 *   Green: Y=145, Cb=54,  Cr=34
 *   Gray:  Y=128, Cb=128, Cr=128
 */
static void h264_write_ipcm_mb(BitWriter *bw, uint8_t y_val, uint8_t cb_val, uint8_t cr_val) {
    /* mb_type: ue(25) for I_PCM in I-slice */
    bitwriter_write_ue(bw, 25);

    /* pcm_alignment_zero_bit: align to byte boundary */
    while (!bitwriter_is_byte_aligned(bw)) {
        bitwriter_write_bit(bw, 0);
    }

    /* Write 256 luma samples (16x16) */
    for (int i = 0; i < 256; i++) {
        bitwriter_write_bits(bw, y_val, 8);
    }

    /* Write 64 Cb samples (8x8 for 4:2:0) */
    for (int i = 0; i < 64; i++) {
        bitwriter_write_bits(bw, cb_val, 8);
    }

    /* Write 64 Cr samples (8x8 for 4:2:0) */
    for (int i = 0; i < 64; i++) {
        bitwriter_write_bits(bw, cr_val, 8);
    }
}

/*
 * Write an IDR I-frame using I_PCM macroblocks
 *
 * y, cb, cr: YCbCr color values for the frame
 *   Default gray: y=128, cb=128, cr=128
 *   Red (BT.601): y=81, cb=90, cr=240
 *   Blue:         y=41, cb=240, cr=110
 */
size_t h264_write_idr_frame_color(NALWriter *nw, H264EncoderConfig *cfg,
                                   uint8_t y, uint8_t cb, uint8_t cr) {
    /* I_PCM needs 384 bytes per MB + header overhead */
    size_t rbsp_capacity = cfg->mb_width * cfg->mb_height * 400 + 1024;
    uint8_t *rbsp = malloc(rbsp_capacity);
    BitWriter bw;
    bitwriter_init(&bw, rbsp, rbsp_capacity);

    /* Reset frame_num for IDR */
    cfg->frame_num = 0;

    /* Write slice header */
    h264_write_idr_slice_header(&bw, cfg);

    /* Write all macroblocks using I_PCM */
    int total_mbs = cfg->mb_width * cfg->mb_height;
    for (int i = 0; i < total_mbs; i++) {
        h264_write_ipcm_mb(&bw, y, cb, cr);
    }

    bitwriter_write_trailing_bits(&bw);
    size_t rbsp_size = bitwriter_get_size(&bw);

    /* Write as IDR NAL unit */
    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_IDR,
                                    rbsp, rbsp_size, 1);

    free(rbsp);
    cfg->frame_num = 1;  /* Next frame is frame_num=1 */
    return written;
}

/* Backward-compatible wrapper with default gray */
size_t h264_write_idr_frame(NALWriter *nw, H264EncoderConfig *cfg) {
    return h264_write_idr_frame_color(nw, cfg, 128, 128, 128);
}

/*
 * Write an IDR I-frame with 3 horizontal color stripes
 * Colors are for top, middle, bottom thirds of frame
 */
size_t h264_write_idr_frame_striped(NALWriter *nw, H264EncoderConfig *cfg,
                                     uint8_t y1, uint8_t cb1, uint8_t cr1,
                                     uint8_t y2, uint8_t cb2, uint8_t cr2,
                                     uint8_t y3, uint8_t cb3, uint8_t cr3) {
    size_t rbsp_capacity = cfg->mb_width * cfg->mb_height * 400 + 1024;
    uint8_t *rbsp = malloc(rbsp_capacity);
    BitWriter bw;
    bitwriter_init(&bw, rbsp, rbsp_capacity);

    cfg->frame_num = 0;
    h264_write_idr_slice_header(&bw, cfg);

    int third = cfg->mb_height / 3;
    for (int mb_y = 0; mb_y < cfg->mb_height; mb_y++) {
        uint8_t y, cb, cr;
        if (mb_y < third) {
            y = y1; cb = cb1; cr = cr1;  /* Top third */
        } else if (mb_y < 2 * third) {
            y = y2; cb = cb2; cr = cr2;  /* Middle third */
        } else {
            y = y3; cb = cb3; cr = cr3;  /* Bottom third */
        }
        for (int mb_x = 0; mb_x < cfg->mb_width; mb_x++) {
            h264_write_ipcm_mb(&bw, y, cb, cr);
        }
    }

    bitwriter_write_trailing_bits(&bw);
    size_t rbsp_size = bitwriter_get_size(&bw);
    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_IDR,
                                    rbsp, rbsp_size, 1);
    free(rbsp);
    cfg->frame_num = 1;
    return written;
}

/*
 * Write a non-IDR I-frame using I_PCM macroblocks
 *
 * y, cb, cr: YCbCr color values for the frame
 */
size_t h264_write_non_idr_i_frame_color(NALWriter *nw, H264EncoderConfig *cfg,
                                         uint8_t y, uint8_t cb, uint8_t cr) {
    /* I_PCM needs 384 bytes per MB + header overhead */
    size_t rbsp_capacity = cfg->mb_width * cfg->mb_height * 400 + 1024;
    uint8_t *rbsp = malloc(rbsp_capacity);
    BitWriter bw;
    bitwriter_init(&bw, rbsp, rbsp_capacity);

    int frame_num = cfg->frame_num;

    /* Write slice header */
    h264_write_non_idr_i_slice_header(&bw, cfg, frame_num);

    /* Write all macroblocks using I_PCM */
    int total_mbs = cfg->mb_width * cfg->mb_height;
    for (int i = 0; i < total_mbs; i++) {
        h264_write_ipcm_mb(&bw, y, cb, cr);
    }

    bitwriter_write_trailing_bits(&bw);
    size_t rbsp_size = bitwriter_get_size(&bw);

    /* Write as non-IDR slice NAL unit (but still a reference) */
    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_SLICE,
                                    rbsp, rbsp_size, 1);

    free(rbsp);
    cfg->frame_num++;
    return written;
}

/* Backward-compatible wrapper with default lighter gray */
size_t h264_write_non_idr_i_frame(NALWriter *nw, H264EncoderConfig *cfg) {
    return h264_write_non_idr_i_frame_color(nw, cfg, 200, 128, 128);
}

/*
 * Write a non-IDR I-frame with 3 horizontal color stripes
 */
size_t h264_write_non_idr_i_frame_striped(NALWriter *nw, H264EncoderConfig *cfg,
                                           uint8_t y1, uint8_t cb1, uint8_t cr1,
                                           uint8_t y2, uint8_t cb2, uint8_t cr2,
                                           uint8_t y3, uint8_t cb3, uint8_t cr3) {
    size_t rbsp_capacity = cfg->mb_width * cfg->mb_height * 400 + 1024;
    uint8_t *rbsp = malloc(rbsp_capacity);
    BitWriter bw;
    bitwriter_init(&bw, rbsp, rbsp_capacity);

    int frame_num = cfg->frame_num;
    h264_write_non_idr_i_slice_header(&bw, cfg, frame_num);

    int third = cfg->mb_height / 3;
    for (int mb_y = 0; mb_y < cfg->mb_height; mb_y++) {
        uint8_t y, cb, cr;
        if (mb_y < third) {
            y = y1; cb = cb1; cr = cr1;
        } else if (mb_y < 2 * third) {
            y = y2; cb = cb2; cr = cr2;
        } else {
            y = y3; cb = cb3; cr = cr3;
        }
        for (int mb_x = 0; mb_x < cfg->mb_width; mb_x++) {
            h264_write_ipcm_mb(&bw, y, cb, cr);
        }
    }

    bitwriter_write_trailing_bits(&bw);
    size_t rbsp_size = bitwriter_get_size(&bw);
    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_SLICE,
                                    rbsp, rbsp_size, 1);
    free(rbsp);
    cfg->frame_num++;
    return written;
}

/*
 * Parsed slice header info
 */
typedef struct {
    size_t mb_data_start_bit;
    int32_t slice_qp_delta;
    uint32_t disable_deblocking_filter_idc;
    int32_t slice_alpha_c0_offset_div2;
    int32_t slice_beta_offset_div2;
} ParsedSliceHeader;

/*
 * Parse x264's IDR slice header to find where MB data starts.
 *
 * Returns: 1 on success, 0 on error
 */
static int parse_idr_slice_header(const uint8_t *rbsp, size_t rbsp_size,
                                   H264EncoderConfig *cfg, ParsedSliceHeader *hdr) {
    BitReader br;
    bitreader_init(&br, rbsp, rbsp_size);

    memset(hdr, 0, sizeof(*hdr));

    /* first_mb_in_slice: ue(v) */
    uint32_t first_mb = bitreader_read_ue(&br);
    (void)first_mb;  /* Should be 0 */

    /* slice_type: ue(v) */
    uint32_t slice_type = bitreader_read_ue(&br);
    (void)slice_type;  /* Should be 2 or 7 for I-slice */

    /* pic_parameter_set_id: ue(v) */
    uint32_t pps_id = bitreader_read_ue(&br);
    (void)pps_id;

    /* frame_num: u(log2_max_frame_num) */
    bitreader_read_bits(&br, cfg->log2_max_frame_num);

    /* For IDR: idr_pic_id: ue(v) */
    bitreader_read_ue(&br);

    /* POC syntax depends on pic_order_cnt_type */
    if (cfg->pic_order_cnt_type == 0) {
        /* pic_order_cnt_lsb: u(log2_max_pic_order_cnt_lsb) */
        bitreader_read_bits(&br, cfg->log2_max_pic_order_cnt_lsb);
    }
    /* poc_type 2: no extra syntax */

    /* dec_ref_pic_marking() for IDR */
    /* no_output_of_prior_pics_flag: u(1) */
    bitreader_read_bit(&br);
    /* long_term_reference_flag: u(1) */
    bitreader_read_bit(&br);

    /* slice_qp_delta: se(v) - PRESERVE THIS */
    hdr->slice_qp_delta = bitreader_read_se(&br);

    /* Deblocking filter syntax if present */
    if (cfg->deblocking_filter_control_present_flag) {
        hdr->disable_deblocking_filter_idc = bitreader_read_ue(&br);
        if (hdr->disable_deblocking_filter_idc != 1) {
            hdr->slice_alpha_c0_offset_div2 = bitreader_read_se(&br);
            hdr->slice_beta_offset_div2 = bitreader_read_se(&br);
        }
    }

    hdr->mb_data_start_bit = bitreader_get_bit_position(&br);
    return 1;
}

/*
 * Copy bits from source to destination, handling bit alignment
 */
static void copy_bits(BitWriter *bw, const uint8_t *src, size_t src_size,
                       size_t start_bit, size_t num_bits) {
    BitReader br;
    bitreader_init(&br, src, src_size);

    /* Skip to start position */
    for (size_t i = 0; i < start_bit; i++) {
        bitreader_read_bit(&br);
    }

    /* Copy bits */
    for (size_t i = 0; i < num_bits; i++) {
        bitwriter_write_bit(bw, bitreader_read_bit(&br));
    }
}

/*
 * Extended rewrite: uses parse_cfg for parsing x264's header, write_cfg for output
 */
size_t h264_rewrite_idr_frame_ex(NALWriter *nw, H264EncoderConfig *write_cfg,
                                  H264EncoderConfig *parse_cfg,
                                  const uint8_t *rbsp, size_t rbsp_size) {
    /* Parse x264's slice header */
    ParsedSliceHeader hdr;
    if (!parse_idr_slice_header(rbsp, rbsp_size, parse_cfg, &hdr)) {
        return 0;
    }

    size_t total_bits = rbsp_size * 8;
    size_t mb_data_bits = total_bits - hdr.mb_data_start_bit;

    size_t out_capacity = rbsp_size + 256;
    uint8_t *out_rbsp = malloc(out_capacity);
    BitWriter bw;
    bitwriter_init(&bw, out_rbsp, out_capacity);

    /* Write our IDR slice header using write_cfg params */
    bitwriter_write_ue(&bw, 0);  /* first_mb_in_slice */
    bitwriter_write_ue(&bw, SLICE_TYPE_I_ALL);  /* slice_type */
    bitwriter_write_ue(&bw, 0);  /* pps_id */
    bitwriter_write_bits(&bw, 0, write_cfg->log2_max_frame_num);  /* frame_num */
    bitwriter_write_ue(&bw, write_cfg->idr_pic_id);  /* idr_pic_id */

    if (write_cfg->pic_order_cnt_type == 0) {
        bitwriter_write_bits(&bw, 0, write_cfg->log2_max_pic_order_cnt_lsb);
    }

    /* dec_ref_pic_marking: long_term_reference_flag = 1 */
    bitwriter_write_bit(&bw, 0);  /* no_output_of_prior_pics_flag */
    bitwriter_write_bit(&bw, 1);  /* long_term_reference_flag = 1 */

    /* PRESERVE x264's slice_qp_delta */
    bitwriter_write_se(&bw, hdr.slice_qp_delta);

    /* PRESERVE x264's deblocking filter settings */
    if (write_cfg->deblocking_filter_control_present_flag) {
        bitwriter_write_ue(&bw, hdr.disable_deblocking_filter_idc);
        if (hdr.disable_deblocking_filter_idc != 1) {
            bitwriter_write_se(&bw, hdr.slice_alpha_c0_offset_div2);
            bitwriter_write_se(&bw, hdr.slice_beta_offset_div2);
        }
    }

    /* Copy x264's MB data */
    copy_bits(&bw, rbsp, rbsp_size, hdr.mb_data_start_bit, mb_data_bits);

    size_t out_size = bitwriter_get_size(&bw);

    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_IDR,
                                    out_rbsp, out_size, 1);

    free(out_rbsp);
    write_cfg->frame_num = 1;
    return written;
}

size_t h264_rewrite_as_non_idr_i_frame_ex(NALWriter *nw, H264EncoderConfig *write_cfg,
                                           H264EncoderConfig *parse_cfg,
                                           const uint8_t *rbsp, size_t rbsp_size,
                                           int frame_num) {
    /* Parse x264's slice header */
    ParsedSliceHeader hdr;
    if (!parse_idr_slice_header(rbsp, rbsp_size, parse_cfg, &hdr)) {
        return 0;
    }

    size_t total_bits = rbsp_size * 8;
    size_t mb_data_bits = total_bits - hdr.mb_data_start_bit;

    size_t out_capacity = rbsp_size + 256;
    uint8_t *out_rbsp = malloc(out_capacity);
    BitWriter bw;
    bitwriter_init(&bw, out_rbsp, out_capacity);

    /* Write non-IDR I-slice header using write_cfg params */
    bitwriter_write_ue(&bw, 0);  /* first_mb_in_slice */
    bitwriter_write_ue(&bw, SLICE_TYPE_I_ALL);  /* slice_type */
    bitwriter_write_ue(&bw, 0);  /* pps_id */
    bitwriter_write_bits(&bw, frame_num, write_cfg->log2_max_frame_num);  /* frame_num */
    /* NO idr_pic_id for non-IDR */

    if (write_cfg->pic_order_cnt_type == 0) {
        bitwriter_write_bits(&bw, frame_num * 2, write_cfg->log2_max_pic_order_cnt_lsb);
    }

    /* dec_ref_pic_marking with MMCO commands */
    bitwriter_write_bit(&bw, 1);  /* adaptive_ref_pic_marking_mode_flag */
    bitwriter_write_ue(&bw, 4);   /* MMCO 4 */
    bitwriter_write_ue(&bw, 2);   /* max_long_term_frame_idx_plus1 = 2 */
    bitwriter_write_ue(&bw, 6);   /* MMCO 6 */
    bitwriter_write_ue(&bw, 1);   /* long_term_frame_idx = 1 */
    bitwriter_write_ue(&bw, 0);   /* MMCO 0 (end) */

    /* PRESERVE x264's slice_qp_delta */
    bitwriter_write_se(&bw, hdr.slice_qp_delta);

    /* PRESERVE x264's deblocking filter settings */
    if (write_cfg->deblocking_filter_control_present_flag) {
        bitwriter_write_ue(&bw, hdr.disable_deblocking_filter_idc);
        if (hdr.disable_deblocking_filter_idc != 1) {
            bitwriter_write_se(&bw, hdr.slice_alpha_c0_offset_div2);
            bitwriter_write_se(&bw, hdr.slice_beta_offset_div2);
        }
    }

    /* Copy x264's MB data */
    copy_bits(&bw, rbsp, rbsp_size, hdr.mb_data_start_bit, mb_data_bits);

    size_t out_size = bitwriter_get_size(&bw);
    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_SLICE,
                                    out_rbsp, out_size, 1);

    free(out_rbsp);
    write_cfg->frame_num = frame_num + 1;
    return written;
}

/*
 * Rewrite x264 IDR frame with our slice header (uses same cfg for parse and write)
 */
size_t h264_rewrite_idr_frame(NALWriter *nw, H264EncoderConfig *cfg,
                               const uint8_t *rbsp, size_t rbsp_size) {
    return h264_rewrite_idr_frame_ex(nw, cfg, cfg, rbsp, rbsp_size);
}

/*
 * Rewrite x264 IDR as non-IDR I-frame with MMCO commands (uses same cfg)
 */
size_t h264_rewrite_as_non_idr_i_frame(NALWriter *nw, H264EncoderConfig *cfg,
                                        const uint8_t *rbsp, size_t rbsp_size,
                                        int frame_num) {
    return h264_rewrite_as_non_idr_i_frame_ex(nw, cfg, cfg, rbsp, rbsp_size, frame_num);
}

/* ============================================================================
 * WAYPOINT SUPPORT FOR EXTENDED SCROLL RANGE
 * ============================================================================
 *
 * Hardware decoders (NVDEC, VAAPI) limit vertical MVs to 512 pixels (2048 qpel).
 * Waypoints are intermediate P-frames marked as long-term references that allow
 * subsequent frames to use smaller MVs.
 *
 * Example for 720p (45 MB):
 *   - Scroll 0-31: Reference A directly (MV 0-496px)
 *   - Scroll 31: Create waypoint (long-term ref 2)
 *   - Scroll 32-45: Reference waypoint (MV 16-224px) instead of A (512-720px)
 */

/* Hardware MV limit: 31 MB = 496 pixels = 1984 qpel (safely under 2048) */
#define WAYPOINT_INTERVAL_MB 31

/*
 * Check if a waypoint is needed at the given scroll offset
 */
int h264_needs_waypoint(H264EncoderConfig *cfg, int offset_mb) {
    /* Need waypoint at multiples of WAYPOINT_INTERVAL_MB */
    if (offset_mb == 0) return 0;  /* Never at offset 0 */
    if (offset_mb % WAYPOINT_INTERVAL_MB != 0) return 0;

    /* Check if we already have a waypoint at this offset */
    for (int i = 0; i < cfg->num_waypoints; i++) {
        if (cfg->waypoints[i].valid && cfg->waypoints[i].offset_mb == offset_mb) {
            return 0;  /* Already have this waypoint */
        }
    }

    return 1;
}

/*
 * Find the best waypoint to use for a given scroll offset
 * Returns the waypoint index, or -1 if should use original refs
 */
static int find_best_waypoint(H264EncoderConfig *cfg, int offset_mb) {
    int best_idx = -1;
    int best_offset = 0;

    /* Find the highest waypoint offset that's <= current offset and within MV range */
    for (int i = 0; i < cfg->num_waypoints; i++) {
        if (!cfg->waypoints[i].valid) continue;

        int wp_offset = cfg->waypoints[i].offset_mb;
        if (wp_offset <= offset_mb && wp_offset > best_offset) {
            /* Check if using this waypoint keeps MV within limit */
            int delta = offset_mb - wp_offset;
            if (delta * 16 <= 496) {  /* 496px = 31 MB, within HW limit */
                best_idx = i;
                best_offset = wp_offset;
            }
        }
    }

    return best_idx;
}

/*
 * Write P-slice header with waypoint support
 *
 * num_refs: 2 (A, B) or 3+ (A, B, waypoints)
 * For waypoint frames, is_reference=1 and writes MMCO to mark as long-term
 */
static void h264_write_p_slice_header_waypoint(BitWriter *bw, H264EncoderConfig *cfg,
                                                int first_mb, int frame_num, int poc_lsb,
                                                int is_reference, int long_term_idx) {
    /* first_mb_in_slice: ue(v) */
    bitwriter_write_ue(bw, first_mb);

    /* slice_type: ue(v) = 0 (P-slice) */
    bitwriter_write_ue(bw, SLICE_TYPE_P);

    /* pic_parameter_set_id: ue(0) */
    bitwriter_write_ue(bw, 0);

    /* frame_num: u(log2_max_frame_num) */
    int frame_num_bits = cfg->log2_max_frame_num;
    bitwriter_write_bits(bw, frame_num & ((1 << frame_num_bits) - 1), frame_num_bits);

    /* If pic_order_cnt_type == 0, write POC LSB */
    if (cfg->pic_order_cnt_type == 0) {
        int poc_bits = cfg->log2_max_pic_order_cnt_lsb;
        bitwriter_write_bits(bw, poc_lsb & ((1 << poc_bits) - 1), poc_bits);
    }

    /* num_ref_idx_active_override_flag: u(1) = 1 */
    bitwriter_write_bit(bw, 1);

    /* num_ref_idx_l0_active_minus1: number of refs - 1 */
    int num_refs = 2 + cfg->num_waypoints;  /* A, B, plus waypoints */
    bitwriter_write_ue(bw, num_refs - 1);

    /* ref_pic_list_modification */
    bitwriter_write_bit(bw, 1);  /* flag = 1 */

    /* Build ref list: A(ltp=0), B(ltp=1), waypoints(ltp=2,3,...) */
    bitwriter_write_ue(bw, 2);  /* modification_of_pic_nums_idc = 2 */
    bitwriter_write_ue(bw, 0);  /* long_term_pic_num = 0 (A) */

    bitwriter_write_ue(bw, 2);
    bitwriter_write_ue(bw, 1);  /* long_term_pic_num = 1 (B) */

    /* Add waypoints to ref list */
    for (int i = 0; i < cfg->num_waypoints; i++) {
        if (cfg->waypoints[i].valid) {
            bitwriter_write_ue(bw, 2);
            bitwriter_write_ue(bw, cfg->waypoints[i].long_term_idx);
        }
    }

    bitwriter_write_ue(bw, 3);  /* End of modification */

    /* dec_ref_pic_marking() */
    if (is_reference) {
        if (long_term_idx >= 0) {
            /* Mark this frame as long-term reference using MMCO */
            bitwriter_write_bit(bw, 1);  /* adaptive_ref_pic_marking_mode_flag = 1 */

            /* MMCO 4: set max_long_term_frame_idx to allow this index
             * max_long_term_frame_idx_plus1 = long_term_idx + 1
             * This extends the allowed range of long-term indices */
            bitwriter_write_ue(bw, 4);   /* memory_management_control_operation = 4 */
            bitwriter_write_ue(bw, long_term_idx + 1);  /* max_long_term_frame_idx_plus1 */

            /* MMCO 6: mark current as long-term with given idx */
            bitwriter_write_ue(bw, 6);   /* memory_management_control_operation = 6 */
            bitwriter_write_ue(bw, long_term_idx);  /* long_term_frame_idx */

            /* MMCO 0: end of MMCO commands */
            bitwriter_write_ue(bw, 0);
        } else {
            /* Sliding window reference management */
            bitwriter_write_bit(bw, 0);
        }
    }

    /* slice_qp_delta: se(0) */
    bitwriter_write_se(bw, 0);

    /* deblocking filter */
    if (cfg->deblocking_filter_control_present_flag) {
        bitwriter_write_ue(bw, 1);  /* disable deblocking */
    }
}

/*
 * Write a waypoint P-frame
 *
 * This creates a P-frame at the given scroll offset and marks it as
 * a long-term reference for subsequent frames to use.
 */
size_t h264_write_waypoint_p_frame(NALWriter *nw, H264EncoderConfig *cfg, int offset_mb) {
    uint8_t rbsp[1024 * 1024];
    BitWriter bw;
    bitwriter_init(&bw, rbsp, sizeof(rbsp));

    int max_frame_num = 1 << cfg->log2_max_frame_num;
    int frame_num = cfg->frame_num % max_frame_num;

    /* Assign long-term index for this waypoint (starting at 2, since A=0, B=1) */
    int long_term_idx = 2 + cfg->num_waypoints;

    /* Write slice header with MMCO to mark as long-term */
    h264_write_p_slice_header_waypoint(&bw, cfg, 0, frame_num, frame_num * 2,
                                        1, long_term_idx);  /* is_reference=1 */

    /* Write macroblock data (same as regular scroll frame) */
    int a_region_end = cfg->mb_height - offset_mb;

    MVInfo *above_row = calloc(cfg->mb_width, sizeof(MVInfo));
    MVInfo *current_row = calloc(cfg->mb_width, sizeof(MVInfo));
    MVInfo left = {0};
    int skip_count = 0;

    /* For waypoint, we may need to use an existing waypoint for the A region */
    int wp_idx = find_best_waypoint(cfg, offset_mb);

    for (int mb_y = 0; mb_y < cfg->mb_height; mb_y++) {
        left.available = 0;

        for (int mb_x = 0; mb_x < cfg->mb_width; mb_x++) {
            int ref_idx;
            int mv_y;
            int mv_x = 0;

            if (mb_y < a_region_end) {
                /* A region */
                if (wp_idx >= 0 && offset_mb > WAYPOINT_INTERVAL_MB) {
                    /* Use waypoint instead of A */
                    int wp_offset = cfg->waypoints[wp_idx].offset_mb;
                    ref_idx = 2 + wp_idx;  /* Waypoint ref index */
                    mv_y = (offset_mb - wp_offset) * 16;
                } else {
                    /* Use A directly */
                    ref_idx = 0;
                    mv_y = offset_mb * 16;
                }
            } else {
                /* B region */
                ref_idx = 1;
                mv_y = (offset_mb - cfg->mb_height) * 16;
            }

            int mv_x_qpel = mv_x * 4;
            int mv_y_qpel = mv_y * 4;

            int pred_mvx, pred_mvy;
            get_mv_prediction_ex(mb_x, mb_y, cfg->mb_width, above_row, &left,
                                 ref_idx, &pred_mvx, &pred_mvy);

            int mvd_x = mv_x_qpel - pred_mvx;
            int mvd_y = mv_y_qpel - pred_mvy;

            bitwriter_write_ue(&bw, skip_count);
            skip_count = 0;
            /* num_refs includes existing waypoints (not the one being created) */
            int num_refs = 2 + cfg->num_waypoints;
            h264_write_p16x16_mb_qpel(&bw, ref_idx, mvd_x, mvd_y, num_refs);

            current_row[mb_x].mv_x = mv_x_qpel;
            current_row[mb_x].mv_y = mv_y_qpel;
            current_row[mb_x].ref_idx = ref_idx;
            current_row[mb_x].available = 1;
            left = current_row[mb_x];
        }

        MVInfo *tmp = above_row;
        above_row = current_row;
        current_row = tmp;
    }

    if (skip_count > 0) {
        bitwriter_write_ue(&bw, skip_count);
    }

    free(above_row);
    free(current_row);

    bitwriter_write_trailing_bits(&bw);
    size_t rbsp_size = bitwriter_get_size(&bw);

    /* Write as reference P-frame (nal_ref_idc = 2) */
    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGH, NAL_TYPE_SLICE,
                                    rbsp, rbsp_size, 1);

    /* Register this waypoint */
    if (cfg->num_waypoints < MAX_WAYPOINTS) {
        cfg->waypoints[cfg->num_waypoints].offset_mb = offset_mb;
        cfg->waypoints[cfg->num_waypoints].long_term_idx = long_term_idx;
        cfg->waypoints[cfg->num_waypoints].valid = 1;
        cfg->num_waypoints++;
    }

    cfg->frame_num++;
    return written;
}
