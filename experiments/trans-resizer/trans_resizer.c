/*
 * trans_resizer - Simple horizontal padding for H.264 CAVLC streams
 *
 * Takes a 320x320 input and outputs 720x320 by adding padding MBs
 * at the end of each row.
 *
 * Input:  20x20 MBs (320x320)
 * Output: 45x20 MBs (720x320)
 *
 * For I-slices: padding uses I_PCM (all black)
 * For P-slices: padding uses skip MBs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bitwriter.h"

#define INPUT_WIDTH   320
#define INPUT_HEIGHT  320
#define OUTPUT_WIDTH  720
#define OUTPUT_HEIGHT 320

#define INPUT_MB_WIDTH   (INPUT_WIDTH / 16)   /* 20 MBs */
#define INPUT_MB_HEIGHT  (INPUT_HEIGHT / 16)  /* 20 MBs */
#define OUTPUT_MB_WIDTH  (OUTPUT_WIDTH / 16)  /* 45 MBs */
#define OUTPUT_MB_HEIGHT (OUTPUT_HEIGHT / 16) /* 20 MBs */

#define PADDING_MBS_PER_ROW (OUTPUT_MB_WIDTH - INPUT_MB_WIDTH) /* 25 MBs */

/* PPS parameters extracted from input stream */
static int pps_num_ref_idx_l0_default = 1;  /* num_ref_idx_l0_active_minus1 + 1 */
static int pps_num_ref_idx_l0_active_minus1 = 0;  /* Raw value for output PPS */
static int pps_pic_init_qp_minus26 = 0;  /* pic_init_qp_minus26 from input PPS */
static int pps_chroma_qp_index_offset = 0;  /* chroma_qp_index_offset from input PPS */

/* Use BitReader from bitwriter.h with wrapper macros for convenience */
#define br_init(br, data, size) bitreader_init(br, data, size)
#define br_read_bit(br) bitreader_read_bit(br)
#define br_read_bits(br, n) bitreader_read_bits(br, n)
#define br_read_ue(br) bitreader_read_ue(br)
#define br_read_se(br) bitreader_read_se(br)

/* Align bitwriter to byte boundary by padding with zeros */
static void bitwriter_align(BitWriter *bw) {
    while (bw->bit_pos != 0) {
        bitwriter_write_bit(bw, 0);
    }
}

/*
 * Convert EBSP to RBSP (remove emulation prevention bytes)
 */
static size_t ebsp_to_rbsp(uint8_t *rbsp, const uint8_t *ebsp, size_t ebsp_size) {
    size_t rbsp_pos = 0;
    int zero_count = 0;
    for (size_t i = 0; i < ebsp_size; i++) {
        if (zero_count >= 2 && ebsp[i] == 0x03 && i + 1 < ebsp_size && ebsp[i + 1] <= 0x03) {
            zero_count = 0;
            continue;
        }
        rbsp[rbsp_pos++] = ebsp[i];
        if (ebsp[i] == 0x00) zero_count++;
        else zero_count = 0;
    }
    return rbsp_pos;
}

/*
 * Write NAL unit with start code and emulation prevention
 */
static size_t write_nal_unit(uint8_t *output, const uint8_t *rbsp, size_t rbsp_size,
                             int nal_ref_idc, int nal_unit_type) {
    size_t pos = 0;

    /* Start code */
    output[pos++] = 0x00;
    output[pos++] = 0x00;
    output[pos++] = 0x00;
    output[pos++] = 0x01;

    /* NAL header */
    output[pos++] = (nal_ref_idc << 5) | nal_unit_type;

    /* RBSP with emulation prevention */
    int zero_count = 0;
    for (size_t i = 0; i < rbsp_size; i++) {
        if (zero_count >= 2 && rbsp[i] <= 0x03) {
            output[pos++] = 0x03;
            zero_count = 0;
        }
        output[pos++] = rbsp[i];
        if (rbsp[i] == 0x00) zero_count++;
        else zero_count = 0;
    }

    return pos;
}

/*
 * Generate SPS for output dimensions
 */
static size_t generate_output_sps(uint8_t *output, int log2_max_frame_num, int max_num_ref_frames) {
    BitWriter bw;
    uint8_t sps_rbsp[64];
    bitwriter_init(&bw, sps_rbsp, sizeof(sps_rbsp));

    /* profile_idc = 66 (Baseline) */
    bitwriter_write_bits(&bw, 66, 8);
    /* constraint_set flags */
    bitwriter_write_bits(&bw, 0xC0, 8);  /* constraint_set0,1 = 1 */
    /* level_idc = 30 */
    bitwriter_write_bits(&bw, 30, 8);
    /* seq_parameter_set_id = 0 */
    bitwriter_write_ue(&bw, 0);
    /* log2_max_frame_num_minus4 */
    bitwriter_write_ue(&bw, log2_max_frame_num - 4);
    /* pic_order_cnt_type = 2 (no POC in slice header) */
    bitwriter_write_ue(&bw, 2);
    /* max_num_ref_frames - from input stream */
    bitwriter_write_ue(&bw, max_num_ref_frames);
    /* gaps_in_frame_num_value_allowed_flag = 0 */
    bitwriter_write_bit(&bw, 0);
    /* pic_width_in_mbs_minus1 */
    bitwriter_write_ue(&bw, OUTPUT_MB_WIDTH - 1);
    /* pic_height_in_map_units_minus1 */
    bitwriter_write_ue(&bw, OUTPUT_MB_HEIGHT - 1);
    /* frame_mbs_only_flag = 1 */
    bitwriter_write_bit(&bw, 1);
    /* direct_8x8_inference_flag = 1 */
    bitwriter_write_bit(&bw, 1);
    /* frame_cropping_flag = 0 */
    bitwriter_write_bit(&bw, 0);
    /* vui_parameters_present_flag = 0 */
    bitwriter_write_bit(&bw, 0);

    /* RBSP trailing bits */
    bitwriter_write_bit(&bw, 1);
    bitwriter_align(&bw);

    size_t sps_size = bitwriter_get_size(&bw);
    return write_nal_unit(output, sps_rbsp, sps_size, 3, 7);
}

/*
 * Generate PPS
 */
static size_t generate_output_pps(uint8_t *output, int num_ref_idx_l0_active_minus1, int pic_init_qp_minus26, int chroma_qp_index_offset) {
    BitWriter bw;
    uint8_t pps_rbsp[32];
    bitwriter_init(&bw, pps_rbsp, sizeof(pps_rbsp));

    /* pic_parameter_set_id = 0 */
    bitwriter_write_ue(&bw, 0);
    /* seq_parameter_set_id = 0 */
    bitwriter_write_ue(&bw, 0);
    /* entropy_coding_mode_flag = 0 (CAVLC) */
    bitwriter_write_bit(&bw, 0);
    /* bottom_field_pic_order_in_frame_present_flag = 0 */
    bitwriter_write_bit(&bw, 0);
    /* num_slice_groups_minus1 = 0 */
    bitwriter_write_ue(&bw, 0);
    /* num_ref_idx_l0_default_active_minus1 - from input PPS */
    bitwriter_write_ue(&bw, num_ref_idx_l0_active_minus1);
    /* num_ref_idx_l1_default_active_minus1 = 0 */
    bitwriter_write_ue(&bw, 0);
    /* weighted_pred_flag = 0 */
    bitwriter_write_bit(&bw, 0);
    /* weighted_bipred_idc = 0 */
    bitwriter_write_bits(&bw, 0, 2);
    /* pic_init_qp_minus26 - from input PPS */
    bitwriter_write_se(&bw, pic_init_qp_minus26);
    /* pic_init_qs_minus26 = 0 */
    bitwriter_write_se(&bw, 0);
    /* chroma_qp_index_offset - from input PPS */
    bitwriter_write_se(&bw, chroma_qp_index_offset);
    /* deblocking_filter_control_present_flag = 1 */
    bitwriter_write_bit(&bw, 1);
    /* constrained_intra_pred_flag = 0 */
    bitwriter_write_bit(&bw, 0);
    /* redundant_pic_cnt_present_flag = 0 */
    bitwriter_write_bit(&bw, 0);

    /* RBSP trailing bits */
    bitwriter_write_bit(&bw, 1);
    bitwriter_align(&bw);

    size_t pps_size = bitwriter_get_size(&bw);
    return write_nal_unit(output, pps_rbsp, pps_size, 3, 8);
}

/*
 * Copy bits from reader to writer
 */
static void copy_bits(BitReader *br, BitWriter *bw, int n) {
    for (int i = 0; i < n; i++) {
        bitwriter_write_bit(bw, br_read_bit(br));
    }
}

/*
 * Write I_PCM macroblock (black padding for I-slice)
 * I_PCM: mb_type = 25, followed by 384 bytes of sample data
 *
 * edge_y: Y value to use for the bottom row (row 15) and leftmost columns
 *         to provide compatible top-right samples for the next row's
 *         rightmost original MBs' intra prediction.
 * is_first_padding: if true, this is the first I_PCM after original content,
 *                   so set edge samples to match original content's right edge.
 */
static void write_ipcm_mb_edge(BitWriter *bw, uint8_t edge_y, int is_first_padding) {
    /* mb_type = 25 (I_PCM) */
    bitwriter_write_ue(bw, 25);

    /* Align to byte boundary */
    bitwriter_align(bw);

    /* Write 256 Y samples in raster scan order (row by row, left to right)
     *
     * For the first padding MB (column 20), we need edge-compatible samples:
     *
     * 1. The BOTTOM ROW (row 15) serves as "top-right" reference for the
     *    NEXT row's rightmost original MB's intra 4x4 prediction.
     *
     * 2. The LEFTMOST COLUMNS (cols 0-3) provide neighbor samples for:
     *    - Sub-pixel motion compensation (6-tap interpolation filter)
     *    - These samples are needed when P-frame MBs use fractional MVs
     *
     * In the original frame, the encoder used edge-extension (repeating the
     * last column) for interpolation. In the resized frame, I_PCM samples
     * are used instead. Setting edge-compatible values prevents corruption.
     */
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            uint8_t y_val;
            if (is_first_padding) {
                if (row == 15 || col == 0) {
                    /* Bottom row or leftmost column: use edge value */
                    y_val = edge_y;
                } else if (col < 4) {
                    /* Gradient cols 1-3 for smooth transition */
                    int blend = (16 * col + edge_y * (4 - col)) / 4;
                    y_val = (uint8_t)blend;
                } else {
                    y_val = 16;  /* Black for TV range */
                }
            } else {
                y_val = 16;
            }
            bitwriter_write_bits(bw, y_val, 8);
        }
    }

    /* Write 64 Cb samples (neutral = 128) */
    for (int i = 0; i < 64; i++) {
        bitwriter_write_bits(bw, 128, 8);
    }

    /* Write 64 Cr samples (neutral = 128) */
    for (int i = 0; i < 64; i++) {
        bitwriter_write_bits(bw, 128, 8);
    }
}

/* Backward-compatible wrapper */
static void write_ipcm_mb(BitWriter *bw) {
    write_ipcm_mb_edge(bw, 16, 0);
}

/*
 * CBP mapping table for Intra (codeNum -> CBP)
 */
static const uint8_t cbp_intra_table[48] = {
    47, 31, 15,  0, 23, 27, 29, 30, 7, 11, 13, 14, 39, 43, 45, 46,
    16,  3,  5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44, 1, 2, 4,
     8, 17, 18, 20, 24, 6,  9, 22, 25, 32, 33, 34, 36, 40, 38, 41
};

/*
 * CBP mapping table for Inter (codeNum -> CBP)
 */
static const uint8_t cbp_inter_table[48] = {
     0, 16,  1,  2,  4,  8, 32,  3,  5, 10, 12, 15, 47,  7, 11, 13,
    14,  6,  9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41
};

/*
 * CAVLC coeff_token tables (partial - just tc=0 entries for skipping empty blocks)
 * Format: (bits, code, total_coeff, trailing_ones)
 */

/* nC = 0 or 1: tc=0 is "1" (1 bit) */
/* nC = 2 or 3: tc=0 is "11" (2 bits) */
/* nC = 4-7:    tc=0 is "1111" (4 bits) */
/* nC >= 8:     tc=0 is "000011" (6 bits) */

typedef struct {
    int bits;
    uint32_t code;
    int tc;
    int t1;
} CoeffToken;

