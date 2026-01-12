#include "h264_writer.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* Forward declarations */
static void h264_write_p_slice_header(BitWriter *bw, ComposerConfig *cfg,
                                       int frame_num, int poc_lsb, int is_reference);
static void h264_write_p_slice_header_waypoint(BitWriter *bw, ComposerConfig *cfg,
                                                int frame_num, int poc_lsb,
                                                int is_reference, int long_term_idx);

void composer_config_init(ComposerConfig *cfg, int width, int height) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->width = width;
    cfg->height = height;
    cfg->mb_width = width / 16;
    cfg->mb_height = height / 16;
    cfg->frame_num = 0;
    cfg->idr_pic_id = 0;

    /* Defaults - will be overridden when parsing external SPS */
    cfg->log2_max_frame_num = 4;
    cfg->pic_order_cnt_type = 2;
    cfg->log2_max_pic_order_cnt_lsb = 4;
    cfg->num_ref_idx_l0_default_minus1 = 1;
    cfg->deblocking_filter_control_present_flag = 1;
}

void composer_config_set_sps_params(ComposerConfig *cfg,
                                     int log2_max_frame_num,
                                     int pic_order_cnt_type,
                                     int log2_max_pic_order_cnt_lsb) {
    cfg->log2_max_frame_num = log2_max_frame_num;
    cfg->pic_order_cnt_type = pic_order_cnt_type;
    cfg->log2_max_pic_order_cnt_lsb = log2_max_pic_order_cnt_lsb;
}

void composer_config_set_pps_params(ComposerConfig *cfg,
                                     int num_ref_idx_l0_default_minus1,
                                     int deblocking_filter_control_present_flag) {
    cfg->num_ref_idx_l0_default_minus1 = num_ref_idx_l0_default_minus1;
    cfg->deblocking_filter_control_present_flag = deblocking_filter_control_present_flag;
}

/*
 * Generate minimal SPS for Baseline profile
 */
size_t h264_generate_sps(uint8_t *rbsp, size_t capacity, int width, int height) {
    BitWriter bw;
    bitwriter_init(&bw, rbsp, capacity);

    int mb_width = width / 16;
    int mb_height = height / 16;

    /* profile_idc: Baseline = 66 */
    bitwriter_write_bits(&bw, 66, 8);

    /* constraint_set flags */
    bitwriter_write_bits(&bw, 0xc0, 8);

    /* level_idc: 40 = Level 4.0 */
    bitwriter_write_bits(&bw, 40, 8);

    /* seq_parameter_set_id: ue(0) */
    bitwriter_write_ue(&bw, 0);

    /* log2_max_frame_num_minus4: ue(0) -> log2=4 */
    bitwriter_write_ue(&bw, 0);

    /* pic_order_cnt_type: ue(2) */
    bitwriter_write_ue(&bw, 2);

    /* max_num_ref_frames: ue(v) - 2 base refs + waypoints */
    bitwriter_write_ue(&bw, 2 + MAX_WAYPOINTS);

    /* gaps_in_frame_num_value_allowed_flag: u(1) = 0 */
    bitwriter_write_bit(&bw, 0);

    /* pic_width_in_mbs_minus1 */
    bitwriter_write_ue(&bw, mb_width - 1);

    /* pic_height_in_map_units_minus1 */
    bitwriter_write_ue(&bw, mb_height - 1);

    /* frame_mbs_only_flag: u(1) = 1 */
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

    bitwriter_write_ue(&bw, 0);  /* pps_id */
    bitwriter_write_ue(&bw, 0);  /* sps_id */
    bitwriter_write_bit(&bw, 0); /* entropy_coding_mode_flag (CAVLC) */
    bitwriter_write_bit(&bw, 0); /* bottom_field_pic_order_in_frame_present_flag */
    bitwriter_write_ue(&bw, 0);  /* num_slice_groups_minus1 */
    bitwriter_write_ue(&bw, 1);  /* num_ref_idx_l0_default_active_minus1 (2 refs) */
    bitwriter_write_ue(&bw, 0);  /* num_ref_idx_l1_default_active_minus1 */
    bitwriter_write_bit(&bw, 0); /* weighted_pred_flag */
    bitwriter_write_bits(&bw, 0, 2); /* weighted_bipred_idc */
    bitwriter_write_se(&bw, 0);  /* pic_init_qp_minus26 */
    bitwriter_write_se(&bw, 0);  /* pic_init_qs_minus26 */
    bitwriter_write_se(&bw, 0);  /* chroma_qp_index_offset */
    bitwriter_write_bit(&bw, 1); /* deblocking_filter_control_present_flag */
    bitwriter_write_bit(&bw, 0); /* constrained_intra_pred_flag */
    bitwriter_write_bit(&bw, 0); /* redundant_pic_cnt_present_flag */

    bitwriter_write_trailing_bits(&bw);
    return bitwriter_get_size(&bw);
}

