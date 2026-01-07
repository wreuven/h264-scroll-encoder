#include "h264_encoder.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

void h264_encoder_init(H264EncoderConfig *cfg, int width, int height) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->width = width;
    cfg->height = height;
    cfg->mb_width = width / 16;
    cfg->mb_height = height / 16;
    cfg->frame_num = 0;
    cfg->idr_pic_id = 0;

    /* Defaults - will be overridden if loading from external SPS */
    cfg->log2_max_frame_num = 9;        /* max_frame_num = 512 (enough for 20s at 25fps) */
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
    /* Set constraint_set0_flag = 1 (Baseline compatible) */
    bitwriter_write_bits(&bw, 0x80, 8);

    /* level_idc: 40 = Level 4.0 */
    bitwriter_write_bits(&bw, 40, 8);

    /* seq_parameter_set_id: ue(0) */
    bitwriter_write_ue(&bw, 0);

    /* log2_max_frame_num_minus4: ue(5) -> log2=9 -> max_frame_num = 512 */
    bitwriter_write_ue(&bw, 5);

    /* pic_order_cnt_type: ue(2) -> POC = 2*frame_num for frames */
    bitwriter_write_ue(&bw, 2);

    /* max_num_ref_frames: ue(2) -> we need 2 refs (A and B) */
    bitwriter_write_ue(&bw, 2);

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
    /* ref_pic_list_modification_flag_l0: u(1) = 0 */
    /* We rely on default ordering - DPB should have A at 0, B at 1 */
    bitwriter_write_bit(bw, 0);

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
 * mvd_l0 = motion vector delta
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

    /* coded_block_pattern is NOT written for P_L0_16x16 with no residual */
    /* Instead, mb_type implicitly signals no CBP when it's the base type */

    /* Actually, for P_L0_16x16, we need to write coded_block_pattern */
    /* coded_block_pattern: me(v) = 0 (no luma or chroma residual) */
    /* For inter MBs, CBP 0 is coded as me(0) */
    /* me(v) for inter is mapped differently - CBP 0 -> codeNum 0 */
    bitwriter_write_ue(bw, 0);  /* CBP = 0, no residual */

    /* No mb_qp_delta since CBP = 0 */
    /* No residual data since CBP = 0 */
}

/*
 * Motion vector prediction for P16x16
 *
 * Simplified: for the first MB or when neighbors have different refs,
 * we often get (0,0) prediction. For a clean implementation, we compute
 * the median of A (left), B (above), C (above-right) MVs.
 *
 * For simplicity in this generator, we use the fact that:
 * - All our MBs use the same MV pattern per row
 * - We can compute expected prediction and write the delta
 */
static void get_mv_prediction(H264EncoderConfig *cfg, int mb_x, int mb_y,
                              int ref_idx, int *pred_mvx, int *pred_mvy,
                              int offset_mb) {
    /*
     * Simplified prediction:
     * - First MB (0,0): prediction is (0,0)
     * - Left edge (mb_x == 0): use above if available
     * - Others: median of left, above, above-right
     *
     * Since all our P-frames have uniform motion per row,
     * the prediction often matches our MV, giving delta of 0.
     *
     * For now, assume prediction is (0,0) and write full MV as delta.
     * This is valid but less efficient. Can be optimized later.
     */
    (void)cfg;
    (void)mb_x;
    (void)mb_y;
    (void)ref_idx;
    (void)offset_mb;

    *pred_mvx = 0;
    *pred_mvy = 0;
}

/*
 * Write a complete P-frame for scrolling composition
 */
