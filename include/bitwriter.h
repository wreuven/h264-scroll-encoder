#ifndef BITWRITER_H
#define BITWRITER_H

#include <stdint.h>
#include <stddef.h>

/*
 * BitWriter - Fast bitstream writer for H.264 NAL unit generation
 *
 * Supports:
 * - Arbitrary bit-aligned writes
 * - Exp-Golomb coding (ue(v), se(v))
 * - RBSP trailing bits
 * - Automatic buffer management
 */

typedef struct {
    uint8_t *buffer;        /* Output buffer */
    size_t capacity;        /* Buffer capacity in bytes */
    size_t byte_pos;        /* Current byte position */
    int bit_pos;            /* Bits used in current byte (0-7, 0 = empty) */
    uint8_t current_byte;   /* Byte being assembled */
} BitWriter;

/* Initialize a bitwriter with given buffer */
void bitwriter_init(BitWriter *bw, uint8_t *buffer, size_t capacity);

/* Write n bits (1-32) from value (MSB first) */
void bitwriter_write_bits(BitWriter *bw, uint32_t value, int n);

/* Write a single bit */
void bitwriter_write_bit(BitWriter *bw, int bit);

/* Write unsigned Exp-Golomb coded value: ue(v) */
void bitwriter_write_ue(BitWriter *bw, uint32_t value);

/* Write signed Exp-Golomb coded value: se(v) */
void bitwriter_write_se(BitWriter *bw, int32_t value);

/* Write RBSP trailing bits (1 bit '1' followed by '0's to byte-align) */
void bitwriter_write_trailing_bits(BitWriter *bw);

/* Flush any partial byte to buffer */
void bitwriter_flush(BitWriter *bw);

/* Get current size in bytes (including partial byte if any) */
size_t bitwriter_get_size(BitWriter *bw);

/* Get current bit position within stream */
size_t bitwriter_get_bit_position(BitWriter *bw);

/* Check if writer is byte-aligned */
int bitwriter_is_byte_aligned(BitWriter *bw);

/*
 * BitReader - Bitstream reader for parsing H.264 slice headers
 */

typedef struct {
    const uint8_t *buffer;  /* Input buffer */
    size_t size;            /* Buffer size in bytes */
    size_t byte_pos;        /* Current byte position */
    int bit_pos;            /* Bits consumed in current byte (0-7) */
} BitReader;

/* Initialize a bitreader with given buffer */
void bitreader_init(BitReader *br, const uint8_t *buffer, size_t size);

/* Read n bits (1-32), MSB first */
uint32_t bitreader_read_bits(BitReader *br, int n);

/* Read a single bit */
int bitreader_read_bit(BitReader *br);

/* Read unsigned Exp-Golomb coded value: ue(v) */
uint32_t bitreader_read_ue(BitReader *br);

/* Read signed Exp-Golomb coded value: se(v) */
int32_t bitreader_read_se(BitReader *br);

/* Get current bit position in stream */
size_t bitreader_get_bit_position(BitReader *br);

/* Check if reader is byte-aligned */
int bitreader_is_byte_aligned(BitReader *br);

/* Get remaining bytes (for extracting MB data) */
size_t bitreader_get_remaining_bytes(BitReader *br);

/* Get pointer to current position (must be byte-aligned) */
const uint8_t *bitreader_get_pointer(BitReader *br);

#endif /* BITWRITER_H */
