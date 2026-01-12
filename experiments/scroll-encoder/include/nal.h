#ifndef NAL_H
#define NAL_H

#include <stdint.h>
#include <stddef.h>

/*
 * NAL Unit Types (H.264 Table 7-1)
 */
#define NAL_TYPE_SLICE          1   /* Coded slice of a non-IDR picture */
#define NAL_TYPE_IDR            5   /* Coded slice of an IDR picture */
#define NAL_TYPE_SEI            6   /* Supplemental enhancement information */
#define NAL_TYPE_SPS            7   /* Sequence parameter set */
#define NAL_TYPE_PPS            8   /* Picture parameter set */
#define NAL_TYPE_AUD            9   /* Access unit delimiter */

/*
 * NAL Reference IDC values
 */
#define NAL_REF_IDC_NONE        0   /* Non-reference picture */
#define NAL_REF_IDC_LOW         1   /* Low importance reference */
#define NAL_REF_IDC_HIGH        2   /* High importance reference */
#define NAL_REF_IDC_HIGHEST     3   /* Highest importance (IDR, SPS, PPS) */

/*
 * NAL Writer context for building Annex-B NAL units
 */
typedef struct {
    uint8_t *output;        /* Output buffer for complete Annex-B stream */
    size_t output_capacity;
    size_t output_pos;

    uint8_t *rbsp;          /* Temporary buffer for RBSP */
    size_t rbsp_capacity;
} NALWriter;

/* Initialize NAL writer with output buffer and temp RBSP buffer */
void nal_writer_init(NALWriter *nw, uint8_t *output, size_t output_capacity,
                     uint8_t *rbsp_temp, size_t rbsp_capacity);

/*
 * Write a complete NAL unit to output in Annex-B format:
 * [start code][nal header][EBSP payload]
 *
 * Parameters:
 *   nw          - NAL writer context
 *   nal_ref_idc - Reference importance (0-3)
 *   nal_type    - NAL unit type (1-31)
 *   rbsp        - Raw byte sequence payload (before emulation prevention)
 *   rbsp_size   - Size of RBSP in bytes
 *   use_long_startcode - If true, use 4-byte start code (00 00 00 01)
 *                        If false, use 3-byte start code (00 00 01)
 *
 * Returns: Number of bytes written to output, or 0 on error
 */
size_t nal_write_unit(NALWriter *nw, int nal_ref_idc, int nal_type,
                      const uint8_t *rbsp, size_t rbsp_size,
                      int use_long_startcode);

/* Get current output position */
size_t nal_writer_get_size(NALWriter *nw);

/* Get pointer to output buffer */
uint8_t *nal_writer_get_output(NALWriter *nw);

/*
 * Convert RBSP to EBSP by inserting emulation prevention bytes (0x03)
 * wherever 00 00 00, 00 00 01, 00 00 02, or 00 00 03 would occur.
 *
 * Returns: Size of EBSP output
 */
size_t rbsp_to_ebsp(uint8_t *ebsp, size_t ebsp_capacity,
                    const uint8_t *rbsp, size_t rbsp_size);

#endif /* NAL_H */
