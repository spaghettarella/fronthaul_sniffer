#pragma once

// Transform stages of the radar core: the delay IFFT that turns each symbol's
// CFR into a CIR, the cancellation steps, the slow-time Doppler FFT, and the
// magnitude / dB conversion that produces the published frame.
//
// Split out of c_radar.c, which keeps the worker, the config and the ring
// pairing. Plain C on purpose: these are the expensive loops, and they have to
// stay where they can be tuned, or moved to another FFT backend, without
// touching the C++ layers.
//
// Threading: a workspace belongs to the radar worker and is only ever used
// from that one thread. FFTW's planner is not thread-safe, so every plan is
// made here, on that thread; the transforms themselves run on fft_threads.

#include "c_radar.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fh_radar_dsp fh_radar_dsp_t;

// fft_threads is what every plan is made with; the caller has already run
// fftwf_init_threads.
fh_radar_dsp_t* fh_radar_dsp_create(int fft_threads);
void fh_radar_dsp_destroy(fh_radar_dsp_t* dsp);

// Sizes the delay-domain matrix for a pass - rows x ifft_size, row-major - and
// returns it for the CFR stage to fill. The matrix is not cleared here: the
// CFR stage writes every cell, the windowed H of the used REs packed from bin
// 0 and zeros in every other bin, and an all-zero row for each symbol that did
// not pair. out_row_valid receives one flag per row for the CFR stage to set:
// nonzero when that symbol paired. NULL when out of memory.
fh_complex_t* fh_radar_dsp_begin(fh_radar_dsp_t* dsp, int rows, int ifft_size, unsigned char** out_row_valid);

// Range window weights over len used REs, or NULL for rectangular - which is
// also what an allocation failure degrades to.
const float* fh_radar_dsp_range_window(fh_radar_dsp_t* dsp, fh_radar_window_t type, int len);

// What one pass hands the transform stages besides the matrix itself.
typedef struct fh_radar_dsp_pass {
    int used_re;        // REs the CFR stage packed, which the window gain is taken over
    int paired_symbols; // 0 skips the transforms; a rolling mean is still drawn
    int first_symbol;   // Symbol number of the matrix's row 0
    uint64_t epoch;     // fh_radar_stats::epoch of the pass; a new one restarts the mean
    int out_rows;       // Rows of dst: the window, or persist_period for a Range-Time mean
    int persist_folds;  // Out: see fh_radar_stats::persist_folds
} fh_radar_dsp_pass_t;

// Runs the transform stages for cfg->output over the matrix
// fh_radar_dsp_begin returned, which it consumes, and writes the frame into
// dst: out_rows x ifft_size for Range-Time, out_rows x cir_depth_taps for
// Range-Doppler. With the rolling mean on, the frame is folded into it and
// the mean is what lands in dst, with -1 in cells nothing has reached yet.
// Returns 0, or -1 when out of memory, in which case dst is zeroed.
int fh_radar_dsp_run(fh_radar_dsp_t* dsp, const fh_radar_config_t* cfg, fh_radar_dsp_pass_t* pass, float* dst);

// Drops everything the rolling mean holds; the next pass starts it afresh.
void fh_radar_dsp_reset_persistence(fh_radar_dsp_t* dsp);

#ifdef __cplusplus
}
#endif