static const CoeffToken ct_table_0_1[] = {
    {1, 0b1, 0, 0},
    {6, 0b000101, 1, 0}, {2, 0b01, 1, 1},
    {8, 0b00000111, 2, 0}, {6, 0b000100, 2, 1}, {3, 0b001, 2, 2},
    {9, 0b000000111, 3, 0}, {8, 0b00000110, 3, 1}, {7, 0b0000101, 3, 2}, {5, 0b00011, 3, 3},
    {10, 0b0000000111, 4, 0}, {9, 0b000000110, 4, 1}, {8, 0b00000101, 4, 2}, {6, 0b000011, 4, 3},
    {11, 0b00000000111, 5, 0}, {10, 0b0000000110, 5, 1}, {9, 0b000000101, 5, 2}, {7, 0b0000100, 5, 3},
    {13, 0b0000000001111, 6, 0}, {11, 0b00000000110, 6, 1}, {10, 0b0000000101, 6, 2}, {8, 0b00000100, 6, 3},
    {13, 0b0000000001011, 7, 0}, {13, 0b0000000001110, 7, 1}, {11, 0b00000000101, 7, 2}, {9, 0b000000100, 7, 3},
    {13, 0b0000000001000, 8, 0}, {13, 0b0000000001010, 8, 1}, {13, 0b0000000001101, 8, 2}, {10, 0b0000000100, 8, 3},
    {14, 0b00000000001111, 9, 0}, {14, 0b00000000001110, 9, 1}, {13, 0b0000000001001, 9, 2}, {11, 0b00000000100, 9, 3},
    {14, 0b00000000001011, 10, 0}, {14, 0b00000000001010, 10, 1}, {14, 0b00000000001101, 10, 2}, {13, 0b0000000001100, 10, 3},
    {15, 0b000000000001111, 11, 0}, {15, 0b000000000001110, 11, 1}, {14, 0b00000000001001, 11, 2}, {14, 0b00000000001100, 11, 3},
    {15, 0b000000000001011, 12, 0}, {15, 0b000000000001010, 12, 1}, {15, 0b000000000001101, 12, 2}, {14, 0b00000000001000, 12, 3},
    {16, 0b0000000000001111, 13, 0}, {15, 0b000000000000001, 13, 1}, {15, 0b000000000001001, 13, 2}, {15, 0b000000000001100, 13, 3},
    {16, 0b0000000000001011, 14, 0}, {16, 0b0000000000001110, 14, 1}, {16, 0b0000000000001101, 14, 2}, {15, 0b000000000001000, 14, 3},
    {16, 0b0000000000000111, 15, 0}, {16, 0b0000000000001010, 15, 1}, {16, 0b0000000000001001, 15, 2}, {16, 0b0000000000001100, 15, 3},
    {16, 0b0000000000000100, 16, 0}, {16, 0b0000000000000110, 16, 1}, {16, 0b0000000000000101, 16, 2}, {16, 0b0000000000001000, 16, 3},
    {0, 0, -1, -1}
};

static const CoeffToken ct_table_2_3[] = {
    {2, 0b11, 0, 0},
    {6, 0b001011, 1, 0}, {2, 0b10, 1, 1},
    {6, 0b000111, 2, 0}, {5, 0b00111, 2, 1}, {3, 0b011, 2, 2},
    {7, 0b0000111, 3, 0}, {6, 0b001010, 3, 1}, {6, 0b001001, 3, 2}, {4, 0b0101, 3, 3},
    {8, 0b00000111, 4, 0}, {6, 0b000110, 4, 1}, {6, 0b000101, 4, 2}, {4, 0b0100, 4, 3},
    {8, 0b00000100, 5, 0}, {7, 0b0000110, 5, 1}, {7, 0b0000101, 5, 2}, {5, 0b00110, 5, 3},
    {9, 0b000000111, 6, 0}, {8, 0b00000110, 6, 1}, {8, 0b00000101, 6, 2}, {6, 0b001000, 6, 3},
    {11, 0b00000001111, 7, 0}, {9, 0b000000110, 7, 1}, {9, 0b000000101, 7, 2}, {6, 0b000100, 7, 3},
    {11, 0b00000001011, 8, 0}, {11, 0b00000001110, 8, 1}, {11, 0b00000001101, 8, 2}, {7, 0b0000100, 8, 3},
    {12, 0b000000001111, 9, 0}, {11, 0b00000001010, 9, 1}, {11, 0b00000001001, 9, 2}, {9, 0b000000100, 9, 3},
    {12, 0b000000001011, 10, 0}, {12, 0b000000001110, 10, 1}, {12, 0b000000001101, 10, 2}, {11, 0b00000001100, 10, 3},
    {12, 0b000000001000, 11, 0}, {12, 0b000000001010, 11, 1}, {12, 0b000000001001, 11, 2}, {11, 0b00000001000, 11, 3},
    {13, 0b0000000001111, 12, 0}, {13, 0b0000000001110, 12, 1}, {13, 0b0000000001101, 12, 2}, {12, 0b000000001100, 12, 3},
    {13, 0b0000000001011, 13, 0}, {13, 0b0000000001010, 13, 1}, {13, 0b0000000001001, 13, 2}, {13, 0b0000000001100, 13, 3},
    {13, 0b0000000000111, 14, 0}, {14, 0b00000000001011, 14, 1}, {13, 0b0000000000110, 14, 2}, {13, 0b0000000001000, 14, 3},
    {14, 0b00000000001001, 15, 0}, {14, 0b00000000001000, 15, 1}, {14, 0b00000000001010, 15, 2}, {13, 0b0000000000001, 15, 3},
    {14, 0b00000000000111, 16, 0}, {14, 0b00000000000110, 16, 1}, {14, 0b00000000000101, 16, 2}, {14, 0b00000000000100, 16, 3},
    {0, 0, -1, -1}
};

static const CoeffToken ct_table_4_7[] = {
    {4, 0b1111, 0, 0},
    {6, 0b001111, 1, 0}, {4, 0b1110, 1, 1},
    {6, 0b001011, 2, 0}, {5, 0b01111, 2, 1}, {4, 0b1101, 2, 2},
    {6, 0b001000, 3, 0}, {5, 0b01100, 3, 1}, {5, 0b01110, 3, 2}, {4, 0b1100, 3, 3},
    {7, 0b0001111, 4, 0}, {5, 0b01010, 4, 1}, {5, 0b01011, 4, 2}, {4, 0b1011, 4, 3},
    {7, 0b0001011, 5, 0}, {5, 0b01000, 5, 1}, {5, 0b01001, 5, 2}, {4, 0b1010, 5, 3},
    {7, 0b0001001, 6, 0}, {6, 0b001110, 6, 1}, {6, 0b001101, 6, 2}, {4, 0b1001, 6, 3},
    {7, 0b0001000, 7, 0}, {6, 0b001010, 7, 1}, {6, 0b001001, 7, 2}, {4, 0b1000, 7, 3},
    {8, 0b00001111, 8, 0}, {7, 0b0001110, 8, 1}, {7, 0b0001101, 8, 2}, {5, 0b01101, 8, 3},
    {8, 0b00001011, 9, 0}, {8, 0b00001110, 9, 1}, {7, 0b0001010, 9, 2}, {6, 0b001100, 9, 3},
    {9, 0b000001111, 10, 0}, {8, 0b00001010, 10, 1}, {8, 0b00001101, 10, 2}, {7, 0b0001100, 10, 3},
    {9, 0b000001011, 11, 0}, {9, 0b000001110, 11, 1}, {8, 0b00001001, 11, 2}, {8, 0b00001100, 11, 3},
    {9, 0b000001000, 12, 0}, {9, 0b000001010, 12, 1}, {9, 0b000001101, 12, 2}, {8, 0b00001000, 12, 3},
    {10, 0b0000001101, 13, 0}, {9, 0b000000111, 13, 1}, {9, 0b000001001, 13, 2}, {9, 0b000001100, 13, 3},
    {10, 0b0000001001, 14, 0}, {10, 0b0000001100, 14, 1}, {10, 0b0000001011, 14, 2}, {10, 0b0000001010, 14, 3},
    {10, 0b0000000101, 15, 0}, {10, 0b0000001000, 15, 1}, {10, 0b0000000111, 15, 2}, {10, 0b0000000110, 15, 3},
    {10, 0b0000000001, 16, 0}, {10, 0b0000000100, 16, 1}, {10, 0b0000000011, 16, 2}, {10, 0b0000000010, 16, 3},
    {0, 0, -1, -1}
};

static const CoeffToken ct_table_chroma_dc[] = {
    {2, 0b01, 0, 0},
    {6, 0b000111, 1, 0}, {1, 0b1, 1, 1},
    {6, 0b000100, 2, 0}, {6, 0b000110, 2, 1}, {3, 0b001, 2, 2},
    {6, 0b000011, 3, 0}, {7, 0b0000011, 3, 1}, {7, 0b0000010, 3, 2}, {6, 0b000101, 3, 3},
    {6, 0b000010, 4, 0}, {8, 0b00000011, 4, 1}, {8, 0b00000010, 4, 2}, {7, 0b0000000, 4, 3},
    {0, 0, -1, -1}
};

static uint32_t peek_bits(BitReader *br, int n) {
    size_t save_byte = br->byte_pos;
    int save_bit = br->bit_pos;
    uint32_t val = br_read_bits(br, n);
    br->byte_pos = save_byte;
    br->bit_pos = save_bit;
    return val;
}

/*
 * Total zeros VLC tables (from ffmpeg h264_cavlc.c)
 * Row index = total_coeff - 1
 * Column index = total_zeros value
 */
static const uint8_t total_zeros_len[16][16] = {
    {1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9},
    {3,3,3,3,3,4,4,4,4,5,5,6,6,6,6},
    {4,3,3,3,4,4,3,3,4,5,5,6,5,6},
    {5,3,4,4,3,3,3,4,3,4,5,5,5},
    {4,4,4,3,3,3,3,3,4,5,4,5},
    {6,5,3,3,3,3,3,3,4,3,6},
    {6,5,3,3,3,2,3,4,3,6},
    {6,4,5,3,2,2,3,3,6},
    {6,6,4,2,2,3,2,5},
    {5,5,3,2,2,2,4},
    {4,4,3,3,1,3},
    {4,4,2,1,3},
    {3,3,1,2},
    {2,2,1},
    {1,1},
};

static const uint8_t total_zeros_bits[16][16] = {
    {1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1},
    {7,6,5,4,3,5,4,3,2,3,2,3,2,1,0},
    {5,7,6,5,4,3,4,3,2,3,2,1,1,0},
    {3,7,5,4,6,5,4,3,3,2,2,1,0},
    {5,4,3,7,6,5,4,3,2,1,1,0},
    {1,1,7,6,5,4,3,2,1,1,0},
    {1,1,5,4,3,3,2,1,1,0},
    {1,1,1,3,3,2,2,1,0},
    {1,0,1,3,2,1,1,1},
    {1,0,1,3,2,1,1},
    {0,1,1,2,1,3},
    {0,1,1,1,1},
    {0,1,1,1},
    {0,1,1},
    {0,1},
};

/* Chroma DC total zeros (for 4 max coeffs) */
static const uint8_t chroma_dc_total_zeros_len[3][4] = {
    { 1, 2, 3, 3, },
    { 1, 2, 2, 0, },
    { 1, 1, 0, 0, },
};

static const uint8_t chroma_dc_total_zeros_bits[3][4] = {
    { 1, 1, 1, 0, },
    { 1, 1, 0, 0, },
    { 1, 0, 0, 0, },
};

/* Run before tables */
static const uint8_t run_len[7][16] = {
    {1,1},
    {1,2,2},
    {2,2,2,2},
    {2,2,2,3,3},
    {2,2,3,3,3,3},
    {2,3,3,3,3,3,3},
    {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11},
};

static const uint8_t run_bits[7][16] = {
    {1,0},
    {1,1,0},
    {3,2,1,0},
    {3,2,1,1,0},
    {3,2,3,2,1,0},
    {3,0,1,3,2,5,4},
    {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1},
};

/*
 * Decode total_zeros VLC
 * Returns total_zeros value, or -1 on error
 */
static int decode_total_zeros(BitReader *br, int total_coeff, int max_coeff) {
    if (total_coeff >= max_coeff) return 0;

    const uint8_t *len_table;
    const uint8_t *bits_table;
    int max_zeros;

    if (max_coeff == 4) {
        /* Chroma DC */
        if (total_coeff > 3) return 0;
        len_table = chroma_dc_total_zeros_len[total_coeff - 1];
        bits_table = chroma_dc_total_zeros_bits[total_coeff - 1];
        max_zeros = 4 - total_coeff;
    } else {
        /* Luma (16 coeffs) or AC (15 coeffs) */
        if (total_coeff > 15) return 0;
        len_table = total_zeros_len[total_coeff - 1];
        bits_table = total_zeros_bits[total_coeff - 1];
        max_zeros = max_coeff - total_coeff;
    }

    /* Try to match each entry */
    for (int tz = 0; tz <= max_zeros; tz++) {
        int len = len_table[tz];
        if (len == 0) continue;
        uint32_t code = bits_table[tz];
        uint32_t peek = peek_bits(br, len);
        if (peek == code) {
            br_read_bits(br, len);
            return tz;
        }
    }

    fprintf(stderr, "Failed to decode total_zeros at %zu.%d (tc=%d, max=%d)\n",
            br->byte_pos, br->bit_pos, total_coeff, max_coeff);
    fprintf(stderr, "  Next 16 bits: ");
    for (int i = 0; i < 16; i++) {
        fprintf(stderr, "%d", br_read_bit(br));
    }
    fprintf(stderr, "\n");
    return -1;
}

