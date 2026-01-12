#include "nal.h"
#include <string.h>
#include <assert.h>

void nal_writer_init(NALWriter *nw, uint8_t *output, size_t output_capacity,
                     uint8_t *rbsp_temp, size_t rbsp_capacity) {
    nw->output = output;
    nw->output_capacity = output_capacity;
    nw->output_pos = 0;
    nw->rbsp = rbsp_temp;
    nw->rbsp_capacity = rbsp_capacity;
}

/*
 * Convert RBSP to EBSP by inserting emulation prevention bytes.
 *
 * H.264 requires that the byte sequences 00 00 00, 00 00 01, 00 00 02,
 * and 00 00 03 never appear in the NAL unit payload. When these sequences
 * occur in the RBSP, we insert 0x03 after the second 0x00.
 *
 * Example: 00 00 00 -> 00 00 03 00
 *          00 00 01 -> 00 00 03 01
 */
size_t rbsp_to_ebsp(uint8_t *ebsp, size_t ebsp_capacity,
                    const uint8_t *rbsp, size_t rbsp_size) {
    size_t ebsp_pos = 0;
    int zero_count = 0;

    for (size_t i = 0; i < rbsp_size; i++) {
        uint8_t byte = rbsp[i];

        if (zero_count >= 2 && byte <= 0x03) {
            /* Need to insert emulation prevention byte */
            assert(ebsp_pos < ebsp_capacity);
            ebsp[ebsp_pos++] = 0x03;
            zero_count = 0;
        }

        assert(ebsp_pos < ebsp_capacity);
        ebsp[ebsp_pos++] = byte;

        if (byte == 0x00) {
            zero_count++;
        } else {
            zero_count = 0;
        }
    }

    return ebsp_pos;
}

size_t nal_write_unit(NALWriter *nw, int nal_ref_idc, int nal_type,
                      const uint8_t *rbsp, size_t rbsp_size,
                      int use_long_startcode) {
    size_t start_pos = nw->output_pos;

    /* Write start code */
    if (use_long_startcode) {
        assert(nw->output_pos + 4 <= nw->output_capacity);
        nw->output[nw->output_pos++] = 0x00;
        nw->output[nw->output_pos++] = 0x00;
        nw->output[nw->output_pos++] = 0x00;
        nw->output[nw->output_pos++] = 0x01;
    } else {
        assert(nw->output_pos + 3 <= nw->output_capacity);
        nw->output[nw->output_pos++] = 0x00;
        nw->output[nw->output_pos++] = 0x00;
        nw->output[nw->output_pos++] = 0x01;
    }

    /* Write NAL header byte */
    /* forbidden_zero_bit (1) | nal_ref_idc (2) | nal_unit_type (5) */
    assert(nw->output_pos < nw->output_capacity);
    uint8_t nal_header = ((nal_ref_idc & 0x03) << 5) | (nal_type & 0x1F);
    nw->output[nw->output_pos++] = nal_header;

    /* Convert RBSP to EBSP and write */
    size_t ebsp_size = rbsp_to_ebsp(nw->output + nw->output_pos,
                                    nw->output_capacity - nw->output_pos,
                                    rbsp, rbsp_size);
    nw->output_pos += ebsp_size;

    return nw->output_pos - start_pos;
}

size_t nal_writer_get_size(NALWriter *nw) {
    return nw->output_pos;
}

uint8_t *nal_writer_get_output(NALWriter *nw) {
    return nw->output;
}
