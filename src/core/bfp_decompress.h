#ifndef BFP_DECOMPRESS_H
#define BFP_DECOMPRESS_H

#include <stdint.h>

int bfp_decompress_prb(
    const char *hex_prb,
    int original_bit_width,
    int compressed_bit_width,
    uint8_t exponent,
    float *i_samples,
    float *q_samples);

int get_compressed_bit_width_from_param(uint8_t ud_comp_param);

#endif