/*
 * Decode run_before VLC
 * Returns run value, or -1 on error
 */
static int decode_run_before(BitReader *br, int zeros_left) {
    if (zeros_left <= 0) return 0;

    int table_idx = (zeros_left > 6) ? 6 : zeros_left - 1;
    const uint8_t *len_table = run_len[table_idx];
    const uint8_t *bits_table = run_bits[table_idx];
    /* max_run is min(zeros_left, table_max). Table 6 has entries 0-14. */
    int max_run = zeros_left;
    if (zeros_left > 6 && max_run > 14) max_run = 14;

    for (int run = 0; run <= max_run; run++) {
        int len = len_table[run];
        if (len == 0) continue;
        uint32_t code = bits_table[run];
        uint32_t peek = peek_bits(br, len);
        if (peek == code) {
            br_read_bits(br, len);
            return run;
        }
    }

    fprintf(stderr, "Failed to decode run_before at %zu.%d (zeros_left=%d)\n",
            br->byte_pos, br->bit_pos, zeros_left);
    fprintf(stderr, "  Next 12 bits: ");
    for (int i = 0; i < 12; i++) {
        fprintf(stderr, "%d", (br->buffer[br->byte_pos + (br->bit_pos + i) / 8] >> (7 - ((br->bit_pos + i) % 8))) & 1);
    }
    fprintf(stderr, " (bytes: %02x %02x)\n", br->buffer[br->byte_pos], br->buffer[br->byte_pos + 1]);
    return -1;
}

/*
 * Read coeff_token from bitstream
 * Returns number of bits consumed, sets *tc and *t1
 */
static int read_coeff_token(BitReader *br, int nC, int *tc, int *t1) {
    const CoeffToken *table;

    if (nC == -1) {
        table = ct_table_chroma_dc;
    } else if (nC <= 1) {
        table = ct_table_0_1;
    } else if (nC <= 3) {
        table = ct_table_2_3;
    } else if (nC <= 7) {
        table = ct_table_4_7;
    } else {
        /* nC >= 8: fixed 6-bit code */
        uint32_t code = br_read_bits(br, 6);
        if (code == 3) {
            *tc = 0;
            *t1 = 0;
        } else {
            *t1 = code & 3;
            *tc = (code >> 2) + 1;
        }
        return 6;
    }

    /* Try to match table entries (longest first for correctness) */
    for (int i = 0; table[i].tc >= 0; i++) {
        int bits = table[i].bits;
        uint32_t code = table[i].code;
        uint32_t peek = peek_bits(br, bits);
        if (peek == code) {
            br_read_bits(br, bits);
            *tc = table[i].tc;
            *t1 = table[i].t1;
            return bits;
        }
    }

    /* Debug: show what bits we see */
    fprintf(stderr, "Failed to match coeff_token at %zu.%d, nC=%d\n",
            br->byte_pos, br->bit_pos, nC);
    size_t save_byte = br->byte_pos;
    int save_bit = br->bit_pos;
    fprintf(stderr, "  Next 16 bits: ");
    for (int i = 0; i < 16; i++) {
        fprintf(stderr, "%d", br_read_bit(br));
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "  Next bytes: ");
    br->byte_pos = save_byte;
    br->bit_pos = save_bit;
    for (int i = 0; i < 4; i++) {
        fprintf(stderr, "%02x ", br->buffer[save_byte + i]);
    }
    fprintf(stderr, " (bit_pos=%d)\n", save_bit);
    br->byte_pos = save_byte;
    br->bit_pos = save_bit;
    return -1;
}

/*
 * Copy a CAVLC block from reader to writer
 * Returns total_coeff on success, -1 on error
 */
static int copy_cavlc_block(BitReader *br, BitWriter *bw, int nC, int max_coeff) {
    int tc, t1;

    /* Read and copy coeff_token */
    size_t start_byte = br->byte_pos;
    int start_bit = br->bit_pos;

    int ct_bits = read_coeff_token(br, nC, &tc, &t1);
    if (ct_bits < 0) return -1;

    /* Debug for blocks near the failure point (around byte 101-105) */
    int debug_block = (start_byte >= 96 && start_byte <= 110);
    if (debug_block) {
        fprintf(stderr, "          coeff_token: tc=%d t1=%d bits=%d at %zu.%d, bytes: %02x %02x %02x\n",
               tc, t1, ct_bits, start_byte, start_bit,
               br->buffer[start_byte], br->buffer[start_byte+1], br->buffer[start_byte+2]);
    }
    (void)start_byte; (void)start_bit; (void)ct_bits;

    /* Rewind and copy the bits */
    br->byte_pos = start_byte;
    br->bit_pos = start_bit;
    copy_bits(br, bw, ct_bits);

    if (tc == 0) return 0;

    /* Copy trailing_one signs */
    copy_bits(br, bw, t1);

    /* Copy levels using OpenH264's algorithm (CavlcGetLevelVal) */
    int suffix_length = (tc > 10 && t1 < 3) ? 1 : 0;
    if (debug_block) {
        fprintf(stderr, "          %d levels to parse, suffix_length=%d, starting at %zu.%d\n",
                tc - t1, suffix_length, br->byte_pos, br->bit_pos);
    }
    for (int i = 0; i < tc - t1; i++) {
        size_t lvl_start_byte = br->byte_pos;
        int lvl_start_bit = br->bit_pos;

        /* level_prefix: count leading zeros, then read stop bit (1) */
        int level_prefix = 0;
        while (br_read_bit(br) == 0 && level_prefix < 16) {
            bitwriter_write_bit(bw, 0);
            level_prefix++;
        }
        bitwriter_write_bit(bw, 1);  /* Stop bit */

        if (level_prefix > 15) {
            fprintf(stderr, "Invalid level_prefix %d\n", level_prefix);
            return -1;
        }

        /* Base levelCode = level_prefix << suffix_length */
        int levelCode = level_prefix << suffix_length;

        /* Determine suffix_size based on prefix and suffix_length */
        int suffix_size = suffix_length;
        if (level_prefix >= 14) {
            if (level_prefix == 14 && suffix_length == 0) {
                suffix_size = 4;  /* Escape: 4-bit suffix */
            } else if (level_prefix == 15) {
                suffix_size = 12;  /* Escape: 12-bit suffix */
                if (suffix_length == 0) {
                    levelCode += 15;  /* Special adjustment for escape code */
                }
            }
        }

        /* Read and copy level_suffix */
        if (suffix_size > 0) {
            int level_suffix = br_read_bits(br, suffix_size);
            bitwriter_write_bits(bw, level_suffix, suffix_size);
            levelCode += level_suffix;
        }

        /* First coefficient after trailing ones: add 2 to levelCode */
        levelCode += ((i == 0) && (t1 < 3)) << 1;

        if (debug_block) {
            fprintf(stderr, "            level[%d]: prefix=%d suffix_size=%d levelCode=%d from %zu.%d to %zu.%d\n",
                    i, level_prefix, suffix_size, levelCode, lvl_start_byte, lvl_start_bit, br->byte_pos, br->bit_pos);
        }

        /* Compute levelVal from levelCode (OpenH264 formula) */
        int levelVal = ((levelCode + 2) >> 1);
        levelVal -= (levelVal << 1) & (-(levelCode & 1));

        /* Update suffix_length for next coefficient */
        suffix_length += !suffix_length;  /* If 0, becomes 1 */
        int threshold = 3 << (suffix_length - 1);
        int absLevel = (levelVal < 0) ? -levelVal : levelVal;
        suffix_length += (absLevel > threshold) && (suffix_length < 6);
    }

    /* Decode and copy total_zeros */
    int total_zeros = 0;
    if (tc < max_coeff) {
        size_t tz_byte = br->byte_pos;
        int tz_bit = br->bit_pos;

        total_zeros = decode_total_zeros(br, tc, max_coeff);
        if (total_zeros < 0) return -1;

        /* Copy the bits we consumed */
        size_t end_byte = br->byte_pos;
        int end_bit = br->bit_pos;
        br->byte_pos = tz_byte;
        br->bit_pos = tz_bit;

        int bits_consumed = (int)((end_byte - tz_byte) * 8 + (end_bit - tz_bit));
        if (debug_block) {
            fprintf(stderr, "          total_zeros=%d, bits=%d\n", total_zeros, bits_consumed);
        }
        copy_bits(br, bw, bits_consumed);
    }

    /* Decode and copy run_before for each coefficient except last */
    int zeros_left = total_zeros;
    for (int i = 0; i < tc - 1 && zeros_left > 0; i++) {
        size_t rb_byte = br->byte_pos;
        int rb_bit = br->bit_pos;

        int run = decode_run_before(br, zeros_left);
        if (run < 0) return -1;

        if (debug_block) {
            fprintf(stderr, "          run_before[%d]: zeros_left=%d run=%d at %zu.%d -> %zu.%d\n",
                    i, zeros_left, run, rb_byte, rb_bit, br->byte_pos, br->bit_pos);
        }

        /* Copy the bits we consumed */
        size_t end_byte = br->byte_pos;
        int end_bit = br->bit_pos;
        br->byte_pos = rb_byte;
        br->bit_pos = rb_bit;

        int bits_consumed = (int)((end_byte - rb_byte) * 8 + (end_bit - rb_bit));
        copy_bits(br, bw, bits_consumed);

        zeros_left -= run;
    }

    return tc;
}

/*
 * Context for tracking total_coeff values for nC computation
 * We track tc values for each 4x4 block position in raster order
 */
typedef struct {
    int luma_tc[16];      /* tc for 16 luma 4x4 blocks */
    int chroma_tc[2][4];  /* tc for 4 chroma AC blocks per plane */
} MBCoeffContext;

/* Global storage for previous MB's tc values (for left neighbor) */
static MBCoeffContext prev_mb_ctx;
/* Storage for top row of MBs */
static MBCoeffContext *top_mb_ctx = NULL;
static int top_mb_ctx_size = 0;

/*
 * Compute nC for a luma block based on neighbors
 * blk_idx: 0-15 in raster scan order within MB
 *
 * Raster order layout (for 4x4 blocks within 16x16 MB):
 *  0  1  2  3
 *  4  5  6  7
 *  8  9 10 11
 * 12 13 14 15
 */
static int compute_luma_nC(int blk_idx, int mb_col, const MBCoeffContext *cur, const MBCoeffContext *left, const MBCoeffContext *top) {
    /* For raster order: row = blk_idx / 4, col = blk_idx % 4 */
    int row = blk_idx / 4;
    int col = blk_idx % 4;

    int nA = -1, nB = -1;  /* -1 means not available */

    /* Left neighbor (same row, col-1) */
    if (col > 0) {
        /* Within same MB: left neighbor is blk_idx - 1 */
        int left_idx = blk_idx - 1;
        nA = cur->luma_tc[left_idx];
    } else if (mb_col > 0 && left != NULL) {
        /* From left MB (rightmost column): row*4 + 3 */
        int left_idx = row * 4 + 3;
        nA = left->luma_tc[left_idx];
    }

    /* Top neighbor (row-1, same col) */
    if (row > 0) {
        /* Within same MB: top neighbor is blk_idx - 4 */
        int top_idx = blk_idx - 4;
        nB = cur->luma_tc[top_idx];
    } else if (top != NULL) {
        /* From top MB (bottom row): 12 + col */
        int top_idx = 12 + col;
        nB = top->luma_tc[top_idx];
    }

    /* Compute nC */
    int nC;
    if (nA >= 0 && nB >= 0) {
        nC = (nA + nB + 1) >> 1;
    } else if (nA >= 0) {
        nC = nA;
    } else if (nB >= 0) {
        nC = nB;
    } else {
        nC = 0;
    }

    /* Debug for raster 13 */
    if (blk_idx == 13) {
        int left_idx = (col > 0) ? blk_idx - 1 : -1;
        int top_idx = (row > 0) ? blk_idx - 4 : -1;
        fprintf(stderr, "        nC debug for raster 13: row=%d col=%d left_idx=%d top_idx=%d nA=%d nB=%d nC=%d\n",
                row, col, left_idx, top_idx, nA, nB, nC);
        if (left_idx >= 0) fprintf(stderr, "          cur->luma_tc[%d]=%d\n", left_idx, cur->luma_tc[left_idx]);
        if (top_idx >= 0) fprintf(stderr, "          cur->luma_tc[%d]=%d\n", top_idx, cur->luma_tc[top_idx]);
    }

    return nC;
}

