#pragma once

// OFDM / ISAC radar core.
//
// Pairs the downlink and uplink symbol rings of one eAxC and divides them to
// recover a Channel Frequency Response per OFDM symbol. Downstream stages
// (delay IFFT, CIR truncation, slow-time Doppler FFT) build on that CFR.
//
// Deliberately C, not C++: the whole path from ring rows to transform output
// is plain contiguous memory, and fh_complex_t is already 'float complex' here,
// which is layout-compatible with fftwf_complex. C++ stops at this header.
//
// Threading: the radar owns a worker thread. The render thread only ever calls
// the config/stats/submit entry points and borrows finished frames through
// fh_radar_acquire_result / fh_radar_release_result. Every function here is
// safe to call from the render thread.

#include "c_oran_memory.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of resource elements in a symbol row. Matches the ring's fixed
// 273 PRB * 12 RE layout; see fh_oran_memory_create.
#define FH_RADAR_RE_COUNT 3276

typedef enum fh_radar_output {
    RADAR_OUT_CFR = 0,           // |H| over (symbol x RE). No FFT; the bring-up view.
    RADAR_OUT_RANGE_TIME = 1,    // |CIR| over (symbol x delay tap)
    RADAR_OUT_RANGE_DOPPLER = 2  // |RD| over (Doppler bin x delay tap)
} fh_radar_output_t;

typedef enum fh_radar_division {
    // Y = H*X with the uplink as the received echo and the downlink as the
    // transmitted reference, so UL/DL is the physical channel. DL/UL yields
    // 1/H, which mirrors the delay axis. Both read |.| ~ 1 on occupied REs,
    // so the sanity check alone cannot tell them apart.
    RADAR_DIV_UL_OVER_DL = 0,
    RADAR_DIV_DL_OVER_UL = 1
} fh_radar_division_t;

typedef struct fh_radar_config {
    int time_symbols;      // Coherent processing interval, in symbols
    int freq_start_re;     // First resource element used, inclusive
    int freq_end_re;       // Last resource element used, inclusive
    int dl_ul_lag_symbols; // UL symbol paired with DL symbol n is n + lag. May be negative.
    fh_radar_division_t division;
    int ifft_size;         // Delay-domain transform size; power of two
    // CIR truncation feeding the slow-time FFT: Andreas's 4096 -> 128 step.
    // Sets the Range-Doppler delay extent and how many slow-time FFTs run.
    // Range-Time plots the full ifft_size taps and ignores this.
    int cir_depth_taps;
    int mti_enabled;        // Subtract each tap's slow-time mean (static clutter removal)
    int blank_taps_enabled; // Zero the leading taps outright
    int blank_taps;         // How many leading taps blank_taps_enabled zeroes
    fh_radar_output_t output;
} fh_radar_config_t;

// Fills cfg with the defaults every field starts at.
void fh_radar_config_defaults(fh_radar_config_t* cfg);

typedef struct fh_radar_stats {
    int paired_symbols;      // Symbols in the window with usable DL and UL rows
    int window_symbols;      // Symbols the window spans
    int window_first_symbol; // Absolute symbol number of result row 0
    int window_first_valid;  // Whether window_first_symbol means anything yet
    int skipped_dl_empty;    // DL row absent, or carrying no occupied RE
    int skipped_ul_missing;  // UL row absent from the ring
    // Mean |H| over resource elements that were occupied in both directions.
    // Reads near 1.0 when the pairing is correct; this is the acceptance test.
    float mean_cfr_magnitude;
    double last_refresh_ms;  // Wall time the most recent full pass took
    uint64_t refresh_count;
    int have_dl;             // Whether a downlink ring was submitted
    int have_ul;
} fh_radar_stats_t;

typedef struct fh_radar fh_radar_t;

// fft_threads is clamped to at least 1 and fixed for the radar's lifetime,
// mirroring how the decode worker count is fixed at startup.
fh_radar_t* fh_radar_create(size_t fft_threads);
void fh_radar_destroy(fh_radar_t* r);

size_t fh_radar_get_fft_threads(const fh_radar_t* r);

void fh_radar_get_config(const fh_radar_t* r, fh_radar_config_t* out);
// Clamps every field, so callers may pass raw widget values straight through.
// Reallocates buffers and rebuilds transform plans only when a field that
// changes their shape actually moved.
void fh_radar_set_config(fh_radar_t* r, const fh_radar_config_t* cfg);

void fh_radar_get_stats(const fh_radar_t* r, fh_radar_stats_t* out);

// Drops accumulated results. Call when the selected stream changes, since the
// existing frame no longer describes what is on screen.
void fh_radar_reset(fh_radar_t* r);

// Publishes the rings the worker should read. Either may be NULL, which the
// worker reports through fh_radar_stats::have_dl / have_ul. Cheap enough to
// call every frame; it only stores the pointers.
void fh_radar_submit(fh_radar_t* r, fh_oran_memory_t* dl, fh_oran_memory_t* ul);

// Borrows the most recent finished magnitude matrix, row-major, rows*cols
// floats. Returns NULL when nothing has been computed yet. Every successful
// call must be matched by fh_radar_release_result; the worker cannot publish
// a new frame while a borrow is outstanding.
const float* fh_radar_acquire_result(fh_radar_t* r, int* out_rows, int* out_cols);
void fh_radar_release_result(fh_radar_t* r);

#ifdef __cplusplus
}
#endif