/* ============================================================================
 * Slice Header Parsing and Rewriting
 * ============================================================================ */

typedef struct {
    size_t mb_data_start_bit;
    int32_t slice_qp_delta;
    uint32_t disable_deblocking_filter_idc;
    int32_t slice_alpha_c0_offset_div2;
    int32_t slice_beta_offset_div2;
} ParsedSliceHeader;

/* Simple bit reader for parsing */
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos;
} SliceBitReader;

static void sbreader_init(SliceBitReader *br, const uint8_t *data, size_t size) {
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 0;
}

static int sbreader_read_bit(SliceBitReader *br) {
    if (br->byte_pos >= br->size) return 0;
    int bit = (br->data[br->byte_pos] >> (7 - br->bit_pos)) & 1;
    br->bit_pos++;
    if (br->bit_pos == 8) {
        br->bit_pos = 0;
        br->byte_pos++;
    }
    return bit;
}

static uint32_t sbreader_read_bits(SliceBitReader *br, int n) {
    uint32_t value = 0;
    for (int i = 0; i < n; i++) {
        value = (value << 1) | sbreader_read_bit(br);
    }
    return value;
}

static uint32_t sbreader_read_ue(SliceBitReader *br) {
    int leading_zeros = 0;
    while (sbreader_read_bit(br) == 0 && leading_zeros < 32) {
        leading_zeros++;
    }
    if (leading_zeros == 0) return 0;
    return (1 << leading_zeros) - 1 + sbreader_read_bits(br, leading_zeros);
}

static int32_t sbreader_read_se(SliceBitReader *br) {
    uint32_t ue = sbreader_read_ue(br);
    if (ue & 1) return (int32_t)((ue + 1) / 2);
    return -(int32_t)(ue / 2);
}

static size_t sbreader_get_bit_position(SliceBitReader *br) {
    return br->byte_pos * 8 + br->bit_pos;
}

static int parse_idr_slice_header(const uint8_t *rbsp, size_t rbsp_size,
                                   ComposerConfig *cfg, ParsedSliceHeader *hdr) {
    SliceBitReader br;
    sbreader_init(&br, rbsp, rbsp_size);
    memset(hdr, 0, sizeof(*hdr));

    sbreader_read_ue(&br);  /* first_mb_in_slice */
    sbreader_read_ue(&br);  /* slice_type */
    sbreader_read_ue(&br);  /* pps_id */
    sbreader_read_bits(&br, cfg->log2_max_frame_num);  /* frame_num */
    sbreader_read_ue(&br);  /* idr_pic_id */

    if (cfg->pic_order_cnt_type == 0) {
        sbreader_read_bits(&br, cfg->log2_max_pic_order_cnt_lsb);
    }

    /* dec_ref_pic_marking for IDR */
    sbreader_read_bit(&br);  /* no_output_of_prior_pics_flag */
    sbreader_read_bit(&br);  /* long_term_reference_flag */

    hdr->slice_qp_delta = sbreader_read_se(&br);

    if (cfg->deblocking_filter_control_present_flag) {
        hdr->disable_deblocking_filter_idc = sbreader_read_ue(&br);
        if (hdr->disable_deblocking_filter_idc != 1) {
            hdr->slice_alpha_c0_offset_div2 = sbreader_read_se(&br);
            hdr->slice_beta_offset_div2 = sbreader_read_se(&br);
        }
    }

    hdr->mb_data_start_bit = sbreader_get_bit_position(&br);
    return 1;
}

static void copy_bits(BitWriter *bw, const uint8_t *src, size_t src_size,
                       size_t start_bit, size_t num_bits) {
    SliceBitReader br;
    sbreader_init(&br, src, src_size);

    for (size_t i = 0; i < start_bit; i++) {
        sbreader_read_bit(&br);
    }

    for (size_t i = 0; i < num_bits; i++) {
        bitwriter_write_bit(bw, sbreader_read_bit(&br));
    }
}

