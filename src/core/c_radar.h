#pragma once

// OFDM / ISAC radar core.
//
// Pairs the downlink and uplink symbol rings of one eAxC and divides them to
// recover a Channel Frequency Response per OFDM symbol. Downstream stages
// (delay IFFT, CIR truncation, slow-time Doppler FFT) build on that CFR; they
// live in c_radar_dsp.c.
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

// Bounds on the transform outputs' rolling mean; see fh_radar_config. The GUI
// clamps its shared Rolling mean fields to the same bounds, so the CFR mean in
// the visualizer and this one always run on the same numbers.
#define FH_RADAR_MAX_PERSIST_PERIOD 4096
#define FH_RADAR_MAX_PERSIST_N 100000

typedef enum fh_radar_output {
    RADAR_OUT_CFR = 0,           // |H| over (symbol x RE). No FFT; the bring-up view.
    RADAR_OUT_RANGE_TIME = 1,    // |CIR| over (symbol x delay tap)
    RADAR_OUT_RANGE_DOPPLER = 2  // |RD| over (Doppler bin x delay tap)
} fh_radar_output_t;

// Taper applied before a transform. Rectangular keeps the full resolution but
// leaves -13 dB sidelobes, which smear the direct path across every other tap
// or Doppler bin; Hann trades a wider main lobe for -31 dB.
typedef enum fh_radar_window {
    RADAR_WINDOW_RECT = 0,
    RADAR_WINDOW_HANN = 1
} fh_radar_window_t;

typedef enum fh_radar_division {
    // Y = H*X with the uplink as the received echo and the downlink as the
    // transmitted reference, so UL/DL is the physical channel. DL/UL yields
    // 1/H, which mirrors the delay axis. Both read |.| ~ 1 on occupied REs,
    // so the sanity check alone cannot tell them apart.
    RADAR_DIV_UL_OVER_DL = 0,
    RADAR_DIV_DL_OVER_UL = 1
} fh_radar_division_t;

typedef struct fh_radar_config {
    // Coherent processing interval, in symbols: the y-depth of every output.
    // Range-Time has one row per symbol, and Range-Doppler runs its slow-time
    // FFTs over this many points. 280 is one frame at 30 kHz.
    int time_symbols;
    // Which symbols a pass covers. Batch-aligned: the newest complete batch,
    // symbols [k * time_symbols, (k + 1) * time_symbols), computed once; a
    // radar that falls behind skips batches rather than showing a window that
    // starts somewhere new every frame. Symbol 0 is the start of frame 0, so
    // a time_symbols that divides a frame or a whole number of frames keeps
    // every batch on frame boundaries. Off: the newest time_symbols symbols,
    // recomputed every pass, which scrolls smoothly while the radar keeps up.
    int batch_aligned;
    int freq_start_re;     // First resource element used, inclusive
    int freq_end_re;       // Last resource element used, inclusive
    int dl_ul_lag_symbols; // UL symbol paired with DL symbol n is n + lag. May be negative.
    fh_radar_division_t division;
    // Delay-domain transform size; power of two. A span wider than this is
    // cropped to its first ifft_size REs for the transform outputs; see
    // fh_radar_used_re_range. The CFR view always shows the whole span.
    int ifft_size;
    // CIR truncation feeding the slow-time FFT: Andreas's 4096 -> 128 step, and
    // the x-depth of Range-Doppler. Keeps the nearest cir_depth_taps delay taps,
    // so it sets the map's range extent and how many slow-time FFTs run.
    // Range-Time plots the full ifft_size taps and ignores this.
    int cir_depth_taps;
    fh_radar_window_t range_window;   // Across the used REs, before the delay IFFT
    fh_radar_window_t doppler_window; // Across the window's symbols, before the Doppler FFT
    // Carrier centre frequency. Changes no cell: it only converts the Doppler
    // axis to radial velocity; see fh_radar_velocity_per_hz.
    double carrier_freq_hz;
    int mti_enabled;       // Subtract each tap's slow-time mean (static clutter removal)
    int blank_taps_enabled; // Zero the leading taps outright
    int blank_taps;         // How many leading taps blank_taps_enabled zeroes
    // Range-Time and Range-Doppler only. Cells are written as dB above a floor
    // db_range below the frame's peak, so they run 0 (floor and below) to
    // db_range (the peak). The CFR view always stays linear: |H| ~ 1 is its
    // acceptance test.
    int db_scale;
    float db_range;
    // Rolling mean of Range-Time and Range-Doppler; the CFR view has its own,
    // in the visualizer. An EMA with weight 1 / persist_n, taken over linear
    // power before the dB step, since dB values each measured against their
    // own frame's peak do not average into anything. Range-Time folds each
    // symbol once into row (symbol % persist_period), like the waterfall mean,
    // and the frame then has persist_period rows. Range-Doppler folds each
    // whole map once, cell by cell; persist_period does not apply to it.
    int persist_enabled;
    int persist_period;
    int persist_n;
    fh_radar_output_t output;
} fh_radar_config_t;

