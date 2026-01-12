#include "nal_parser.h"
#include <string.h>

void nal_parser_init(NALParser *parser, const uint8_t *data, size_t size) {
    parser->data = data;
    parser->size = size;
    parser->pos = 0;
}

/*
 * Find start code (00 00 01 or 00 00 00 01)
 * Returns position after start code, or size if not found
 */
static size_t find_start_code(const uint8_t *data, size_t size, size_t start) {
    for (size_t i = start; i + 2 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                return i + 3;
            }
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                return i + 4;
            }
        }
    }
    return size;
}

int nal_parser_next(NALParser *parser, NALUnit *unit) {
    /* Find start of this NAL */
    size_t nal_start = find_start_code(parser->data, parser->size, parser->pos);
    if (nal_start >= parser->size) {
        return 0;
    }

    /* Find start of next NAL (or end of data) */
    size_t nal_end = parser->size;
    for (size_t i = nal_start; i + 2 < parser->size; i++) {
        if (parser->data[i] == 0 && parser->data[i + 1] == 0 &&
            (parser->data[i + 2] == 1 ||
             (i + 3 < parser->size && parser->data[i + 2] == 0 && parser->data[i + 3] == 1))) {
            nal_end = i;
            break;
        }
    }

    /* Remove trailing zeros before next start code */
    while (nal_end > nal_start && parser->data[nal_end - 1] == 0) {
        nal_end--;
    }

    if (nal_end <= nal_start) {
        parser->pos = nal_end;
        return 0;
    }

    /* Parse NAL header */
    uint8_t header = parser->data[nal_start];
    unit->nal_ref_idc = (header >> 5) & 0x03;
    unit->nal_unit_type = header & 0x1F;
    unit->data = parser->data + nal_start + 1;
    unit->size = nal_end - nal_start - 1;

    parser->pos = nal_end;
    return 1;
}

size_t ebsp_to_rbsp(uint8_t *rbsp, const uint8_t *ebsp, size_t ebsp_size) {
    size_t rbsp_pos = 0;
    int zero_count = 0;

    for (size_t i = 0; i < ebsp_size; i++) {
        if (zero_count >= 2 && ebsp[i] == 0x03 && i + 1 < ebsp_size && ebsp[i + 1] <= 0x03) {
            /* Skip emulation prevention byte */
            zero_count = 0;
            continue;
        }

        rbsp[rbsp_pos++] = ebsp[i];

        if (ebsp[i] == 0x00) {
            zero_count++;
        } else {
            zero_count = 0;
        }
    }

    return rbsp_pos;
}

/*
 * Bit reader for parsing RBSP
 */
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos;
} BitReader;

static void bitreader_init(BitReader *br, const uint8_t *data, size_t size) {
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 0;
}

static int bitreader_read_bit(BitReader *br) {
    if (br->byte_pos >= br->size) return 0;

    int bit = (br->data[br->byte_pos] >> (7 - br->bit_pos)) & 1;
    br->bit_pos++;
    if (br->bit_pos == 8) {
        br->bit_pos = 0;
        br->byte_pos++;
    }
    return bit;
}

static uint32_t bitreader_read_bits(BitReader *br, int n) {
    uint32_t value = 0;
    for (int i = 0; i < n; i++) {
        value = (value << 1) | bitreader_read_bit(br);
    }
    return value;
}

static uint32_t bitreader_read_ue(BitReader *br) {
    int leading_zeros = 0;
    while (bitreader_read_bit(br) == 0 && leading_zeros < 32) {
        leading_zeros++;
    }
    if (leading_zeros == 0) return 0;
    uint32_t value = (1 << leading_zeros) - 1 + bitreader_read_bits(br, leading_zeros);
    return value;
}