size_t h264_rewrite_idr_frame(NALWriter *nw, ComposerConfig *write_cfg,
                               ComposerConfig *parse_cfg,
                               const uint8_t *rbsp, size_t rbsp_size) {
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

    /* Write our IDR slice header */
    bitwriter_write_ue(&bw, 0);  /* first_mb_in_slice */
    bitwriter_write_ue(&bw, SLICE_TYPE_I_ALL);
    bitwriter_write_ue(&bw, 0);  /* pps_id */
    bitwriter_write_bits(&bw, 0, write_cfg->log2_max_frame_num);
    bitwriter_write_ue(&bw, write_cfg->idr_pic_id);

    if (write_cfg->pic_order_cnt_type == 0) {
        bitwriter_write_bits(&bw, 0, write_cfg->log2_max_pic_order_cnt_lsb);
    }

    /* dec_ref_pic_marking: long_term_reference_flag = 1 */
    bitwriter_write_bit(&bw, 0);  /* no_output_of_prior_pics_flag */
    bitwriter_write_bit(&bw, 1);  /* long_term_reference_flag */

    /* Preserve encoder's slice_qp_delta */
    bitwriter_write_se(&bw, hdr.slice_qp_delta);

    if (write_cfg->deblocking_filter_control_present_flag) {
        bitwriter_write_ue(&bw, hdr.disable_deblocking_filter_idc);
        if (hdr.disable_deblocking_filter_idc != 1) {
            bitwriter_write_se(&bw, hdr.slice_alpha_c0_offset_div2);
            bitwriter_write_se(&bw, hdr.slice_beta_offset_div2);
        }
    }

    /* Copy MB data */
    copy_bits(&bw, rbsp, rbsp_size, hdr.mb_data_start_bit, mb_data_bits);

    size_t out_size = bitwriter_get_size(&bw);
    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_IDR,
                                    out_rbsp, out_size, 1);

    free(out_rbsp);
    write_cfg->frame_num = 1;
    return written;
}

size_t h264_rewrite_as_non_idr_i_frame(NALWriter *nw, ComposerConfig *write_cfg,
                                        ComposerConfig *parse_cfg,
                                        const uint8_t *rbsp, size_t rbsp_size,
                                        int frame_num) {
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

    /* Write non-IDR I-slice header */
    bitwriter_write_ue(&bw, 0);  /* first_mb_in_slice */
    bitwriter_write_ue(&bw, SLICE_TYPE_I_ALL);
    bitwriter_write_ue(&bw, 0);  /* pps_id */
    bitwriter_write_bits(&bw, frame_num, write_cfg->log2_max_frame_num);

    if (write_cfg->pic_order_cnt_type == 0) {
        bitwriter_write_bits(&bw, frame_num * 2, write_cfg->log2_max_pic_order_cnt_lsb);
    }

    /* dec_ref_pic_marking with MMCO commands */
    bitwriter_write_bit(&bw, 1);  /* adaptive_ref_pic_marking_mode_flag */
    bitwriter_write_ue(&bw, 4);   /* MMCO 4: max_long_term_frame_idx_plus1 */
    bitwriter_write_ue(&bw, 2);   /* = 2 (allows indices 0 and 1) */
    bitwriter_write_ue(&bw, 6);   /* MMCO 6: mark as long-term */
    bitwriter_write_ue(&bw, 1);   /* long_term_frame_idx = 1 */
    bitwriter_write_ue(&bw, 0);   /* MMCO 0: end */

    bitwriter_write_se(&bw, hdr.slice_qp_delta);

    if (write_cfg->deblocking_filter_control_present_flag) {
        bitwriter_write_ue(&bw, hdr.disable_deblocking_filter_idc);
        if (hdr.disable_deblocking_filter_idc != 1) {
            bitwriter_write_se(&bw, hdr.slice_alpha_c0_offset_div2);
            bitwriter_write_se(&bw, hdr.slice_beta_offset_div2);
        }
    }

    copy_bits(&bw, rbsp, rbsp_size, hdr.mb_data_start_bit, mb_data_bits);

    size_t out_size = bitwriter_get_size(&bw);
    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_SLICE,
                                    out_rbsp, out_size, 1);

    free(out_rbsp);
    write_cfg->frame_num = frame_num + 1;
    return written;
}

/* ============================================================================
 * P-Frame Generation
 * ============================================================================ */

typedef struct {
    int mv_x, mv_y;
    int ref_idx;
    int available;
} MVInfo;

static int median3(int a, int b, int c) {
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { b = c; }
    if (a > b) { a = b; }
    return b > a ? b : a;
}