// Fills cfg with the defaults every field starts at.
void fh_radar_config_defaults(fh_radar_config_t* cfg);

// Batch-aligned windows: the smallest symbol ring that still holds a whole
// batch when the worker reaches it. The newest complete batch can trail the
// newest symbol by up to a batch plus the settle margin, so a ring of exactly
// twice the window already loses the first symbols of a late batch.
int fh_radar_min_ring_symbols(const fh_radar_config_t* cfg);

// The REs the transform outputs actually use: the configured span, cropped to
// its first ifft_size REs when it is wider than the transform. The worker and
// the settings window both go through this, so they cannot disagree.
void fh_radar_used_re_range(const fh_radar_config_t* cfg, int* out_start, int* out_end);

// The complex channel estimate the CFR view takes |.| of, for one downlink
// symbol: paired with uplink symbol dl_symbol + lag and divided the way cfg
// says, over [re_first, re_last] narrowed to cfg's frequency range.
// Unwindowed, like the CFR view. A resource element yields a value only where
// the denominator is nonzero; values are written packed, in RE order, into
// out, which must hold re_last - re_first + 1 of them. Returns how many were
// written, or -1 when the symbol does not pair: the DL or UL row missing from
// its ring, or a DL row with nothing in the range - the same symbols the
// radar's own pass skips. Reads the rings without a lock, like the worker.
// Callable from any thread; it touches no radar state.
int fh_radar_cfr_symbol(const fh_radar_config_t* cfg,
                        const fh_oran_memory_t* dl,
                        const fh_oran_memory_t* ul,
                        int dl_symbol,
                        int re_first,
                        int re_last,
                        fh_complex_t* out);

// Radial velocity per hertz of Doppler at the configured carrier, in m/s/Hz:
// v_rel = f_D * c0 / (2 f_c), the 2 because the echo covers the path twice.
// Same sign as the Doppler, so positive is approaching. 0 for a missing cfg.
// The Doppler axis and the settings window both scale through this.
double fh_radar_velocity_per_hz(const fh_radar_config_t* cfg);

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
    double cfr_ms;           // Of which pairing and dividing the rings
    double transform_ms;     // Of which the transform stages; 0 for the CFR view
    // REs the transform stages used, after cropping to the IFFT size.
    // Inclusive; both 0 for the CFR view.
    int used_re_start;
    int used_re_end;
    uint64_t refresh_count;
    int have_dl;             // Whether a downlink ring was submitted
    int have_ul;
    // The output the published frame was computed for. Trails the config for
    // up to one pass after the output is switched, since the worker finishes
    // the pass it is on first.
    fh_radar_output_t output;
    // Batch-aligned windows only. The batch the frame was computed from (its
    // first symbol is batch_index * window_symbols), and how many complete
    // batches were passed over since the last reset because a newer one was
    // already complete by the time the worker got to it.
    int64_t batch_index;
    uint64_t batches_skipped;
    // No batch has completed yet since the last reset; nothing is published
    // until one has.
    int batch_waiting;
    // Rolling mean of the transform outputs. persist_phase_rows: the frame is
    // a Range-Time mean, whose rows are phases 0 .. persist_period - 1 within
    // the averaging period rather than symbols. persist_folds: how much the
    // mean holds - periods spanned for Range-Time, maps folded for
    // Range-Doppler. Both 0 with the mean off.
    int persist_phase_rows;
    int persist_period;
    int persist_folds;
    // Moves whenever published frames stop being comparable with the ones
    // before: a new DL/UL pair, fh_radar_reset, or a config edit that changes
    // the values themselves (frequency range, lag, division). Anything
    // accumulated across frames, such as a rolling mean, starts over when it
    // moves.
    uint64_t epoch;
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

// Starts the transform outputs' rolling mean over, keeping every setting.
void fh_radar_reset_persistence(fh_radar_t* r);

// Whether the output is on screen. While inactive the worker computes nothing
// and keeps its last frame, so the transforms cost nothing outside the Radar
// view; turning it back on starts a pass straight away. Starts inactive.
void fh_radar_set_active(fh_radar_t* r, int active);

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