int parse_sps(const uint8_t *rbsp, size_t size,
              int *width, int *height,
              int *log2_max_frame_num,
              int *pic_order_cnt_type,
              int *log2_max_pic_order_cnt_lsb) {
    BitReader br;
    bitreader_init(&br, rbsp, size);

    /* profile_idc */
    int profile_idc = bitreader_read_bits(&br, 8);

    /* constraint_set flags + reserved */
    bitreader_read_bits(&br, 8);

    /* level_idc */
    bitreader_read_bits(&br, 8);

    /* seq_parameter_set_id */
    bitreader_read_ue(&br);

    /* Handle high profiles (chroma_format_idc, etc.) */
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {
        int chroma_format_idc = bitreader_read_ue(&br);
        if (chroma_format_idc == 3) {
            bitreader_read_bit(&br);  /* separate_colour_plane_flag */
        }
        bitreader_read_ue(&br);  /* bit_depth_luma_minus8 */
        bitreader_read_ue(&br);  /* bit_depth_chroma_minus8 */
        bitreader_read_bit(&br);  /* qpprime_y_zero_transform_bypass_flag */
        int seq_scaling_matrix_present = bitreader_read_bit(&br);
        if (seq_scaling_matrix_present) {
            /* Skip scaling lists - complex, assume not present for Baseline */
            return -1;
        }
    }

    /* log2_max_frame_num_minus4 */
    *log2_max_frame_num = bitreader_read_ue(&br) + 4;

    /* pic_order_cnt_type */
    *pic_order_cnt_type = bitreader_read_ue(&br);

    *log2_max_pic_order_cnt_lsb = 0;
    if (*pic_order_cnt_type == 0) {
        *log2_max_pic_order_cnt_lsb = bitreader_read_ue(&br) + 4;
    } else if (*pic_order_cnt_type == 1) {
        bitreader_read_bit(&br);  /* delta_pic_order_always_zero_flag */
        /* Skip offset_for_non_ref_pic, offset_for_top_to_bottom_field */
        /* and num_ref_frames_in_pic_order_cnt_cycle entries */
        /* This is complex - return error for now */
        return -1;
    }
    /* pic_order_cnt_type == 2 has no additional syntax */

    /* max_num_ref_frames */
    bitreader_read_ue(&br);

    /* gaps_in_frame_num_value_allowed_flag */
    bitreader_read_bit(&br);

    /* pic_width_in_mbs_minus1 */
    int pic_width_in_mbs = bitreader_read_ue(&br) + 1;

    /* pic_height_in_map_units_minus1 */
    int pic_height_in_map_units = bitreader_read_ue(&br) + 1;

    /* frame_mbs_only_flag */
    int frame_mbs_only = bitreader_read_bit(&br);

    int mb_height = pic_height_in_map_units;
    if (!frame_mbs_only) {
        /* mb_adaptive_frame_field_flag */
        bitreader_read_bit(&br);
        mb_height *= 2;
    }

    *width = pic_width_in_mbs * 16;
    *height = mb_height * 16;

    /* Could parse frame_cropping for exact dimensions, but skip for now */

    return 0;
}

int parse_pps(const uint8_t *rbsp, size_t size,
              int *num_ref_idx_l0_default_minus1,
              int *deblocking_filter_control_present_flag) {
    BitReader br;
    bitreader_init(&br, rbsp, size);

    /* pic_parameter_set_id: ue */
    bitreader_read_ue(&br);

    /* seq_parameter_set_id: ue */
    bitreader_read_ue(&br);

    /* entropy_coding_mode_flag: u(1) */
    bitreader_read_bit(&br);

    /* bottom_field_pic_order_in_frame_present_flag: u(1) */
    bitreader_read_bit(&br);

    /* num_slice_groups_minus1: ue */
    int num_slice_groups = bitreader_read_ue(&br);
    if (num_slice_groups > 0) {
        /* Slice group map - complex, not supported */
        return -1;
    }

    /* num_ref_idx_l0_default_active_minus1: ue */
    *num_ref_idx_l0_default_minus1 = bitreader_read_ue(&br);

    /* num_ref_idx_l1_default_active_minus1: ue */
    bitreader_read_ue(&br);

    /* weighted_pred_flag: u(1) */
    bitreader_read_bit(&br);

    /* weighted_bipred_idc: u(2) */
    bitreader_read_bits(&br, 2);

    /* pic_init_qp_minus26: se */
    /* Need signed exp-golomb for this */
    uint32_t code = bitreader_read_ue(&br);
    (void)code;  /* Convert to signed but we don't need it */

    /* pic_init_qs_minus26: se */
    bitreader_read_ue(&br);

    /* chroma_qp_index_offset: se */
    bitreader_read_ue(&br);

    /* deblocking_filter_control_present_flag: u(1) */
    *deblocking_filter_control_present_flag = bitreader_read_bit(&br);

    return 0;
}