/*
 * Compute nC for a chroma AC block based on neighbors
 * plane: 0=Cb, 1=Cr
 * blk_idx: 0-3 within the plane
 */
static int compute_chroma_nC(int plane, int blk_idx, int mb_col, const MBCoeffContext *cur, const MBCoeffContext *left, const MBCoeffContext *top) {
    /* Chroma 4x4 block arrangement within MB plane:
     * 0 1
     * 2 3
     */
    int row = blk_idx / 2;
    int col = blk_idx % 2;

    int nA = -1, nB = -1;

    /* Left neighbor */
    if (col > 0) {
        nA = cur->chroma_tc[plane][blk_idx - 1];
    } else if (mb_col > 0 && left != NULL) {
        nA = left->chroma_tc[plane][row * 2 + 1];  /* Right column of left MB */
    }

    /* Top neighbor */
    if (row > 0) {
        nB = cur->chroma_tc[plane][blk_idx - 2];
    } else if (top != NULL) {
        nB = top->chroma_tc[plane][2 + col];  /* Bottom row of top MB */
    }

    if (nA >= 0 && nB >= 0) {
        return (nA + nB + 1) >> 1;
    } else if (nA >= 0) {
        return nA;
    } else if (nB >= 0) {
        return nB;
    }
    return 0;
}

/*
 * Copy I_4x4 macroblock residual with proper nC tracking
 * Block scan order for I_4x4: 8x8 blocks in Z-order, 4x4 within each 8x8 in Z-order
 * 8x8 block order: 0=top-left, 1=top-right, 2=bottom-left, 3=bottom-right
 * 4x4 within 8x8: also Z-order
 *
 * Mapping to raster-scan 4x4 indices:
 * 8x8[0]: 4x4[0,1,2,3] -> raster[0,1,4,5]
 * 8x8[1]: 4x4[0,1,2,3] -> raster[2,3,6,7]
 * 8x8[2]: 4x4[0,1,2,3] -> raster[8,9,12,13]
 * 8x8[3]: 4x4[0,1,2,3] -> raster[10,11,14,15]
 */
static int copy_i4x4_residual(BitReader *br, BitWriter *bw, uint32_t cbp,
                              int mb_row, int mb_col, MBCoeffContext *cur_ctx) {
    int cbp_luma = cbp & 0xF;
    int cbp_chroma = (cbp >> 4) & 0x3;

    /* Get neighbor contexts */
    MBCoeffContext *left = (mb_col > 0) ? &prev_mb_ctx : NULL;
    MBCoeffContext *top = (mb_row > 0 && top_mb_ctx != NULL) ? &top_mb_ctx[mb_col] : NULL;

    /* Initialize current context to 0 */
    memset(cur_ctx, 0, sizeof(*cur_ctx));

    /* Map from scan order to raster index */
    static const int scan_to_raster[16] = {
        0, 1, 4, 5,    /* 8x8 block 0 */
        2, 3, 6, 7,    /* 8x8 block 1 */
        8, 9, 12, 13,  /* 8x8 block 2 */
        10, 11, 14, 15 /* 8x8 block 3 */
    };

    fprintf(stderr, "    I_4x4 residual: cbp_luma=0x%x cbp_chroma=%d at %zu.%d, bytes: %02x %02x %02x %02x\n",
            cbp_luma, cbp_chroma, br->byte_pos, br->bit_pos,
            br->buffer[br->byte_pos], br->buffer[br->byte_pos+1],
            br->buffer[br->byte_pos+2], br->buffer[br->byte_pos+3]);

    /* 16 luma blocks in scan order */
    for (int i8x8 = 0; i8x8 < 4; i8x8++) {
        if (cbp_luma & (1 << i8x8)) {
            for (int i4x4 = 0; i4x4 < 4; i4x4++) {
                int scan_idx = i8x8 * 4 + i4x4;
                int raster_idx = scan_to_raster[scan_idx];
                int nC = compute_luma_nC(raster_idx, mb_col, cur_ctx, left, top);
                fprintf(stderr, "      Luma[%d,%d] scan=%d raster=%d nC=%d at %zu.%d\n",
                        i8x8, i4x4, scan_idx, raster_idx, nC, br->byte_pos, br->bit_pos);
                int tc = copy_cavlc_block(br, bw, nC, 16);
                if (tc < 0) return -1;
                fprintf(stderr, "        tc=%d, now at %zu.%d\n", tc, br->byte_pos, br->bit_pos);
                cur_ctx->luma_tc[raster_idx] = tc;
            }
        } else {
            /* No residual for this 8x8 block, set tc=0 for all 4x4 */
            for (int i4x4 = 0; i4x4 < 4; i4x4++) {
                int scan_idx = i8x8 * 4 + i4x4;
                int raster_idx = scan_to_raster[scan_idx];
                cur_ctx->luma_tc[raster_idx] = 0;
            }
        }
    }

    /* Chroma */
    if (cbp_chroma > 0) {
        fprintf(stderr, "      Chroma DC at %zu.%d (cbp_chroma=%d)\n", br->byte_pos, br->bit_pos, cbp_chroma);
        /* Cb DC */
        int cb_tc = copy_cavlc_block(br, bw, -1, 4);
        if (cb_tc < 0) return -1;
        fprintf(stderr, "        Cb DC tc=%d, now at %zu.%d\n", cb_tc, br->byte_pos, br->bit_pos);
        /* Cr DC */
        int cr_tc = copy_cavlc_block(br, bw, -1, 4);
        if (cr_tc < 0) return -1;
        fprintf(stderr, "        Cr DC tc=%d, now at %zu.%d\n", cr_tc, br->byte_pos, br->bit_pos);

        if (cbp_chroma == 2) {
            /* Chroma AC - 8 blocks with proper nC */
            for (int c = 0; c < 2; c++) {
                for (int i = 0; i < 4; i++) {
                    int nC = compute_chroma_nC(c, i, mb_col, cur_ctx, left, top);
                    fprintf(stderr, "        Chroma AC[%d,%d] nC=%d at %zu.%d\n", c, i, nC, br->byte_pos, br->bit_pos);
                    int tc = copy_cavlc_block(br, bw, nC, 15);
                    if (tc < 0) return -1;
                    cur_ctx->chroma_tc[c][i] = tc;
                    fprintf(stderr, "          tc=%d, now at %zu.%d\n", tc, br->byte_pos, br->bit_pos);
                }
            }
        }
    }
    fprintf(stderr, "      I_4x4 residual done at %zu.%d\n", br->byte_pos, br->bit_pos);

    return 0;
}


/*
 * Copy I_16x16 macroblock residual with proper nC tracking
 */
static int copy_i16x16_residual(BitReader *br, BitWriter *bw, int cbp_luma, int cbp_chroma,
                                int mb_row, int mb_col, MBCoeffContext *cur_ctx) {
    static int debug_count = 0;
    /* Debug row 6, row 15 (and more) to catch frame 3's problem area */
    int do_debug = (mb_row == 6) || (mb_row == 15) || (debug_count >= 5 && debug_count <= 7);
    debug_count++;

    /* Get neighbor contexts */
    MBCoeffContext *left = (mb_col > 0) ? &prev_mb_ctx : NULL;
    MBCoeffContext *top = (mb_row > 0 && top_mb_ctx != NULL) ? &top_mb_ctx[mb_col] : NULL;

    /* Initialize current context to 0 */
    memset(cur_ctx, 0, sizeof(*cur_ctx));

    /* Luma DC block (16 coefficients) - uses nC based on neighbors */
    if (do_debug) {
        fprintf(stderr, "    Luma DC at %zu.%d, next bytes: %02x %02x %02x %02x\n",
                br->byte_pos, br->bit_pos,
                br->buffer[br->byte_pos], br->buffer[br->byte_pos+1],
                br->buffer[br->byte_pos+2], br->buffer[br->byte_pos+3]);
    }
    int dc_nC = compute_luma_nC(0, mb_col, cur_ctx, left, top);
    int tc = copy_cavlc_block(br, bw, dc_nC, 16);
    if (tc < 0) return -1;
    if (do_debug) fprintf(stderr, "      -> tc=%d (nC=%d), now at %zu.%d\n", tc, dc_nC, br->byte_pos, br->bit_pos);

    /* Luma AC blocks (15 coefficients each) - in scan order, convert to raster */
    static const int scan_to_raster[16] = {
        0, 1, 4, 5,    /* 8x8 block 0 */
        2, 3, 6, 7,    /* 8x8 block 1 */
        8, 9, 12, 13,  /* 8x8 block 2 */
        10, 11, 14, 15 /* 8x8 block 3 */
    };
    for (int i8x8 = 0; i8x8 < 4; i8x8++) {
        for (int i4x4 = 0; i4x4 < 4; i4x4++) {
            int scan_idx = i8x8 * 4 + i4x4;
            int raster_idx = scan_to_raster[scan_idx];
            if (cbp_luma & (1 << i8x8)) {
                int nC = compute_luma_nC(raster_idx, mb_col, cur_ctx, left, top);
                if (do_debug) fprintf(stderr, "    Luma AC[%d] raster=%d at %zu.%d (nC=%d)\n", scan_idx, raster_idx, br->byte_pos, br->bit_pos, nC);
                tc = copy_cavlc_block(br, bw, nC, 15);
                if (tc < 0) return -1;
                cur_ctx->luma_tc[raster_idx] = tc;
                if (do_debug) fprintf(stderr, "      -> tc=%d, now at %zu.%d\n", tc, br->byte_pos, br->bit_pos);
            } else {
                cur_ctx->luma_tc[raster_idx] = 0;
            }
        }
    }

    /* Chroma */
    if (cbp_chroma > 0) {
        /* Cb DC */
        if (do_debug) fprintf(stderr, "    Cb DC at %zu.%d\n", br->byte_pos, br->bit_pos);
        tc = copy_cavlc_block(br, bw, -1, 4);
        if (tc < 0) return -1;
        if (do_debug) fprintf(stderr, "      -> tc=%d, now at %zu.%d\n", tc, br->byte_pos, br->bit_pos);

        /* Cr DC */
        if (do_debug) fprintf(stderr, "    Cr DC at %zu.%d\n", br->byte_pos, br->bit_pos);
        tc = copy_cavlc_block(br, bw, -1, 4);
        if (tc < 0) return -1;
        if (do_debug) fprintf(stderr, "      -> tc=%d, now at %zu.%d\n", tc, br->byte_pos, br->bit_pos);

        if (cbp_chroma == 2) {
            /* Chroma AC - 8 blocks */
            for (int c = 0; c < 2; c++) {
                for (int i = 0; i < 4; i++) {
                    int nC = compute_chroma_nC(c, i, mb_col, cur_ctx, left, top);
                    int idx = c * 4 + i;
                    if (do_debug) fprintf(stderr, "    Chroma AC[%d] at %zu.%d (nC=%d)\n", idx, br->byte_pos, br->bit_pos, nC);
                    tc = copy_cavlc_block(br, bw, nC, 15);
                    if (tc < 0) return -1;
                    cur_ctx->chroma_tc[c][i] = tc;
                    if (do_debug) fprintf(stderr, "      -> tc=%d, now at %zu.%d\n", tc, br->byte_pos, br->bit_pos);
                }
            }
        }
    }

    if (do_debug) {
        fprintf(stderr, "    I_16x16 residual done at %zu.%d, next bytes: %02x %02x %02x %02x\n",
                br->byte_pos, br->bit_pos,
                br->buffer[br->byte_pos], br->buffer[br->byte_pos+1],
                br->buffer[br->byte_pos+2], br->buffer[br->byte_pos+3]);
    }
    return 0;
}

/*
 * Process I-slice: copy MBs and add I_PCM padding
 */