size_t h264_write_scroll_p_frame(NALWriter *nw, H264EncoderConfig *cfg, int offset_mb) {
    uint8_t rbsp[1024 * 1024];  /* 1MB should be plenty for slice data */
    BitWriter bw;
    bitwriter_init(&bw, rbsp, sizeof(rbsp));

    /* Increment frame_num */
    int max_frame_num = 1 << cfg->log2_max_frame_num;
    int frame_num = cfg->frame_num % max_frame_num;

    /* Write slice header - is_reference=0 for non-reference P-frames */
    h264_write_p_slice_header(&bw, cfg, 0, frame_num, frame_num * 2, 0);

    /*
     * Scroll composition logic:
     *
     * With "B scrolls in" behavior:
     * - Image A is conceptually above image B in a tall virtual canvas
     * - offset_mb = 0: show all of A
     * - offset_mb = mb_height: show all of B
     *
     * For each output row mb_y:
     *   source_row = mb_y + offset_mb
     *   if source_row < mb_height:
     *     use ref A at row source_row
     *     MV_y = (source_row - mb_y) * 16 = offset_mb * 16
     *   else:
     *     use ref B at row (source_row - mb_height)
     *     MV_y = (source_row - mb_height - mb_y) * 16 = (offset_mb - mb_height) * 16
     *
     * Since we want the content to scroll UP (A moves up, B appears from below):
     * - MV_y for A region: -offset_mb * 16 (negative = content from higher Y)
     * - Wait, let me reconsider...
     *
     * Actually for motion compensation:
     * - reconstructed[x,y] = reference[x + mvx, y + mvy]
     * - If we want output row 0 to show reference row offset_mb:
     *   MV_y = offset_mb (positive)
     *
     * Let me define clearly:
     * - offset_mb = 0: output shows A as-is
     * - offset_mb = N: output row 0 shows A row N, output row (mb_height-N-1) shows A row (mb_height-1)
     *                  output row (mb_height-N) shows B row 0, etc.
     *
     * Boundary: A region is rows [0, mb_height - offset_mb)
     *           B region is rows [mb_height - offset_mb, mb_height)
     */

    int a_region_end = cfg->mb_height - offset_mb;  /* First row of B region */

    for (int mb_y = 0; mb_y < cfg->mb_height; mb_y++) {
        for (int mb_x = 0; mb_x < cfg->mb_width; mb_x++) {
            int ref_idx;
            int mv_y;
            int mv_x = 0;  /* No horizontal scroll */

            if (mb_y < a_region_end) {
                /* A region: reference A (IDR, older in DPB = ref_idx 1) */
                /* Fetch from row (mb_y + offset_mb) */
                ref_idx = 1;
                mv_y = offset_mb * 16;  /* Convert MB offset to pixels */
            } else {
                /* B region: reference B (non-IDR I-frame, most recent = ref_idx 0) */
                /* Fetch from row (mb_y - a_region_end) */
                ref_idx = 0;
                mv_y = (offset_mb - cfg->mb_height) * 16;  /* Negative offset in pixels */
            }

            /* Get MV prediction (currently returns 0,0) */
            int pred_mvx, pred_mvy;
            get_mv_prediction(cfg, mb_x, mb_y, ref_idx, &pred_mvx, &pred_mvy, offset_mb);

            /* Write MB with delta from prediction */
            int mvd_x = mv_x - pred_mvx;
            int mvd_y = mv_y - pred_mvy;

            /* For CAVLC P-slices, must write mb_skip_run before each macroblock */
            /* mb_skip_run = 0 means "no skipped MBs, next is a coded MB" */
            bitwriter_write_ue(&bw, 0);

            /* Now write the macroblock */
            h264_write_p16x16_mb(&bw, ref_idx, mvd_x, mvd_y);
        }
    }

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
    /* long_term_reference_flag: u(1) = 0 (use short-term ref) */
    bitwriter_write_bit(bw, 0);

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

    /* dec_ref_pic_marking() for non-IDR */
    /* adaptive_ref_pic_marking_mode_flag: u(1) = 0 (sliding window) */
    bitwriter_write_bit(bw, 0);

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
 */
static void h264_write_ipcm_mb(BitWriter *bw, uint8_t luma_val, uint8_t chroma_val) {
    /* mb_type: ue(25) for I_PCM in I-slice */
    bitwriter_write_ue(bw, 25);

    /* pcm_alignment_zero_bit: align to byte boundary */
    while (!bitwriter_is_byte_aligned(bw)) {
        bitwriter_write_bit(bw, 0);
    }

    /* Write 256 luma samples (16x16) */
    for (int i = 0; i < 256; i++) {
        bitwriter_write_bits(bw, luma_val, 8);
    }

    /* Write 64 Cb samples (8x8 for 4:2:0) */
    for (int i = 0; i < 64; i++) {
        bitwriter_write_bits(bw, chroma_val, 8);
    }

    /* Write 64 Cr samples (8x8 for 4:2:0) */
    for (int i = 0; i < 64; i++) {
        bitwriter_write_bits(bw, chroma_val, 8);
    }
}

/*
 * Write an IDR I-frame using I_PCM macroblocks
 * luma_val = 128 for gray, chroma_val = 128 for neutral
 */
size_t h264_write_idr_frame(NALWriter *nw, H264EncoderConfig *cfg) {
    /* I_PCM needs 384 bytes per MB + header overhead */
    size_t rbsp_capacity = cfg->mb_width * cfg->mb_height * 400 + 1024;
    uint8_t *rbsp = malloc(rbsp_capacity);
    BitWriter bw;
    bitwriter_init(&bw, rbsp, rbsp_capacity);

    /* Reset frame_num for IDR */
    cfg->frame_num = 0;

    /* Write slice header */
    h264_write_idr_slice_header(&bw, cfg);

    /* Write all macroblocks using I_PCM (gray for frame A) */
    int total_mbs = cfg->mb_width * cfg->mb_height;
    for (int i = 0; i < total_mbs; i++) {
        h264_write_ipcm_mb(&bw, 128, 128);  /* Mid-gray */
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

/*
 * Write a non-IDR I-frame using I_PCM macroblocks
 */
size_t h264_write_non_idr_i_frame(NALWriter *nw, H264EncoderConfig *cfg) {
    /* I_PCM needs 384 bytes per MB + header overhead */
    size_t rbsp_capacity = cfg->mb_width * cfg->mb_height * 400 + 1024;
    uint8_t *rbsp = malloc(rbsp_capacity);
    BitWriter bw;
    bitwriter_init(&bw, rbsp, rbsp_capacity);

    int frame_num = cfg->frame_num;

    /* Write slice header */
    h264_write_non_idr_i_slice_header(&bw, cfg, frame_num);

    /* Write all macroblocks using I_PCM (different gray for frame B) */
    int total_mbs = cfg->mb_width * cfg->mb_height;
    for (int i = 0; i < total_mbs; i++) {
        h264_write_ipcm_mb(&bw, 200, 128);  /* Lighter gray to distinguish from A */
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
