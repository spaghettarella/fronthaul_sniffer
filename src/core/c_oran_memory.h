#pragma once

#include "c_fronthaul_decoder.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <complex>
typedef std::complex<float> fh_complex_t;
#else
#include <complex.h>
typedef float complex fh_complex_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_oran_memory {
    fh_complex_t** symbols;  // 2D array: symbols[i][j] where i is symbol, j is IQ sample (0-3275)
    int* row_symbol_numbers; // Absolute symbol number stored in each ring row (INT_MIN means empty)
    int symbol_lowest;
    int symbol_highest;
    int index_symbol_highest;
    size_t max_symbols;
    uint64_t frame_counter;  // Continuous frame counter (tracks wraparounds of 8-bit frame_id)
    uint8_t last_frame_id;   // Previous frame_id; used to detect wraparound
    int64_t replay_symbol_offset;  // Per-stream offset added on each pcap replay loop
    int loop_first_symbol;         // First raw symbol number seen in the current replay loop
    int loop_last_symbol;          // Last raw symbol number seen in the current replay loop
} fh_oran_memory_t;

typedef struct fh_oran_memory_stats {
    int symbol_lowest;
    int symbol_highest;
    size_t max_symbols;
} fh_oran_memory_stats_t;

fh_oran_memory_t* fh_oran_memory_create(size_t max_symbols);
void fh_oran_memory_destroy(fh_oran_memory_t* mem);

int fh_oran_memory_save(fh_oran_memory_t* mem, const fh_decoded_packet_t* pkt, int scs_khz);
int fh_oran_memory_wipe(fh_oran_memory_t* mem);
void fh_oran_memory_advance_loop(fh_oran_memory_t* mem);

fh_oran_memory_stats_t fh_oran_memory_stats(const fh_oran_memory_t* mem);

#ifdef __cplusplus
}
#endif