static int process_i_slice(BitReader *br, BitWriter *bw) {
    printf("Processing I-slice...\n");

    /* Allocate context storage for top row if needed */
    if (top_mb_ctx == NULL || top_mb_ctx_size < INPUT_MB_WIDTH) {
        free(top_mb_ctx);
        top_mb_ctx = calloc(INPUT_MB_WIDTH, sizeof(MBCoeffContext));
        top_mb_ctx_size = INPUT_MB_WIDTH;
    }

    /* Current row context storage */
    MBCoeffContext *cur_row_ctx = calloc(INPUT_MB_WIDTH, sizeof(MBCoeffContext));

    for (int row = 0; row < INPUT_MB_HEIGHT; row++) {
        /* Reset previous MB context at start of each row */
        memset(&prev_mb_ctx, 0, sizeof(prev_mb_ctx));

        /* Track bit positions at start of row */
        size_t row_start_input_bits = br->byte_pos * 8 + br->bit_pos;
        size_t row_start_output_bits = bitwriter_get_bit_position(bw);

        /* Copy original MBs for this row */
        for (int col = 0; col < INPUT_MB_WIDTH; col++) {
            size_t mb_start_byte = br->byte_pos;
            int mb_start_bit = br->bit_pos;
            size_t mb_start_out_byte = bw->byte_pos;
            int mb_start_out_bit = bw->bit_pos;

            uint32_t mb_type = br_read_ue(br);
            bitwriter_write_ue(bw, mb_type);

            fprintf(stderr, "  MB[%d,%d]: type=%d at byte=%zu bit=%d  [%02x %02x %02x]\n",
                    row, col, mb_type, mb_start_byte, mb_start_bit,
                    br->buffer[mb_start_byte], br->buffer[mb_start_byte+1], br->buffer[mb_start_byte+2]);

            /* For first few MBs of rows 0 and 1, compare output bytes with input */
            if ((row <= 1) && col == 0) {
                /* At MB start, input and output should be at aligned positions */
                /* After row 0, output has I_PCM padding, so positions differ */
                /* But the BITS should match at corresponding positions */
                fprintf(stderr, "    >>> MB[%d,%d] START: input=%zu.%d output=%zu.%d\n",
                        row, col, mb_start_byte, mb_start_bit, mb_start_out_byte, mb_start_out_bit);

                /* For MB[1,0], trace the first 32 bits of input and what we wrote */
                if (row == 1) {
                    fprintf(stderr, "    >>> MB[1,0] input bytes: %02x %02x %02x %02x %02x\n",
                            br->buffer[mb_start_byte], br->buffer[mb_start_byte+1],
                            br->buffer[mb_start_byte+2], br->buffer[mb_start_byte+3],
                            br->buffer[mb_start_byte+4]);
                    fprintf(stderr, "    >>> MB[1,0] output bytes: %02x %02x %02x %02x %02x\n",
                            bw->buffer[mb_start_out_byte], bw->buffer[mb_start_out_byte+1],
                            bw->buffer[mb_start_out_byte+2], bw->buffer[mb_start_out_byte+3],
                            bw->buffer[mb_start_out_byte+4]);

                    /* Show mb_type that was just read/written */
                    fprintf(stderr, "    >>> MB[1,0] mb_type=%d (ue encoding)\n", mb_type);
                    fprintf(stderr, "    >>> MB[1,0] after mb_type: input=%zu.%d output=%zu.%d\n",
                            br->byte_pos, br->bit_pos, bw->byte_pos, bw->bit_pos);
                    fprintf(stderr, "    >>> MB[1,0] output current_byte=%02x (not yet flushed)\n", bw->current_byte);

                    /* Flush and check */
                    uint8_t saved_byte = bw->current_byte;
                    int saved_bit = bw->bit_pos;
                    size_t saved_pos = bw->byte_pos;
                    bitwriter_flush(bw);
                    fprintf(stderr, "    >>> MB[1,0] output bytes after flush: %02x %02x %02x %02x\n",
                            bw->buffer[mb_start_out_byte], bw->buffer[mb_start_out_byte+1],
                            bw->buffer[mb_start_out_byte+2], bw->buffer[mb_start_out_byte+3]);
                    /* Restore (this is hacky but for debug) */
                    bw->byte_pos = saved_pos;
                    bw->bit_pos = saved_bit;
                    bw->current_byte = saved_byte;
                }
            }

            if (mb_type == 0) {
                /* I_4x4 */
                /* Copy 16 intra prediction modes */
                fprintf(stderr, "    I_4x4 pred modes at %zu.%d:\n", br->byte_pos, br->bit_pos);
                for (int blk = 0; blk < 16; blk++) {
                    int prev_flag = br_read_bit(br);
                    bitwriter_write_bit(bw, prev_flag);
                    if (!prev_flag) {
                        int rem = br_read_bits(br, 3);
                        bitwriter_write_bits(bw, rem, 3);
                        fprintf(stderr, "      [%2d] prev=0 rem=%d at %zu.%d\n", blk, rem, br->byte_pos, br->bit_pos);
                    } else {
                        fprintf(stderr, "      [%2d] prev=1 at %zu.%d\n", blk, br->byte_pos, br->bit_pos);
                    }
                }
                fprintf(stderr, "    After pred modes at %zu.%d\n", br->byte_pos, br->bit_pos);

                /* intra_chroma_pred_mode */
                size_t cp_byte = br->byte_pos;
                int cp_bit = br->bit_pos;
                fprintf(stderr, "    chroma_pred UE at %zu.%d: bytes=%02x %02x %02x\n",
                        cp_byte, cp_bit, br->buffer[cp_byte], br->buffer[cp_byte+1], br->buffer[cp_byte+2]);
                uint32_t chroma_pred = br_read_ue(br);
                fprintf(stderr, "    chroma_pred=%u at %zu.%d\n", chroma_pred, br->byte_pos, br->bit_pos);
                if (chroma_pred > 3) {
                    fprintf(stderr, "Invalid chroma_pred %d at MB[%d,%d] (byte %zu bit %d)\n",
                            chroma_pred, row, col, br->byte_pos, br->bit_pos);
                    fprintf(stderr, "    Read from byte %zu bit %d\n", cp_byte, cp_bit);
                    return -1;
                }
                bitwriter_write_ue(bw, chroma_pred);

                /* coded_block_pattern */
                uint32_t cbp_code = br_read_ue(br);
                uint32_t cbp = (cbp_code < 48) ? cbp_intra_table[cbp_code] : 0;
                fprintf(stderr, "    cbp_code=%u cbp=%u (luma=%u chroma=%u) at %zu.%d\n",
                        cbp_code, cbp, cbp & 0xF, (cbp >> 4) & 0x3, br->byte_pos, br->bit_pos);
                bitwriter_write_ue(bw, cbp_code);

                if (cbp > 0) {
                    /* mb_qp_delta */
                    int32_t qp_delta = br_read_se(br);
                    fprintf(stderr, "    qp_delta=%d at %zu.%d\n", qp_delta, br->byte_pos, br->bit_pos);
                    bitwriter_write_se(bw, qp_delta);

                    /* Copy residual with proper nC tracking */
                    if (copy_i4x4_residual(br, bw, cbp, row, col, &cur_row_ctx[col]) < 0) {
                        fprintf(stderr, "Failed to copy I_4x4 residual at MB[%d,%d]\n", row, col);
                        free(cur_row_ctx);
                        return -1;
                    }
                } else {
                    /* No residual, zero context */
                    memset(&cur_row_ctx[col], 0, sizeof(MBCoeffContext));
                }

                /* Save context for next MB's left neighbor */
                memcpy(&prev_mb_ctx, &cur_row_ctx[col], sizeof(MBCoeffContext));

            } else if (mb_type >= 1 && mb_type <= 24) {
                /* I_16x16 */
                int idx = mb_type - 1;
                int cbp_chroma = (idx / 4) % 3;
                int cbp_luma = (idx / 12) ? 15 : 0;

                fprintf(stderr, "    I_16x16 idx=%d cbp_luma=%d cbp_chroma=%d at %zu.%d\n",
                        idx, cbp_luma, cbp_chroma, br->byte_pos, br->bit_pos);

                /* intra_chroma_pred_mode */
                uint32_t chroma_pred = br_read_ue(br);
                if (chroma_pred > 3) {
                    fprintf(stderr, "Invalid chroma_pred %d at MB[%d,%d]\n", chroma_pred, row, col);
                    free(cur_row_ctx);
                    return -1;
                }
                bitwriter_write_ue(bw, chroma_pred);
                fprintf(stderr, "    chroma_pred=%d at %zu.%d\n", chroma_pred, br->byte_pos, br->bit_pos);

                /* mb_qp_delta (always present for I_16x16) */
                int32_t qp_delta = br_read_se(br);
                bitwriter_write_se(bw, qp_delta);
                fprintf(stderr, "    qp_delta=%d at %zu.%d\n", qp_delta, br->byte_pos, br->bit_pos);

                /* Copy residual with proper nC tracking */
                if (copy_i16x16_residual(br, bw, cbp_luma, cbp_chroma, row, col, &cur_row_ctx[col]) < 0) {
                    fprintf(stderr, "Failed to copy I_16x16 residual at MB[%d,%d]\n", row, col);
                    free(cur_row_ctx);
                    return -1;
                }

                /* Save context for next MB's left neighbor */
                memcpy(&prev_mb_ctx, &cur_row_ctx[col], sizeof(MBCoeffContext));

            } else if (mb_type == 25) {
                /* I_PCM */
                /* Align and copy 384 bytes */
                while (br->bit_pos != 0) br_read_bit(br);
                bitwriter_align(bw);

                for (int i = 0; i < 384; i++) {
                    bitwriter_write_bits(bw, br_read_bits(br, 8), 8);
                }

                /* I_PCM resets all coefficients to 0 for nC purposes */
                memset(&cur_row_ctx[col], 0, sizeof(MBCoeffContext));
                memset(&prev_mb_ctx, 0, sizeof(prev_mb_ctx));
            } else {
                fprintf(stderr, "Unknown I mb_type %d at MB[%d,%d]\n", mb_type, row, col);
                free(cur_row_ctx);
                return -1;
            }

            /* End-of-MB verification for first row MBs */
            if (row == 0) {
                size_t mb_end_byte = br->byte_pos;
                int mb_end_bit = br->bit_pos;
                size_t mb_end_out_byte = bw->byte_pos;
                int mb_end_out_bit = bw->bit_pos;

                size_t input_bits = (mb_end_byte - mb_start_byte) * 8 + (mb_end_bit - mb_start_bit);
                size_t output_bits = (mb_end_out_byte - mb_start_out_byte) * 8 + (mb_end_out_bit - mb_start_out_bit);

                if (input_bits != output_bits) {
                    fprintf(stderr, "    >>> MB[%d,%d] BIT MISMATCH: input=%zu bits, output=%zu bits (diff=%zd)\n",
                            row, col, input_bits, output_bits, (ssize_t)output_bits - (ssize_t)input_bits);
                }
            }
        }

        /* Check bit position tracking - input and output should have consumed same number of bits */
        size_t row_end_input_bits = br->byte_pos * 8 + br->bit_pos;
        size_t row_end_output_bits = bitwriter_get_bit_position(bw);
        size_t input_bits_consumed = row_end_input_bits - row_start_input_bits;
        size_t output_bits_written = row_end_output_bits - row_start_output_bits;

        fprintf(stderr, "  Row %d bit tracking: input consumed %zu bits, output wrote %zu bits (diff=%zd)\n",
                row, input_bits_consumed, output_bits_written,
                (ssize_t)output_bits_written - (ssize_t)input_bits_consumed);

        if (input_bits_consumed != output_bits_written) {
            fprintf(stderr, "*** BIT POSITION MISMATCH at row %d! ***\n", row);
            fprintf(stderr, "    Input: %zu -> %zu (%zu bits)\n",
                    row_start_input_bits, row_end_input_bits, input_bits_consumed);
            fprintf(stderr, "    Output: %zu -> %zu (%zu bits)\n",
                    row_start_output_bits, row_end_output_bits, output_bits_written);
        }

        /* Compare actual bytes for rows 0-2 to verify bit values match */
        if (row <= 2) {
            size_t start_byte = row_start_input_bits / 8;
            size_t end_byte = (row_end_input_bits + 7) / 8;
            size_t out_start_byte = row_start_output_bits / 8;

            fprintf(stderr, "  Row %d byte comparison (input bytes %zu-%zu vs output bytes %zu-):\n",
                    row, start_byte, end_byte, out_start_byte);

            int first_diff = -1;
            for (size_t i = 0; i < end_byte - start_byte && i < 20; i++) {
                uint8_t in_byte = br->buffer[start_byte + i];
                uint8_t out_byte = bw->buffer[out_start_byte + i];
                if (in_byte != out_byte && first_diff < 0) {
                    first_diff = (int)i;
                }
                if (i < 10 || (first_diff >= 0 && (int)i <= first_diff + 5)) {
                    fprintf(stderr, "    [%zu] in=%02x out=%02x %s\n",
                            i, in_byte, out_byte, in_byte != out_byte ? "<<< DIFF" : "");
                }
            }
            if (first_diff >= 0) {
                fprintf(stderr, "    First difference at offset %d in row %d\n", first_diff, row);
            }
        }

        /* Add padding MBs (I_PCM) - these have zero coefficients
         *
         * IMPORTANT: The first I_PCM MB (column 20) needs special handling.
         * Its bottom-left samples serve as "top-right" reference for the
         * NEXT row's rightmost original MB's intra prediction.
         *
         * Without this fix, the decoder sees I_PCM's black (Y=16) samples
         * instead of the extrapolated samples the encoder expected, causing
         * prediction mismatch and visual corruption.
         *
         * We use edge_y=235 as a reasonable approximation for bright content.
         * A proper solution would extract the actual edge value from the
         * decoded frame.
         */
        size_t before_ipcm_byte = bw->byte_pos;
        int before_ipcm_bit = bw->bit_pos;
        for (int p = 0; p < PADDING_MBS_PER_ROW; p++) {
            if (p == 0) {
                /* First I_PCM: use edge-compatible Y values for intra prediction
                 *
                 * Y=235 fixes I-frame corruption (max_diff ~3) but P-frames may
                 * still show edge distortion when motion compensation references
                 * padding area with I_PCM (neutral chroma) instead of original
                 * edge colors. For perfect results, would need to extract actual
                 * edge YCbCr values from decoded frame.
                 */
                write_ipcm_mb_edge(bw, 235, 1);
            } else {
                write_ipcm_mb(bw);
            }
        }
        size_t after_ipcm_byte = bw->byte_pos;
        int after_ipcm_bit = bw->bit_pos;
        fprintf(stderr, "  Row %d I_PCM padding: output %zu.%d -> %zu.%d (%zu bytes written)\n",
                row, before_ipcm_byte, before_ipcm_bit, after_ipcm_byte, after_ipcm_bit,
                after_ipcm_byte - before_ipcm_byte);

        /* Swap row contexts: current row becomes top row for next iteration */
        memcpy(top_mb_ctx, cur_row_ctx, INPUT_MB_WIDTH * sizeof(MBCoeffContext));

        printf("  Row %d: copied %d MBs, added %d padding\n",
               row, INPUT_MB_WIDTH, PADDING_MBS_PER_ROW);
    }

    free(cur_row_ctx);
    return 0;
}

