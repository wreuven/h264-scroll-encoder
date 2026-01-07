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

/*
 * BitReader implementation
 */

void bitreader_init(BitReader *br, const uint8_t *buffer, size_t size) {
    br->buffer = buffer;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 0;
}

int bitreader_read_bit(BitReader *br) {
    if (br->byte_pos >= br->size) {
        return 0;  /* EOF - return 0 */
    }

    int bit = (br->buffer[br->byte_pos] >> (7 - br->bit_pos)) & 1;
    br->bit_pos++;

    if (br->bit_pos == 8) {
        br->byte_pos++;
        br->bit_pos = 0;
    }

    return bit;
}

uint32_t bitreader_read_bits(BitReader *br, int n) {
    uint32_t value = 0;

    for (int i = 0; i < n; i++) {
        value = (value << 1) | bitreader_read_bit(br);
    }

    return value;
}

uint32_t bitreader_read_ue(BitReader *br) {
    /* Count leading zeros */
    int leading_zeros = 0;
    while (bitreader_read_bit(br) == 0 && leading_zeros < 32) {
        leading_zeros++;
    }

    if (leading_zeros == 0) {
        return 0;
    }

    /* Read the suffix bits */
    uint32_t suffix = bitreader_read_bits(br, leading_zeros);
    return (1 << leading_zeros) - 1 + suffix;
}

int32_t bitreader_read_se(BitReader *br) {
    uint32_t ue_val = bitreader_read_ue(br);

    /* Decode: odd values are positive, even are negative */
    if (ue_val & 1) {
        return (int32_t)((ue_val + 1) / 2);
    } else {
        return -(int32_t)(ue_val / 2);
    }
}

size_t bitreader_get_bit_position(BitReader *br) {
    return br->byte_pos * 8 + br->bit_pos;
}

int bitreader_is_byte_aligned(BitReader *br) {
    return br->bit_pos == 0;
}

size_t bitreader_get_remaining_bytes(BitReader *br) {
    if (br->bit_pos != 0) {
        /* Not byte-aligned, remaining bytes after current partial byte */
        return br->size - br->byte_pos - 1;
    }
    return br->size - br->byte_pos;
}

const uint8_t *bitreader_get_pointer(BitReader *br) {
    return br->buffer + br->byte_pos;
}
