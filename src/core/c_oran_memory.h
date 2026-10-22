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
    uint8_t last_frame_id;   // Newest frame_id seen; late sections never move it back
    int frame_id_seen;       // Whether last_frame_id holds a real frame yet
    int scs_khz;             // Subcarrier spacing this stream was last saved with. Recorded per
                             // stream because the PHY tap can override it from each record's
                             // numerology; consumers need it to map symbol numbers back to
                             // frame/subframe/slot positions. 0 until the first save.
    int64_t replay_symbol_offset;  // Offset added on each pcap replay loop; the same for every stream
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
// Moves a stream on to the next pass of a looped replay: shifts its numbering by
// loop_span symbols and restarts its per-pass tracking. loop_span has to be the
// span of the whole pass across every stream, not this stream's own, and the
// same for all of them - streams cover different parts of a capture (DL and UL
// occupy different slots), so per-stream spans would move them apart by the
// difference on every pass, and DL/UL pairing along with them.
void fh_oran_memory_advance_loop(fh_oran_memory_t* mem, int64_t loop_span);

fh_oran_memory_stats_t fh_oran_memory_stats(const fh_oran_memory_t* mem);

// Symbols in one 256-frame cycle of the 8-bit frame_id at the stream's
// subcarrier spacing: how far apart two symbol numbers with the same frame_id,
// subframe, slot and symbol lie. 0 until the stream has been saved with a
// known spacing. Each stream counts these cycles on its own, so two streams
// only agree on symbol numbers modulo this; see resolve_frame_cycle.
int64_t fh_oran_memory_cycle_symbols(const fh_oran_memory_t* mem);

// Resolves the ring row holding an absolute symbol number, checking its tag so
// a row since reused for another symbol (ring wraparound, or a gap the stream
// never filled) is not mistaken for it. Returns 1 and sets *out_idx when the
// symbol is present, 0 otherwise.
int fh_oran_memory_row_for_symbol(const fh_oran_memory_t* mem, int symbol_num, size_t* out_idx);

#ifdef __cplusplus
}
#endif