/*
 * Copy inter (P) macroblock residual with proper nC tracking
 */
static int copy_inter_residual(BitReader *br, BitWriter *bw, uint32_t cbp,
                               int mb_row, int mb_col, MBCoeffContext *cur_ctx) {
    int cbp_luma = cbp & 0xF;
    int cbp_chroma = (cbp >> 4) & 0x3;
    int do_debug = (mb_row == 15 && (mb_col == 3 || mb_col == 4));

    if (do_debug) {
        fprintf(stderr, "      Inter residual MB[%d,%d]: cbp_luma=%d cbp_chroma=%d at %zu.%d\n",
                mb_row, mb_col, cbp_luma, cbp_chroma, br->byte_pos, br->bit_pos);
    }

    /* Get neighbor contexts */
    MBCoeffContext *left = (mb_col > 0) ? &prev_mb_ctx : NULL;
    MBCoeffContext *top = (mb_row > 0 && top_mb_ctx != NULL) ? &top_mb_ctx[mb_col] : NULL;

    /* Initialize current context to 0 */
    memset(cur_ctx, 0, sizeof(*cur_ctx));

    /* Map from scan order to raster index */
    static const int scan_to_raster[16] = {
        0, 1, 4, 5,    /* 8x8 block 0 */
        2, 3, 6, 7,    /* 8x8 block 1 */
        8, 9, 12, 13,  /* 8x8 block 2 */
        10, 11, 14, 15 /* 8x8 block 3 */
    };

    /* 16 luma blocks in scan order */
    for (int i8x8 = 0; i8x8 < 4; i8x8++) {
        if (cbp_luma & (1 << i8x8)) {
            for (int i4x4 = 0; i4x4 < 4; i4x4++) {
                int scan_idx = i8x8 * 4 + i4x4;
                int raster_idx = scan_to_raster[scan_idx];
                int nC = compute_luma_nC(raster_idx, mb_col, cur_ctx, left, top);
                if (do_debug) {
                    fprintf(stderr, "        Luma[%d] raster=%d nC=%d at %zu.%d\n",
                            scan_idx, raster_idx, nC, br->byte_pos, br->bit_pos);
                }
                int tc = copy_cavlc_block(br, bw, nC, 16);
                if (tc < 0) return -1;
                cur_ctx->luma_tc[raster_idx] = tc;
                if (do_debug) {
                    fprintf(stderr, "          tc=%d, now at %zu.%d\n", tc, br->byte_pos, br->bit_pos);
                }
            }
        } else {
            /* No residual for this 8x8 block, set tc=0 for all 4x4 */
            for (int i4x4 = 0; i4x4 < 4; i4x4++) {
                int scan_idx = i8x8 * 4 + i4x4;
                int raster_idx = scan_to_raster[scan_idx];
                cur_ctx->luma_tc[raster_idx] = 0;
            }
        }
    }

    /* Chroma */
    if (cbp_chroma > 0) {
        if (do_debug) {
            fprintf(stderr, "        Cb DC at %zu.%d\n", br->byte_pos, br->bit_pos);
        }
        /* Cb DC */
        int cb_tc = copy_cavlc_block(br, bw, -1, 4);
        if (cb_tc < 0) return -1;
        if (do_debug) {
            fprintf(stderr, "          tc=%d, now at %zu.%d\n", cb_tc, br->byte_pos, br->bit_pos);
            fprintf(stderr, "        Cr DC at %zu.%d\n", br->byte_pos, br->bit_pos);
        }
        /* Cr DC */
        int cr_tc = copy_cavlc_block(br, bw, -1, 4);
        if (cr_tc < 0) return -1;
        if (do_debug) {
            fprintf(stderr, "          tc=%d, now at %zu.%d\n", cr_tc, br->byte_pos, br->bit_pos);
        }

        if (cbp_chroma == 2) {
            /* Chroma AC - 8 blocks with proper nC */
            for (int c = 0; c < 2; c++) {
                for (int i = 0; i < 4; i++) {
                    int nC = compute_chroma_nC(c, i, mb_col, cur_ctx, left, top);
                    int tc = copy_cavlc_block(br, bw, nC, 15);
                    if (tc < 0) return -1;
                    cur_ctx->chroma_tc[c][i] = tc;
                }
            }
        }
    }

    if (do_debug) {
        fprintf(stderr, "      Inter residual done at %zu.%d\n", br->byte_pos, br->bit_pos);
    }
    return 0;
}

/*
 * P-slice macroblock types (for mb_type after skip_run):
 * 0 = P_L0_16x16:  1 partition (16x16), 1 ref_idx, 1 mvd
 * 1 = P_L0_L0_16x8: 2 partitions (16x8), 2 ref_idx, 2 mvd
 * 2 = P_L0_L0_8x16: 2 partitions (8x16), 2 ref_idx, 2 mvd
 * 3 = P_8x8:       4 sub-partitions (8x8 each), sub_mb_type for each
 * 4 = P_8x8ref0:   like P_8x8 but ref_idx=0 for all
 * 5+ = Intra modes (I_4x4 at 5, I_16x16 at 6-29, I_PCM at 30)
 *
 * Sub-macroblock types for P_8x8:
 * 0 = P_L0_8x8:   1 partition (8x8), 1 mvd
 * 1 = P_L0_8x4:   2 partitions (8x4), 2 mvd
 * 2 = P_L0_4x8:   2 partitions (4x8), 2 mvd
 * 3 = P_L0_4x4:   4 partitions (4x4), 4 mvd
 */

/* Number of MVs per sub_mb_type */
static const int sub_mb_mvs[4] = {1, 2, 2, 4};

/*
 * Process P-slice: copy MBs and add skip padding
 *
 * P-slice encoding is:
 *   mb_skip_run [coded_mb mb_skip_run]* (until end of slice)
 *
 * After a skip_run, there MUST be a coded MB (unless at end of slice).
 * We can't write consecutive skip_runs!
 *
 * Strategy:
 * - Accumulate pending_skips (input skips + padding at row boundaries)
 * - Only write skip_run immediately before a coded MB or at slice end
 */
