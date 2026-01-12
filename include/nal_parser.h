#ifndef NAL_PARSER_H
#define NAL_PARSER_H

#include <stdint.h>
#include <stddef.h>

/*
 * NAL Unit Parser for Annex-B streams
 *
 * Finds NAL units in an Annex-B byte stream and extracts RBSP.
 */

typedef struct {
    int nal_ref_idc;
    int nal_unit_type;
    const uint8_t *data;    /* Pointer to NAL unit data (after header) */
    size_t size;            /* Size of NAL unit data */
    size_t rbsp_size;       /* Size after removing emulation prevention bytes */
} NALUnit;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} NALParser;

/* Initialize parser with Annex-B data */
void nal_parser_init(NALParser *parser, const uint8_t *data, size_t size);

/* Find next NAL unit. Returns 1 if found, 0 if end of stream */
int nal_parser_next(NALParser *parser, NALUnit *unit);

/* Convert EBSP to RBSP (remove emulation prevention bytes) */
size_t ebsp_to_rbsp(uint8_t *rbsp, const uint8_t *ebsp, size_t ebsp_size);

/*
 * Parse SPS to extract key values
 *
 * Returns 0 on success, -1 on error
 */
int parse_sps(const uint8_t *rbsp, size_t size,
              int *width, int *height,
              int *log2_max_frame_num,
              int *pic_order_cnt_type,
              int *log2_max_pic_order_cnt_lsb);

/*
 * Parse PPS to extract key values
 *
 * Returns 0 on success, -1 on error
 */
int parse_pps(const uint8_t *rbsp, size_t size,
              int *num_ref_idx_l0_default_minus1,
              int *deblocking_filter_control_present_flag);

#endif /* NAL_PARSER_H */