static void get_mv_prediction(int mb_x, int mb_y, int mb_width,
                               const MVInfo *above_row, const MVInfo *left,
                               int cur_ref_idx,
                               int *pred_mvx, int *pred_mvy) {
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

    /* C: above-right, or D: above-left */
    if (mb_y > 0 && mb_x + 1 < mb_width && above_row[mb_x + 1].available) {
        c = above_row[mb_x + 1];
        c.available = 1;
        c_ref_match = (c.ref_idx == cur_ref_idx);
    } else if (mb_y > 0 && mb_x > 0 && above_row[mb_x - 1].available) {
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
        if (a_ref_match) { *pred_mvx = a.mv_x; *pred_mvy = a.mv_y; }
        else if (b_ref_match) { *pred_mvx = b.mv_x; *pred_mvy = b.mv_y; }
        else { *pred_mvx = c.mv_x; *pred_mvy = c.mv_y; }
    } else {
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

static void write_p16x16_mb(BitWriter *bw, int ref_idx, int mvd_x, int mvd_y, int num_refs) {
    /* mb_type: ue(0) = P_L0_16x16 */
    bitwriter_write_ue(bw, 0);

    /* ref_idx_l0 */
    if (num_refs == 1) {
        /* No ref_idx written */
    } else if (num_refs == 2) {
        bitwriter_write_bit(bw, 1 - (ref_idx & 1));
    } else {
        bitwriter_write_ue(bw, ref_idx);
    }

    /* mvd (quarter-pel) */
    bitwriter_write_se(bw, mvd_x);
    bitwriter_write_se(bw, mvd_y);

    /* coded_block_pattern: ue(0) = no residual */
    bitwriter_write_ue(bw, 0);
}

static void h264_write_p_slice_header(BitWriter *bw, ComposerConfig *cfg,
                                       int frame_num, int poc_lsb, int is_reference) {
    bitwriter_write_ue(bw, 0);  /* first_mb_in_slice */
    bitwriter_write_ue(bw, SLICE_TYPE_P);
    bitwriter_write_ue(bw, 0);  /* pps_id */

    int frame_num_bits = cfg->log2_max_frame_num;
    bitwriter_write_bits(bw, frame_num & ((1 << frame_num_bits) - 1), frame_num_bits);

    if (cfg->pic_order_cnt_type == 0) {
        int poc_bits = cfg->log2_max_pic_order_cnt_lsb;
        bitwriter_write_bits(bw, poc_lsb & ((1 << poc_bits) - 1), poc_bits);
    }

    /* num_ref_idx_active_override_flag = 1 */
    bitwriter_write_bit(bw, 1);
    bitwriter_write_ue(bw, 1);  /* 2 active refs */

    /* Explicit ref list modification */
    bitwriter_write_bit(bw, 1);
    bitwriter_write_ue(bw, 2); bitwriter_write_ue(bw, 0);  /* LTP 0 (A) */
    bitwriter_write_ue(bw, 2); bitwriter_write_ue(bw, 1);  /* LTP 1 (B) */
    bitwriter_write_ue(bw, 3);  /* End */

    if (is_reference) {
        bitwriter_write_bit(bw, 0);  /* Sliding window */
    }

    bitwriter_write_se(bw, 0);  /* slice_qp_delta */

    if (cfg->deblocking_filter_control_present_flag) {
        bitwriter_write_ue(bw, 1);  /* Disable deblocking */
    }
}

static void h264_write_p_slice_header_waypoint(BitWriter *bw, ComposerConfig *cfg,
                                                int frame_num, int poc_lsb,
                                                int is_reference, int long_term_idx) {
    bitwriter_write_ue(bw, 0);
    bitwriter_write_ue(bw, SLICE_TYPE_P);
    bitwriter_write_ue(bw, 0);

    int frame_num_bits = cfg->log2_max_frame_num;
    bitwriter_write_bits(bw, frame_num & ((1 << frame_num_bits) - 1), frame_num_bits);

    if (cfg->pic_order_cnt_type == 0) {
        int poc_bits = cfg->log2_max_pic_order_cnt_lsb;
        bitwriter_write_bits(bw, poc_lsb & ((1 << poc_bits) - 1), poc_bits);
    }

    bitwriter_write_bit(bw, 1);
    int num_refs = 2 + cfg->num_waypoints;
    bitwriter_write_ue(bw, num_refs - 1);

    /* Ref list modification */
    bitwriter_write_bit(bw, 1);
    bitwriter_write_ue(bw, 2); bitwriter_write_ue(bw, 0);  /* A */
    bitwriter_write_ue(bw, 2); bitwriter_write_ue(bw, 1);  /* B */
    for (int i = 0; i < cfg->num_waypoints; i++) {
        if (cfg->waypoints[i].valid) {
            bitwriter_write_ue(bw, 2);
            bitwriter_write_ue(bw, cfg->waypoints[i].long_term_idx);
        }
    }
    bitwriter_write_ue(bw, 3);

    if (is_reference) {
        if (long_term_idx >= 0) {
            bitwriter_write_bit(bw, 1);
            bitwriter_write_ue(bw, 4);
            bitwriter_write_ue(bw, long_term_idx + 1);
            bitwriter_write_ue(bw, 6);
            bitwriter_write_ue(bw, long_term_idx);
            bitwriter_write_ue(bw, 0);
        } else {
            bitwriter_write_bit(bw, 0);
        }
    }

    bitwriter_write_se(bw, 0);

    if (cfg->deblocking_filter_control_present_flag) {
        bitwriter_write_ue(bw, 1);
    }
}

size_t h264_write_scroll_p_frame(NALWriter *nw, ComposerConfig *cfg, int offset_px) {
    uint8_t *rbsp = malloc(1024 * 1024);
    BitWriter bw;
    bitwriter_init(&bw, rbsp, 1024 * 1024);

    int max_frame_num = 1 << cfg->log2_max_frame_num;
    int frame_num = cfg->frame_num % max_frame_num;

    if (cfg->num_waypoints > 0) {
        h264_write_p_slice_header_waypoint(&bw, cfg, frame_num, frame_num * 2, 0, -1);
    } else {
        h264_write_p_slice_header(&bw, cfg, frame_num, frame_num * 2, 0);
    }

    int a_region_end = (cfg->height - offset_px) / 16;

    /* Find waypoints for A and B regions */
    int wp_idx_a = -1, wp_offset_a = 0;
    if (offset_px > MV_LIMIT_PX && cfg->num_waypoints > 0) {
        for (int i = 0; i < cfg->num_waypoints; i++) {
            if (!cfg->waypoints[i].valid) continue;
            int wo = cfg->waypoints[i].offset_px;
            if (wo <= offset_px && wo > wp_offset_a) {
                int delta = offset_px - wo;
                if (delta <= MV_LIMIT_PX) {
                    wp_idx_a = i;
                    wp_offset_a = wo;
                }
            }
        }
    }

    int wp_idx_b = -1, wp_offset_b = 0;
    int b_direct_mv = offset_px - cfg->height;
    if (b_direct_mv < -MV_LIMIT_PX && cfg->num_waypoints > 0) {
        for (int i = 0; i < cfg->num_waypoints; i++) {
            if (!cfg->waypoints[i].valid) continue;
            int wo = cfg->waypoints[i].offset_px;
            if (wo > offset_px) {
                int delta = offset_px - wo;
                if (delta >= -MV_LIMIT_PX) {
                    wp_idx_b = i;
                    wp_offset_b = wo;
                    break;
                }
            }
        }
    }

    MVInfo *above_row = calloc(cfg->mb_width, sizeof(MVInfo));
    MVInfo *current_row = calloc(cfg->mb_width, sizeof(MVInfo));
    MVInfo left = {0};
    int skip_count = 0;

    for (int mb_y = 0; mb_y < cfg->mb_height; mb_y++) {
        left.available = 0;

        for (int mb_x = 0; mb_x < cfg->mb_width; mb_x++) {
            int ref_idx, mv_y, mv_x = 0;

            if (mb_y < a_region_end) {
                if (wp_idx_a >= 0) {
                    ref_idx = 2 + wp_idx_a;
                    mv_y = offset_px - wp_offset_a;
                } else {
                    ref_idx = 0;
                    mv_y = offset_px;
                }
            } else {
                if (wp_idx_b >= 0) {
                    ref_idx = 2 + wp_idx_b;
                    mv_y = offset_px - wp_offset_b;
                } else {
                    ref_idx = 1;
                    mv_y = offset_px - cfg->height;
                }
            }

            int mv_x_qpel = mv_x * 4;
            int mv_y_qpel = mv_y * 4;

            int pred_mvx, pred_mvy;
            get_mv_prediction(mb_x, mb_y, cfg->mb_width, above_row, &left,
                              ref_idx, &pred_mvx, &pred_mvy);

            int mvd_x = mv_x_qpel - pred_mvx;
            int mvd_y = mv_y_qpel - pred_mvy;

            /* P_Skip disabled for now */
            bitwriter_write_ue(&bw, skip_count);
            skip_count = 0;

            int num_refs = 2 + cfg->num_waypoints;
            write_p16x16_mb(&bw, ref_idx, mvd_x, mvd_y, num_refs);

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

    size_t written = nal_write_unit(nw, NAL_REF_IDC_NONE, NAL_TYPE_SLICE,
                                    rbsp, rbsp_size, 1);

    free(rbsp);
    cfg->frame_num++;
    return written;
}

int h264_needs_waypoint(ComposerConfig *cfg, int offset_px) {
    if (offset_px == 0) return 0;
    if (offset_px % MV_LIMIT_PX != 0) return 0;

    for (int i = 0; i < cfg->num_waypoints; i++) {
        if (cfg->waypoints[i].valid && cfg->waypoints[i].offset_px == offset_px) {
            return 0;
        }
    }
    return 1;
}

size_t h264_write_waypoint_p_frame(NALWriter *nw, ComposerConfig *cfg, int offset_px) {
    uint8_t *rbsp = malloc(1024 * 1024);
    BitWriter bw;
    bitwriter_init(&bw, rbsp, 1024 * 1024);

    int max_frame_num = 1 << cfg->log2_max_frame_num;
    int frame_num = cfg->frame_num % max_frame_num;
    int long_term_idx = 2 + cfg->num_waypoints;

    h264_write_p_slice_header_waypoint(&bw, cfg, frame_num, frame_num * 2, 1, long_term_idx);

    int a_region_end = (cfg->height - offset_px) / 16;

    /* Find best existing waypoint for A region */
    int wp_idx = -1, wp_offset = 0;
    if (offset_px > MV_LIMIT_PX) {
        for (int i = 0; i < cfg->num_waypoints; i++) {
            if (!cfg->waypoints[i].valid) continue;
            int wo = cfg->waypoints[i].offset_px;
            if (wo <= offset_px && wo > wp_offset) {
                int delta = offset_px - wo;
                if (delta <= MV_LIMIT_PX) {
                    wp_idx = i;
                    wp_offset = wo;
                }
            }
        }
    }

    MVInfo *above_row = calloc(cfg->mb_width, sizeof(MVInfo));
    MVInfo *current_row = calloc(cfg->mb_width, sizeof(MVInfo));
    MVInfo left = {0};
    int skip_count = 0;

    for (int mb_y = 0; mb_y < cfg->mb_height; mb_y++) {
        left.available = 0;

        for (int mb_x = 0; mb_x < cfg->mb_width; mb_x++) {
            int ref_idx, mv_y, mv_x = 0;

            if (mb_y < a_region_end) {
                if (wp_idx >= 0) {
                    ref_idx = 2 + wp_idx;
                    mv_y = offset_px - wp_offset;
                } else {
                    ref_idx = 0;
                    mv_y = offset_px;
                }
            } else {
                ref_idx = 1;
                mv_y = offset_px - cfg->height;
            }

            int mv_x_qpel = mv_x * 4;
            int mv_y_qpel = mv_y * 4;

            int pred_mvx, pred_mvy;
            get_mv_prediction(mb_x, mb_y, cfg->mb_width, above_row, &left,
                              ref_idx, &pred_mvx, &pred_mvy);

            int mvd_x = mv_x_qpel - pred_mvx;
            int mvd_y = mv_y_qpel - pred_mvy;

            bitwriter_write_ue(&bw, skip_count);
            skip_count = 0;
            int num_refs = 2 + cfg->num_waypoints;
            write_p16x16_mb(&bw, ref_idx, mvd_x, mvd_y, num_refs);

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

    size_t written = nal_write_unit(nw, NAL_REF_IDC_HIGH, NAL_TYPE_SLICE,
                                    rbsp, rbsp_size, 1);

    /* Register waypoint */
    if (cfg->num_waypoints < MAX_WAYPOINTS) {
        cfg->waypoints[cfg->num_waypoints].offset_px = offset_px;
        cfg->waypoints[cfg->num_waypoints].long_term_idx = long_term_idx;
        cfg->waypoints[cfg->num_waypoints].valid = 1;
        cfg->num_waypoints++;
    }

    free(rbsp);
    cfg->frame_num++;
    return written;
}