static int process_p_slice(BitReader *br, BitWriter *bw, int num_ref_idx_l0) {
    printf("Processing P-slice (num_ref=%d)...\n", num_ref_idx_l0);

    /* Allocate context storage for top row if needed */
    if (top_mb_ctx == NULL || top_mb_ctx_size < INPUT_MB_WIDTH) {
        free(top_mb_ctx);
        top_mb_ctx = calloc(INPUT_MB_WIDTH, sizeof(MBCoeffContext));
        top_mb_ctx_size = INPUT_MB_WIDTH;
    }

    /* Current row context storage */
    MBCoeffContext *cur_row_ctx = calloc(INPUT_MB_WIDTH, sizeof(MBCoeffContext));

    int row = 0, col = 0;
    int pending_output_skips = 0;  /* ALL accumulated skips for output (including padding) */
    int input_mb_total = INPUT_MB_WIDTH * INPUT_MB_HEIGHT;
    int input_mb_count = 0;

    while (input_mb_count < input_mb_total) {
        /* At start of each row, reset left context */
        if (col == 0) {
            memset(&prev_mb_ctx, 0, sizeof(prev_mb_ctx));
        }

        /* Read mb_skip_run from input */
        uint32_t skip_run = br_read_ue(br);
        fprintf(stderr, "  [skip_run=%u row=%d col=%d count=%d/%d at %zu.%d]\n",
                skip_run, row, col, input_mb_count, input_mb_total, br->byte_pos, br->bit_pos);

        /* Process each skipped MB - accumulate but don't write yet */
        while (skip_run > 0 && input_mb_count < input_mb_total) {
            /* Zero context for skipped MB */
            memset(&cur_row_ctx[col], 0, sizeof(MBCoeffContext));
            memset(&prev_mb_ctx, 0, sizeof(prev_mb_ctx));

            pending_output_skips++;
            col++;
            input_mb_count++;
            skip_run--;

            /* End of input row? Add padding to pending skips */
            if (col == INPUT_MB_WIDTH) {
                pending_output_skips += PADDING_MBS_PER_ROW;

                /* Save row context for next row */
                memcpy(top_mb_ctx, cur_row_ctx, INPUT_MB_WIDTH * sizeof(MBCoeffContext));
                printf("  Row %d: skipped (pending=%d)\n", row, pending_output_skips);

                row++;
                col = 0;
            }
        }

        /* If we've processed all input MBs, write final skip_run and we're done */
        if (input_mb_count >= input_mb_total) {
            if (pending_output_skips > 0) {
                bitwriter_write_ue(bw, pending_output_skips);
                printf("  Final skip_run: %d\n", pending_output_skips);
            }
            break;
        }

        /* Otherwise, we have a coded MB to process */
        /* Write pending skips before this coded MB */
        bitwriter_write_ue(bw, pending_output_skips);
        pending_output_skips = 0;

        /* Read coded macroblock */
        uint32_t mb_type = br_read_ue(br);

        fprintf(stderr, "  P MB[%d,%d]: type=%d at %zu.%d\n",
                row, col, mb_type, br->byte_pos, br->bit_pos);

        bitwriter_write_ue(bw, mb_type);

        if (mb_type <= 4) {
            /* Inter prediction mode */
            int num_partitions;
            if (mb_type == 0) num_partitions = 1;       /* 16x16 */
            else if (mb_type <= 2) num_partitions = 2;  /* 16x8 or 8x16 */
            else num_partitions = 4;                     /* 8x8 */

            /* For P_8x8 and P_8x8ref0, read sub_mb_type for each 8x8 partition */
            int sub_mb_type[4] = {0, 0, 0, 0};
            int total_mvs = num_partitions;

            if (mb_type >= 3) {
                /* P_8x8 or P_8x8ref0 */
                total_mvs = 0;
                for (int i = 0; i < 4; i++) {
                    sub_mb_type[i] = br_read_ue(br);
                    bitwriter_write_ue(bw, sub_mb_type[i]);
                    if (sub_mb_type[i] < 4) {
                        total_mvs += sub_mb_mvs[sub_mb_type[i]];
                    }
                }
            }

            /* Read ref_idx if num_ref_idx_l0 > 1 */
            /* ref_idx is coded as te(num_ref_idx_l0_active_minus1):
             * - When max_ref_idx == 1 (2 refs): coded as u(1) - single bit
             * - When max_ref_idx > 1 (3+ refs): coded as ue(v)
             */
            if (num_ref_idx_l0 > 1) {
                int max_ref_idx = num_ref_idx_l0 - 1;
                if (mb_type == 4) {
                    /* P_8x8ref0: ref_idx is implicitly 0 */
                } else if (mb_type == 3) {
                    /* P_8x8: ref_idx for each 8x8 partition */
                    for (int i = 0; i < 4; i++) {
                        int ref_idx;
                        if (max_ref_idx == 1) {
                            ref_idx = br_read_bit(br);
                            bitwriter_write_bit(bw, ref_idx);
                        } else {
                            ref_idx = br_read_ue(br);
                            bitwriter_write_ue(bw, ref_idx);
                        }
                    }
                } else {
                    /* P_L0 modes: ref_idx for each partition */
                    for (int i = 0; i < num_partitions; i++) {
                        int ref_idx;
                        if (max_ref_idx == 1) {
                            ref_idx = br_read_bit(br);
                            bitwriter_write_bit(bw, ref_idx);
                        } else {
                            ref_idx = br_read_ue(br);
                            bitwriter_write_ue(bw, ref_idx);
                        }
                    }
                }
            }

            /* Read motion vectors (mvd_l0) */
            if (mb_type < 3) {
                /* P_L0_16x16, P_L0_L0_16x8, P_L0_L0_8x16 */
                for (int i = 0; i < num_partitions; i++) {
                    int mvd_x = br_read_se(br);
                    int mvd_y = br_read_se(br);
                    fprintf(stderr, "    mv[%d]: (%d,%d) at %zu.%d\n", i, mvd_x, mvd_y, br->byte_pos, br->bit_pos);
                    bitwriter_write_se(bw, mvd_x);
                    bitwriter_write_se(bw, mvd_y);
                }
            } else {
                /* P_8x8 / P_8x8ref0: MVs depend on sub_mb_type */
                for (int i = 0; i < 4; i++) {
                    int num_sub_mvs = (sub_mb_type[i] < 4) ? sub_mb_mvs[sub_mb_type[i]] : 1;
                    for (int j = 0; j < num_sub_mvs; j++) {
                        int mvd_x = br_read_se(br);
                        int mvd_y = br_read_se(br);
                        bitwriter_write_se(bw, mvd_x);
                        bitwriter_write_se(bw, mvd_y);
                    }
                }
            }

            /* coded_block_pattern (inter table) */
            uint32_t cbp_code = br_read_ue(br);
            uint32_t cbp = (cbp_code < 48) ? cbp_inter_table[cbp_code] : 0;
            bitwriter_write_ue(bw, cbp_code);
            fprintf(stderr, "    cbp_code=%u cbp=%u at %zu.%d\n", cbp_code, cbp, br->byte_pos, br->bit_pos);

            if (cbp > 0) {
                /* mb_qp_delta */
                int32_t qp_delta = br_read_se(br);
                bitwriter_write_se(bw, qp_delta);
                fprintf(stderr, "    qp_delta=%d, now at %zu.%d\n", qp_delta, br->byte_pos, br->bit_pos);

                /* Copy residual */
                if (copy_inter_residual(br, bw, cbp, row, col, &cur_row_ctx[col]) < 0) {
                    fprintf(stderr, "Failed to copy inter residual at MB[%d,%d]\n", row, col);
                    free(cur_row_ctx);
                    return -1;
                }
                fprintf(stderr, "    residual done at %zu.%d\n", br->byte_pos, br->bit_pos);
            } else {
                memset(&cur_row_ctx[col], 0, sizeof(MBCoeffContext));
            }

            memcpy(&prev_mb_ctx, &cur_row_ctx[col], sizeof(MBCoeffContext));

        } else if (mb_type == 5) {
            /* I_4x4 in P-slice */
            fprintf(stderr, "    I_4x4 in P-slice\n");
            /* Copy 16 intra prediction modes */
            for (int blk = 0; blk < 16; blk++) {
                int prev_flag = br_read_bit(br);
                bitwriter_write_bit(bw, prev_flag);
                if (!prev_flag) {
                    int rem = br_read_bits(br, 3);
                    bitwriter_write_bits(bw, rem, 3);
                }
            }
            fprintf(stderr, "    pred modes done at %zu.%d\n", br->byte_pos, br->bit_pos);

            /* intra_chroma_pred_mode */
            uint32_t chroma_pred = br_read_ue(br);
            bitwriter_write_ue(bw, chroma_pred);
            fprintf(stderr, "    chroma_pred=%u at %zu.%d\n", chroma_pred, br->byte_pos, br->bit_pos);

            /* coded_block_pattern (intra table) */
            uint32_t cbp_code = br_read_ue(br);
            uint32_t cbp = (cbp_code < 48) ? cbp_intra_table[cbp_code] : 0;
            bitwriter_write_ue(bw, cbp_code);
            fprintf(stderr, "    cbp_code=%u cbp=%u at %zu.%d\n", cbp_code, cbp, br->byte_pos, br->bit_pos);

            if (cbp > 0) {
                int32_t qp_delta = br_read_se(br);
                bitwriter_write_se(bw, qp_delta);
                fprintf(stderr, "    qp_delta=%d at %zu.%d\n", qp_delta, br->byte_pos, br->bit_pos);

                if (copy_i4x4_residual(br, bw, cbp, row, col, &cur_row_ctx[col]) < 0) {
                    fprintf(stderr, "Failed to copy I_4x4 residual in P-slice at MB[%d,%d]\n", row, col);
                    free(cur_row_ctx);
                    return -1;
                }
                fprintf(stderr, "    I_4x4 residual done at %zu.%d\n", br->byte_pos, br->bit_pos);
            } else {
                memset(&cur_row_ctx[col], 0, sizeof(MBCoeffContext));
            }

            memcpy(&prev_mb_ctx, &cur_row_ctx[col], sizeof(MBCoeffContext));

        } else if (mb_type >= 6 && mb_type <= 29) {
            /* I_16x16 in P-slice */
            int idx = mb_type - 6;
            int cbp_chroma = (idx / 4) % 3;
            int cbp_luma = (idx / 12) ? 15 : 0;
            fprintf(stderr, "    I_16x16: idx=%d cbp_luma=%d cbp_chroma=%d\n", idx, cbp_luma, cbp_chroma);

            /* intra_chroma_pred_mode */
            uint32_t chroma_pred = br_read_ue(br);
            bitwriter_write_ue(bw, chroma_pred);
            fprintf(stderr, "    chroma_pred=%u at %zu.%d\n", chroma_pred, br->byte_pos, br->bit_pos);

            /* mb_qp_delta (always present for I_16x16) */
            int32_t qp_delta = br_read_se(br);
            bitwriter_write_se(bw, qp_delta);
            fprintf(stderr, "    qp_delta=%d at %zu.%d\n", qp_delta, br->byte_pos, br->bit_pos);

            if (copy_i16x16_residual(br, bw, cbp_luma, cbp_chroma, row, col, &cur_row_ctx[col]) < 0) {
                fprintf(stderr, "Failed to copy I_16x16 residual in P-slice at MB[%d,%d]\n", row, col);
                free(cur_row_ctx);
                return -1;
            }
            fprintf(stderr, "    I_16x16 residual done at %zu.%d\n", br->byte_pos, br->bit_pos);

            memcpy(&prev_mb_ctx, &cur_row_ctx[col], sizeof(MBCoeffContext));

        } else if (mb_type == 30) {
            /* I_PCM in P-slice */
            while (br->bit_pos != 0) br_read_bit(br);
            bitwriter_align(bw);

            for (int i = 0; i < 384; i++) {
                bitwriter_write_bits(bw, br_read_bits(br, 8), 8);
            }

            memset(&cur_row_ctx[col], 0, sizeof(MBCoeffContext));
            memset(&prev_mb_ctx, 0, sizeof(prev_mb_ctx));
        } else {
            fprintf(stderr, "Unknown P mb_type %d at MB[%d,%d]\n", mb_type, row, col);
            free(cur_row_ctx);
            return -1;
        }

        col++;
        input_mb_count++;

        /* End of input row? Add padding to pending skips (don't write yet!) */
        if (col == INPUT_MB_WIDTH) {
            /* Add padding skips to accumulator */
            pending_output_skips += PADDING_MBS_PER_ROW;

            /* Zero context for padding MBs */
            memset(&prev_mb_ctx, 0, sizeof(prev_mb_ctx));

            /* Save row context for next row's top neighbor */
            memcpy(top_mb_ctx, cur_row_ctx, INPUT_MB_WIDTH * sizeof(MBCoeffContext));

            printf("  Row %d: coded MB, pending=%d\n", row, pending_output_skips);

            row++;
            col = 0;
        }
    }

    /* Write any remaining pending skips at end of slice */
    if (pending_output_skips > 0) {
        bitwriter_write_ue(bw, pending_output_skips);
        printf("  Final skip_run: %d\n", pending_output_skips);
    }

    free(cur_row_ctx);
    return 0;
}

