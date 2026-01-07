#include "bitwriter.h"
#include <string.h>
#include <assert.h>

void bitwriter_init(BitWriter *bw, uint8_t *buffer, size_t capacity) {
    bw->buffer = buffer;
    bw->capacity = capacity;
    bw->byte_pos = 0;
    bw->bit_pos = 0;
    bw->current_byte = 0;
}

void bitwriter_write_bit(BitWriter *bw, int bit) {
    bw->current_byte = (bw->current_byte << 1) | (bit & 1);
    bw->bit_pos++;

    if (bw->bit_pos == 8) {
        assert(bw->byte_pos < bw->capacity);
        bw->buffer[bw->byte_pos++] = bw->current_byte;
        bw->current_byte = 0;
        bw->bit_pos = 0;
    }
}

void bitwriter_write_bits(BitWriter *bw, uint32_t value, int n) {
    assert(n >= 1 && n <= 32);

    /* Write bits from MSB to LSB */
    for (int i = n - 1; i >= 0; i--) {
        bitwriter_write_bit(bw, (value >> i) & 1);
    }
}

/*
 * Exp-Golomb encoding for unsigned integers (ue(v))
 *
 * value | codeword
 * ------|---------
 *   0   | 1
 *   1   | 010
 *   2   | 011
 *   3   | 00100
 *   4   | 00101
 *   ...
 *
 * Format: [M zeros][1][INFO bits]
 * where M = floor(log2(value + 1))
 * and INFO = value + 1 - 2^M (M bits)
 */
void bitwriter_write_ue(BitWriter *bw, uint32_t value) {
    /* Special case for 0 */
    if (value == 0) {
        bitwriter_write_bit(bw, 1);
        return;
    }

    /* Calculate number of leading zeros needed */
    uint32_t v = value + 1;
    int leading_zeros = 0;
    uint32_t temp = v;

    while (temp > 1) {
        temp >>= 1;
        leading_zeros++;
    }

    /* Write leading zeros */
    for (int i = 0; i < leading_zeros; i++) {
        bitwriter_write_bit(bw, 0);
    }

    /* Write the value + 1 in (leading_zeros + 1) bits */
    bitwriter_write_bits(bw, v, leading_zeros + 1);
}

/*
 * Exp-Golomb encoding for signed integers (se(v))
 *
 * value | mapped to ue
 * ------|--------------
 *   0   | 0
 *   1   | 1
 *  -1   | 2
 *   2   | 3
 *  -2   | 4
 *   ...
 *
 * Mapping: se(v) -> ue(2*|v| - (v > 0 ? 1 : 0))
 * Or equivalently: positive v maps to 2v-1, negative v maps to -2v
 */
void bitwriter_write_se(BitWriter *bw, int32_t value) {
    uint32_t mapped;

    if (value > 0) {
        mapped = 2 * value - 1;
    } else {
        mapped = -2 * value;
    }

    bitwriter_write_ue(bw, mapped);
}

void bitwriter_write_trailing_bits(BitWriter *bw) {
    /* Write rbsp_stop_one_bit (always 1) */
    bitwriter_write_bit(bw, 1);

    /* Write rbsp_alignment_zero_bits until byte-aligned */
    while (bw->bit_pos != 0) {
        bitwriter_write_bit(bw, 0);
    }
}

void bitwriter_flush(BitWriter *bw) {
    if (bw->bit_pos > 0) {
        /* Pad remaining bits with zeros and write */
        bw->current_byte <<= (8 - bw->bit_pos);
        assert(bw->byte_pos < bw->capacity);
        bw->buffer[bw->byte_pos++] = bw->current_byte;
        bw->current_byte = 0;
        bw->bit_pos = 0;
    }
}

size_t bitwriter_get_size(BitWriter *bw) {
    return bw->byte_pos + (bw->bit_pos > 0 ? 1 : 0);
}

size_t bitwriter_get_bit_position(BitWriter *bw) {
    return bw->byte_pos * 8 + bw->bit_pos;
}

int bitwriter_is_byte_aligned(BitWriter *bw) {
    return bw->bit_pos == 0;
}
