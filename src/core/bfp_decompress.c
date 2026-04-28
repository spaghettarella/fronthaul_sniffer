// ============================================================================
// BFP DECOMPRESS - DECOMPRESSION OF COMPRESSED IQ SAMPLES FROM PRBs
// ============================================================================

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

static int64_t extract_signed_bits(const uint8_t *buffer, uint32_t bit_offset, int bit_width) {
    int64_t value = 0;

    // Extract bits from buffer, MSB first
    for (int i = 0; i < bit_width; i++) {
        uint32_t byte_idx = (bit_offset + i) / 8;
        uint32_t bit_idx = (bit_offset + i) % 8;

        // Extract bit (bit 7 is MSB, bit 0 is LSB in each byte)
        uint8_t bit = (buffer[byte_idx] >> (7 - bit_idx)) & 1;
        value = (value << 1) | bit;
    }

    // Handle 2's complement sign extension for negative numbers
    if (bit_width < 64) {
        int64_t sign_bit = 1LL << (bit_width - 1);
        if (value & sign_bit) {
            // Negative number - sign extend all upper bits
            value |= (-sign_bit);
        }
    }

    return value;
}

int bfp_decompress_prb(const char *ptr_prb, int original_bit_width, int compressed_bit_width, uint8_t exponent, float *i_samples, float *q_samples) {
    // Validate inputs
    if (!ptr_prb || !i_samples || !q_samples) {
        return -1; // Invalid input pointers
    }

    if (compressed_bit_width < 1 || compressed_bit_width > 16) {
        return -2; // Bit width out of range
    }

    // Calculate required bits and bytes for this PRB
    // 12 IQ pairs * 2 samples/pair * compressed_bit_width bits/sample
    uint32_t bits_needed = 12 * 2 * compressed_bit_width;
    uint32_t bytes_needed = bits_needed / 8;

    // Collect the bytes we need for this PRB
    uint8_t *prb_bytes = (uint8_t *)malloc(bytes_needed);
    if (!prb_bytes) {
        return -4; // Memory allocation failure
    }

    for (uint32_t i = 0; i < bytes_needed; i++) {
        prb_bytes[i] = ptr_prb[i];
    }

    uint32_t bit_pos = 0;

    // Decompress 12 IQ pairs (24 mantissas total)
    for (int sample_idx = 0; sample_idx < 12; sample_idx++) {
        // Extract I-sample mantissa (2's complement)
        int64_t i_mantissa = extract_signed_bits(prb_bytes, bit_pos, compressed_bit_width);
        bit_pos += compressed_bit_width;

        // Extract Q-sample mantissa (2's complement)
        int64_t q_mantissa = extract_signed_bits(prb_bytes, bit_pos, compressed_bit_width);
        bit_pos += compressed_bit_width;

        int actual_exponent = (int)exponent - compressed_bit_width + 1;

        // Decompress: mantissa * 2^exponent = mantissa << exponent
        i_samples[sample_idx] = (float)(i_mantissa * pow(2.0, actual_exponent));
        q_samples[sample_idx] = (float)(q_mantissa * pow(2.0, actual_exponent));
    }

    free(prb_bytes);
    return 0;
}