/*
 * Main function
 */
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.h264> <output.h264>\n", argv[0]);
        fprintf(stderr, "\nResizes %dx%d to %dx%d by adding horizontal padding\n",
                INPUT_WIDTH, INPUT_HEIGHT, OUTPUT_WIDTH, OUTPUT_HEIGHT);
        return 1;
    }

    const char *input_file = argv[1];
    const char *output_file = argv[2];

    /* Read input file */
    FILE *f = fopen(input_file, "rb");
    if (!f) {
        perror("Failed to open input file");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    size_t input_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *input_data = malloc(input_size);
    if (fread(input_data, 1, input_size, f) != input_size) {
        perror("Failed to read input file");
        fclose(f);
        return 1;
    }
    fclose(f);

    printf("Input: %s (%zu bytes)\n", input_file, input_size);

    /* First pass: scan for PPS to extract num_ref_idx_l0_active_minus1 */
    {
        size_t scan_pos = 0;
        while (scan_pos < input_size - 3) {
            if (input_data[scan_pos] == 0 && input_data[scan_pos+1] == 0) {
                int sc_len = 0;
                if (input_data[scan_pos+2] == 1) sc_len = 3;
                else if (scan_pos < input_size - 4 && input_data[scan_pos+2] == 0 && input_data[scan_pos+3] == 1) sc_len = 4;

                if (sc_len > 0) {
                    size_t nal_start = scan_pos + sc_len;
                    int nal_type = input_data[nal_start] & 0x1F;

                    if (nal_type == 8) {
                        /* Found PPS - extract num_ref_idx_l0_active_minus1 */
                        size_t nal_end = nal_start + 1;
                        while (nal_end < input_size - 3) {
                            if (input_data[nal_end] == 0 && input_data[nal_end+1] == 0 &&
                                (input_data[nal_end+2] == 0 || input_data[nal_end+2] == 1)) break;
                            nal_end++;
                        }
                        if (nal_end >= input_size - 3) nal_end = input_size;

                        size_t nal_size = nal_end - nal_start;
                        uint8_t *pps_rbsp = malloc(nal_size);
                        size_t pps_rbsp_size = ebsp_to_rbsp(pps_rbsp, input_data + nal_start + 1, nal_size - 1);
                        BitReader pps_br;
                        br_init(&pps_br, pps_rbsp, pps_rbsp_size);

                        br_read_ue(&pps_br);  /* pps_id */
                        br_read_ue(&pps_br);  /* sps_id */
                        br_read_bit(&pps_br); /* entropy_coding_mode_flag */
                        br_read_bit(&pps_br); /* bottom_field_pic_order_in_frame_present_flag */
                        uint32_t num_slice_groups_minus1 = br_read_ue(&pps_br);

                        if (num_slice_groups_minus1 == 0) {
                            pps_num_ref_idx_l0_active_minus1 = br_read_ue(&pps_br);
                            pps_num_ref_idx_l0_default = pps_num_ref_idx_l0_active_minus1 + 1;
                            /* Continue parsing to get pic_init_qp_minus26 and chroma_qp_index_offset */
                            br_read_ue(&pps_br);  /* num_ref_idx_l1_active_minus1 */
                            br_read_bit(&pps_br); /* weighted_pred_flag */
                            br_read_bits(&pps_br, 2); /* weighted_bipred_idc */
                            pps_pic_init_qp_minus26 = br_read_se(&pps_br);
                            br_read_se(&pps_br);  /* pic_init_qs_minus26 */
                            pps_chroma_qp_index_offset = br_read_se(&pps_br);
                            printf("Pre-scan PPS: num_ref_idx_l0=%d, pic_init_qp_minus26=%d, chroma_qp_index_offset=%d\n",
                                   pps_num_ref_idx_l0_active_minus1, pps_pic_init_qp_minus26, pps_chroma_qp_index_offset);
                        }

                        free(pps_rbsp);
                        break;  /* Found PPS, stop scanning */
                    }
                    scan_pos = nal_start;
                }
            }
            scan_pos++;
        }
    }

    /* Allocate output buffer (much larger for I_PCM padding)
     * Each I_PCM MB = 384 bytes. 25 padding MBs/row * 20 rows = 500 per frame.
     * Plus original data + NAL escaping overhead. Allow for many frames. */
    size_t output_max = input_size * 50 + OUTPUT_MB_WIDTH * OUTPUT_MB_HEIGHT * 500;
    uint8_t *output_data = malloc(output_max);
    size_t output_pos = 0;

    /* Generate output SPS/PPS with parameters from input PPS */
    output_pos += generate_output_sps(output_data + output_pos, 4, pps_num_ref_idx_l0_default);
    output_pos += generate_output_pps(output_data + output_pos, pps_num_ref_idx_l0_active_minus1, pps_pic_init_qp_minus26, pps_chroma_qp_index_offset);

    printf("Generated SPS/PPS for %dx%d output (max_ref=%d)\n",
           OUTPUT_WIDTH, OUTPUT_HEIGHT, pps_num_ref_idx_l0_default);

    /* Find and process NAL units */
    size_t pos = 0;
    while (pos < input_size - 3) {
        /* Find start code (3-byte or 4-byte) */
        int start_code_len = 0;
        if (pos < input_size - 3 && input_data[pos] == 0 && input_data[pos+1] == 0) {
            if (input_data[pos+2] == 1) {
                start_code_len = 3;
            } else if (pos < input_size - 4 && input_data[pos+2] == 0 && input_data[pos+3] == 1) {
                start_code_len = 4;
            }
        }

        if (start_code_len > 0) {
            size_t nal_start = pos + start_code_len;
            uint8_t nal_header = input_data[nal_start];
            int nal_ref_idc = (nal_header >> 5) & 0x3;
            int nal_type = nal_header & 0x1F;

            /* Find end of NAL */
            size_t nal_end = nal_start + 1;
            while (nal_end < input_size - 3) {
                if (input_data[nal_end] == 0 && input_data[nal_end+1] == 0 &&
                    (input_data[nal_end+2] == 0 || input_data[nal_end+2] == 1)) {
                    break;
                }
                nal_end++;
            }
            if (nal_end >= input_size - 3) nal_end = input_size;

            size_t nal_size = nal_end - nal_start;
            printf("NAL type %d at %zu, size %zu\n", nal_type, pos, nal_size);

            if (nal_type == 7) {
                /* Skip SPS - we generate our own */
                pos = nal_end;
                continue;
            }

            if (nal_type == 8) {
                /* Parse PPS to extract num_ref_idx_l0_active_minus1 */
                uint8_t *pps_rbsp = malloc(nal_size);
                size_t pps_rbsp_size = ebsp_to_rbsp(pps_rbsp, input_data + nal_start + 1, nal_size - 1);
                BitReader pps_br;
                br_init(&pps_br, pps_rbsp, pps_rbsp_size);

                /* PPS fields:
                 * - pps_id (ue)
                 * - sps_id (ue)
                 * - entropy_coding_mode_flag (u1)
                 * - bottom_field_pic_order_in_frame_present_flag (u1)
                 * - num_slice_groups_minus1 (ue)
                 * - num_ref_idx_l0_active_minus1 (ue)
                 */
                br_read_ue(&pps_br);  /* pps_id */
                br_read_ue(&pps_br);  /* sps_id */
                br_read_bit(&pps_br); /* entropy_coding_mode_flag */
                br_read_bit(&pps_br); /* bottom_field_pic_order_in_frame_present_flag */
                uint32_t num_slice_groups_minus1 = br_read_ue(&pps_br);

                if (num_slice_groups_minus1 == 0) {
                    /* No slice groups, directly read num_ref_idx_l0_active_minus1 */
                    pps_num_ref_idx_l0_active_minus1 = br_read_ue(&pps_br);
                    pps_num_ref_idx_l0_default = pps_num_ref_idx_l0_active_minus1 + 1;
                    /* Continue parsing to get pic_init_qp_minus26 and chroma_qp_index_offset */
                    br_read_ue(&pps_br);  /* num_ref_idx_l1_active_minus1 */
                    br_read_bit(&pps_br); /* weighted_pred_flag */
                    br_read_bits(&pps_br, 2); /* weighted_bipred_idc */
                    pps_pic_init_qp_minus26 = br_read_se(&pps_br);
                    br_read_se(&pps_br);  /* pic_init_qs_minus26 */
                    pps_chroma_qp_index_offset = br_read_se(&pps_br);
                    printf("  PPS: num_ref_idx_l0_active_minus1=%d (default=%d), pic_init_qp_minus26=%d, chroma_qp_index_offset=%d\n",
                           pps_num_ref_idx_l0_active_minus1, pps_num_ref_idx_l0_default, pps_pic_init_qp_minus26, pps_chroma_qp_index_offset);
                } else {
                    /* Slice groups present - would need more complex parsing */
                    printf("  PPS: slice groups present, using default num_ref_idx_l0=1\n");
                }

                free(pps_rbsp);
                pos = nal_end;
                continue;
            }

            if (nal_type == 5 || nal_type == 1) {
                /* Slice NAL - process it */
                int is_idr = (nal_type == 5);

                /* Convert EBSP to RBSP */
                uint8_t *rbsp = malloc(nal_size);
                size_t rbsp_size = ebsp_to_rbsp(rbsp, input_data + nal_start + 1, nal_size - 1);

                BitReader br;
                br_init(&br, rbsp, rbsp_size);

                /* Parse slice header */
                uint32_t first_mb = br_read_ue(&br);
                uint32_t slice_type_raw = br_read_ue(&br);
                int slice_type = slice_type_raw % 5;
                int is_i_slice = (slice_type == 2 || slice_type == 7);
                int is_p_slice = (slice_type == 0 || slice_type == 5);

                uint32_t pps_id = br_read_ue(&br);
                uint32_t frame_num = br_read_bits(&br, 4);  /* Assuming log2_max=4 */

                uint32_t idr_pic_id = 0;
                if (is_idr) {
                    idr_pic_id = br_read_ue(&br);
                }

                /* POC type 2 - no POC fields */

                /* For P-slices: num_ref_idx_active_override_flag */
                int num_ref_idx_l0 = pps_num_ref_idx_l0_default;  /* From PPS parsing */
                int num_ref_idx_override = 0;
                if (is_p_slice) {
                    num_ref_idx_override = br_read_bit(&br);
                    if (num_ref_idx_override) {
                        num_ref_idx_l0 = br_read_ue(&br) + 1;
                    }
                }

                /* ref_pic_list_modification for P-slices */
                if (is_p_slice) {
                    int ref_pic_list_modification_flag_l0 = br_read_bit(&br);
                    if (ref_pic_list_modification_flag_l0) {
                        /* Parse modification operations until modification_of_pic_nums_idc == 3 */
                        uint32_t mod_idc;
                        do {
                            mod_idc = br_read_ue(&br);
                            if (mod_idc == 0 || mod_idc == 1) {
                                br_read_ue(&br);  /* abs_diff_pic_num_minus1 */
                            } else if (mod_idc == 2) {
                                br_read_ue(&br);  /* long_term_pic_num */
                            }
                        } while (mod_idc != 3);
                    }
                }

                /* dec_ref_pic_marking */
                if (is_idr) {
                    br_read_bit(&br);  /* no_output_of_prior_pics_flag */
                    br_read_bit(&br);  /* long_term_reference_flag */
                } else if (nal_ref_idc != 0) {
                    /* Non-IDR reference picture */
                    int adaptive_ref_pic_marking_mode_flag = br_read_bit(&br);
                    if (adaptive_ref_pic_marking_mode_flag) {
                        uint32_t op;
                        do {
                            op = br_read_ue(&br);
                            if (op == 1 || op == 3) {
                                br_read_ue(&br);  /* difference_of_pic_nums_minus1 */
                            }
                            if (op == 2) {
                                br_read_ue(&br);  /* long_term_pic_num */
                            }
                            if (op == 3 || op == 6) {
                                br_read_ue(&br);  /* long_term_frame_idx */
                            }
                            if (op == 4) {
                                br_read_ue(&br);  /* max_long_term_frame_idx_plus1 */
                            }
                        } while (op != 0);
                    }
                }

                /* slice_qp_delta */
                int32_t qp_delta = br_read_se(&br);

                /* deblocking_filter_control_present_flag was set in PPS */
                uint32_t disable_deblock = br_read_ue(&br);
                int32_t alpha_offset = 0, beta_offset = 0;
                if (disable_deblock != 1) {
                    alpha_offset = br_read_se(&br);
                    beta_offset = br_read_se(&br);
                }

                printf("  Slice: first_mb=%d type=%d(%s) frame_num=%d qp_delta=%d num_ref=%d\n",
                       first_mb, slice_type, is_i_slice ? "I" : (is_p_slice ? "P" : "B"),
                       frame_num, qp_delta, num_ref_idx_l0);

                /* Write output slice */
                uint8_t *slice_rbsp = malloc(output_max);
                BitWriter bw;
                bitwriter_init(&bw, slice_rbsp, output_max);

                /* Write slice header */
                bitwriter_write_ue(&bw, first_mb);
                bitwriter_write_ue(&bw, slice_type_raw);
                bitwriter_write_ue(&bw, pps_id);
                bitwriter_write_bits(&bw, frame_num, 4);

                if (is_idr) {
                    bitwriter_write_ue(&bw, idr_pic_id);
                }

                /* Write num_ref_idx_active_override for P-slices */
                if (is_p_slice) {
                    bitwriter_write_bit(&bw, num_ref_idx_override);
                    if (num_ref_idx_override) {
                        bitwriter_write_ue(&bw, num_ref_idx_l0 - 1);
                    }
                }

                /* Write ref_pic_list_modification for P-slices (no modification) */
                if (is_p_slice) {
                    bitwriter_write_bit(&bw, 0);  /* ref_pic_list_modification_flag_l0 = 0 */
                }

                /* Write dec_ref_pic_marking */
                if (is_idr) {
                    bitwriter_write_bit(&bw, 0);  /* no_output */
                    bitwriter_write_bit(&bw, 0);  /* long_term */
                } else if (nal_ref_idc != 0) {
                    bitwriter_write_bit(&bw, 0);  /* adaptive_ref_pic_marking = 0 (sliding window) */
                }

                bitwriter_write_se(&bw, qp_delta);

                bitwriter_write_ue(&bw, disable_deblock);
                if (disable_deblock != 1) {
                    bitwriter_write_se(&bw, alpha_offset);
                    bitwriter_write_se(&bw, beta_offset);
                }

                /* Process MB data */
                fprintf(stderr, "  Slice header ends at byte %zu bit %d\n", br.byte_pos, br.bit_pos);

                int result;
                if (is_i_slice) {
                    result = process_i_slice(&br, &bw);
                } else if (is_p_slice) {
                    result = process_p_slice(&br, &bw, num_ref_idx_l0);
                } else {
                    fprintf(stderr, "Unsupported slice type %d\n", slice_type);
                    result = -1;
                }

                if (result < 0) {
                    fprintf(stderr, "Failed to process slice\n");
                    free(rbsp);
                    free(slice_rbsp);
                    free(input_data);
                    free(output_data);
                    return 1;
                }

                /* RBSP trailing bits */
                bitwriter_write_bit(&bw, 1);
                bitwriter_align(&bw);

                size_t slice_size = bitwriter_get_size(&bw);
                output_pos += write_nal_unit(output_data + output_pos, slice_rbsp, slice_size,
                                            nal_ref_idc, nal_type);

                printf("  Output slice: %zu bytes\n", slice_size);

                free(rbsp);
                free(slice_rbsp);
            }

            pos = nal_end;
        } else {
            pos++;
        }
    }

    /* Write output file */
    f = fopen(output_file, "wb");
    if (!f) {
        perror("Failed to open output file");
        free(input_data);
        free(output_data);
        return 1;
    }

    fwrite(output_data, 1, output_pos, f);
    fclose(f);

    printf("Output: %s (%zu bytes)\n", output_file, output_pos);

    free(input_data);
    free(output_data);

    /* Verify output */
    printf("\nVerifying output...\n");
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error -i '%s' -f null - 2>&1", output_file);
    int verify_result = system(cmd);

    if (verify_result == 0) {
        printf("Verification: SUCCESS\n");
    } else {
        printf("Verification: FAILED\n");
    }

    return verify_result != 0;
}
