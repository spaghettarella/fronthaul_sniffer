// ============================================================================
// BFP DECOMPRESS - DECOMPRESSION OF COMPRESSED IQ SAMPLES FROM PRBs
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <math.h>

static inline uint32_t read_unsigned_bits(const uint8_t* buffer,
                                          size_t* byte_index,
                                          uint64_t* accumulator,
                                          int* accumulator_bits,
                                          int bit_width) {
    while (*accumulator_bits < bit_width) {
        *accumulator = (*accumulator << 8) | (uint64_t)buffer[*byte_index];
        *byte_index += 1;
        *accumulator_bits += 8;
    }

    const int shift = *accumulator_bits - bit_width;
    const uint64_t mask = (1ULL << bit_width) - 1ULL;
    const uint32_t value = (uint32_t)((*accumulator >> shift) & mask);

    *accumulator_bits -= bit_width;
    if (*accumulator_bits == 0) {
        *accumulator = 0;
    } else {
        *accumulator &= (1ULL << *accumulator_bits) - 1ULL;
    }

    return value;
}

static inline int32_t sign_extend_u32(uint32_t value, int bit_width) {
    const uint32_t sign = 1U << (bit_width - 1);
    return (int32_t)((value ^ sign) - sign);
}

// O-RAN BFP restores a sample of the original fixed-point width as
// mantissa * 2^exponent. That is then divided by 2^(original_bit_width - 1),
// so a full-scale sample reads 1.0 - the same [-1, +1) scale the uncompressed
// path's 2^-15 gives 16-bit samples. The original width is what sets the full
// scale, not the mantissa width: normalising by the mantissa width instead
// made 9-bit BFP read 2^7 (42 dB) above the same signal sent uncompressed.
int bfp_decompress_prb(const char *ptr_prb, int original_bit_width, int compressed_bit_width, uint8_t exponent, float *i_samples, float *q_samples) {
    // Validate inputs
    if (!ptr_prb || !i_samples || !q_samples) {
        return -1; // Invalid input pointers
    }

    if (compressed_bit_width < 1 || compressed_bit_width > 16) {
        return -2; // Bit width out of range
    }
    if (original_bit_width < 1 || original_bit_width > 32) {
        return -3; // Original width out of range
    }

    const uint8_t* bytes = (const uint8_t*)ptr_prb;
    size_t byte_index = 0;
    uint64_t accumulator = 0;
    int accumulator_bits = 0;
    const int actual_exponent = (int)exponent - original_bit_width + 1;
    const float scale = ldexpf(1.0f, actual_exponent);

    // Decompress 12 IQ pairs (24 mantissas total)
    for (int sample_idx = 0; sample_idx < 12; sample_idx++) {
        const uint32_t i_raw = read_unsigned_bits(
            bytes,
            &byte_index,
            &accumulator,
            &accumulator_bits,
            compressed_bit_width);
        const uint32_t q_raw = read_unsigned_bits(
            bytes,
            &byte_index,
            &accumulator,
            &accumulator_bits,
            compressed_bit_width);

        const int32_t i_mantissa = sign_extend_u32(i_raw, compressed_bit_width);
        const int32_t q_mantissa = sign_extend_u32(q_raw, compressed_bit_width);

        // Decompress: mantissa * 2^exponent, normalised to full scale
        i_samples[sample_idx] = (float)i_mantissa * scale;
        q_samples[sample_idx] = (float)q_mantissa * scale;
    }
    return 0;
}