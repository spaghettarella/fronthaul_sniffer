#include "c_radar.h"
#include "c_radar_dsp.h"

#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- Defaults -------------------------------------------------------------
// One frame at 30 kHz: a quarter of the symbol ring's default depth, which
// leaves a batch-aligned window room to stay whole while the next batch fills
// in behind it. A window as deep as the ring would already have lost its first
// symbols by the time its last one has settled.
#define RADAR_DEFAULT_TIME_SYMBOLS 280
#define RADAR_DEFAULT_IFFT_SIZE 4096
#define RADAR_DEFAULT_CIR_DEPTH 128
#define RADAR_DEFAULT_BLANK_TAPS 2
#define RADAR_DEFAULT_DB_RANGE 60.0f
// The same starting point as the visualizer's CFR mean, since the settings
// window edits the two through one set of fields.
#define RADAR_DEFAULT_PERSIST_PERIOD 1120
#define RADAR_DEFAULT_PERSIST_N 8

// Hard ceiling on the window, so a typo cannot allocate gigabytes. The widest
// frame is a Range-Time at the largest IFFT: each of the two result buffers
// then costs 4096 * 8192 * sizeof(float) = 128 MB, and the complex delay
// matrix behind them 256 MB. The defaults (1120 x 4096) need about a tenth.
#define RADAR_MAX_TIME_SYMBOLS 4096
#define RADAR_MIN_TIME_SYMBOLS 14

#define RADAR_MIN_IFFT_SIZE 512
#define RADAR_MAX_IFFT_SIZE 8192

#define RADAR_MAX_LAG_SYMBOLS 4096

#define RADAR_MIN_DB_RANGE 10.0f
#define RADAR_MAX_DB_RANGE 200.0f

// Carrier for the Doppler axis's velocity scale. Wide enough for anything
// from sub-GHz LTE to FR2.
#define RADAR_DEFAULT_CARRIER_HZ 3.75e9
#define RADAR_MIN_CARRIER_HZ 1.0e8
#define RADAR_MAX_CARRIER_HZ 1.0e11

#define RADAR_SPEED_OF_LIGHT 299792458.0

// How long the worker idles between passes when nothing new has arrived.
#define RADAR_IDLE_SLEEP_MS 25

// Batch-aligned windows: how long the worker waits before looking again for a
// newly completed batch, and how far past a batch's last symbol both rings
// have to be before it counts as complete. The decode workers finish packets
// out of order, so the newest symbol can be saved before the ones just ahead
// of it; one subframe at 30 kHz is far beyond how far that reaches.
#define RADAR_BATCH_POLL_MS 2
#define RADAR_BATCH_SETTLE_SYMBOLS 28

// How many frames one ring may run on while the other's newest symbol does not
// move before the other counts as stalled; see fh_radar_stats::ul_stalled. TDD
// keeps a direction silent for at most its pattern period, 10 ms at most, and
// the check compares consecutive passes, so a slow pass cannot fake a stall.
#define RADAR_STALL_FRAMES 4

struct fh_radar {
    size_t fft_threads;

    // Transform buffers and plans. Created with the radar but only ever used
    // by the worker, so it needs no lock of its own.
    fh_radar_dsp_t* dsp;

    pthread_mutex_t lock;
    pthread_cond_t cv;

    fh_radar_config_t cfg;
    fh_radar_stats_t stats;

    // Rings published by the render thread. Borrowed pointers; the memories
    // outlive the radar.
    fh_oran_memory_t* dl;
    fh_oran_memory_t* ul;

    // Double-buffered magnitude matrices. The worker owns every allocation so
    // that a shape change can never free a buffer the render thread is reading.
    float* front;
    float* back;
    int rows, cols;           // shape of front
    int back_rows, back_cols; // shape of back
    int have_result;
    int borrowed;

    int reset_requested;
    // fh_radar_reset_persistence: handed to the workspace by the worker,
    // which is the only thread that may touch it.
    int persist_reset_requested;
    int stop;
    // Whether anyone is looking at the output. An inactive worker computes
    // nothing and keeps its last frame; see fh_radar_set_active.
    int active;

    // See fh_radar_stats::epoch. Kept outside stats because a reset wipes
    // those, and the count has to survive exactly the events it records.
    uint64_t epoch;

    // Moves on every fh_radar_set_config, so a batch already on screen is
    // computed again when any setting changes.
    uint64_t cfg_generation;

    // Batch-aligned windows; worker-only. The batch last published and the
    // config generation it was computed under, and the running count of
    // batches passed over. Cleared by a reset.
    int batch_valid;
    int64_t batch_last;
    uint64_t batch_cfg_generation;
    uint64_t batches_skipped;

    // Stall guard; worker-only. Each ring's newest symbol as last seen, and
    // the other ring's newest at the last pass where this one had moved.
    // Cleared by a reset.
    int stall_valid;
    int stall_dl_seen;
    int stall_ul_seen;
    int64_t stall_dl_at_ul_move;
    int64_t stall_ul_at_dl_move;

    // Rolling means of the published stats; worker-only. What each pairing
    // mean was last folded under, so it starts over when that changes, and
    // when each was last folded, for the weight. Cleared by a reset.
    int avg_pair_valid;
    uint64_t avg_pair_epoch;
    int avg_pair_window;
    double avg_pair_ms;
    float avg_paired;
    float avg_skipped_dl;
    float avg_skipped_ul;
    int avg_mag_valid;
    double avg_mag_ms;
    float avg_mag;
    int avg_time_valid;
    fh_radar_output_t avg_time_output;
    double avg_time_ms;
    double avg_refresh;
    double avg_cfr;
    double avg_transform;

    pthread_t worker;
    int worker_started;
};

// --- Config ---------------------------------------------------------------

void fh_radar_config_defaults(fh_radar_config_t* cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->time_symbols = RADAR_DEFAULT_TIME_SYMBOLS;
    cfg->batch_aligned = 1;
    cfg->freq_start_re = 0;
    cfg->freq_end_re = FH_RADAR_RE_COUNT - 1;
    cfg->dl_ul_lag_symbols = 0;
    cfg->division = RADAR_DIV_UL_OVER_DL;
    cfg->ifft_size = RADAR_DEFAULT_IFFT_SIZE;
    cfg->cir_depth_taps = RADAR_DEFAULT_CIR_DEPTH;
    cfg->range_window = RADAR_WINDOW_HANN;
    cfg->doppler_window = RADAR_WINDOW_HANN;
    cfg->carrier_freq_hz = RADAR_DEFAULT_CARRIER_HZ;
    cfg->mti_enabled = 1;
    cfg->blank_taps_enabled = 0;
    cfg->blank_taps = RADAR_DEFAULT_BLANK_TAPS;
    cfg->db_scale = 1;
    cfg->db_range = RADAR_DEFAULT_DB_RANGE;
    cfg->persist_enabled = 0;
    cfg->persist_period = RADAR_DEFAULT_PERSIST_PERIOD;
    cfg->persist_n = RADAR_DEFAULT_PERSIST_N;
    cfg->output = RADAR_OUT_CFR;
}

int fh_radar_min_ring_symbols(const fh_radar_config_t* cfg) {
    if (cfg == NULL || cfg->time_symbols <= 0) {
        return 0;
    }
    // Two batches - the one being read and the one filling in behind it - plus
    // the settle margin, and as much again for symbols that arrive while the
    // oldest rows of the batch are still being read.
    return 2 * cfg->time_symbols + 2 * RADAR_BATCH_SETTLE_SYMBOLS;
}

void fh_radar_used_re_range(const fh_radar_config_t* cfg, int* out_start, int* out_end) {
    if (cfg == NULL) {
        return;
    }
    const int start = cfg->freq_start_re;
    int end = cfg->freq_end_re;
    // Cropped from the start rather than centred, so the first RE used is
    // always the one the start field names.
    if (cfg->ifft_size > 0 && end - start + 1 > cfg->ifft_size) {
        end = start + cfg->ifft_size - 1;
    }
    if (out_start != NULL) {
        *out_start = start;
    }
    if (out_end != NULL) {
        *out_end = end;
    }
}

double fh_radar_velocity_per_hz(const fh_radar_config_t* cfg) {
    if (cfg == NULL || !(cfg->carrier_freq_hz > 0.0)) {
        return 0.0;
    }
    return RADAR_SPEED_OF_LIGHT / (2.0 * cfg->carrier_freq_hz);
}

static int clamp_int(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// Rounds up to a power of two inside the supported transform range. The delay
// axis scale depends on this, so a value between sizes has to resolve to one
// of them rather than being used as-is.
static int snap_pow2(int value) {
    int size = RADAR_MIN_IFFT_SIZE;
    if (value >= RADAR_MAX_IFFT_SIZE) {
        return RADAR_MAX_IFFT_SIZE;
    }
    while (size < value && size < RADAR_MAX_IFFT_SIZE) {
        size <<= 1;
    }
    return size;
}

// Every bound lives here so the GUI can hand raw widget values straight over.
static void sanitize_config(fh_radar_config_t* cfg) {
    cfg->time_symbols = clamp_int(cfg->time_symbols, RADAR_MIN_TIME_SYMBOLS, RADAR_MAX_TIME_SYMBOLS);
    cfg->batch_aligned = cfg->batch_aligned ? 1 : 0;

    cfg->freq_start_re = clamp_int(cfg->freq_start_re, 0, FH_RADAR_RE_COUNT - 1);
    cfg->freq_end_re = clamp_int(cfg->freq_end_re, cfg->freq_start_re, FH_RADAR_RE_COUNT - 1);

    cfg->dl_ul_lag_symbols = clamp_int(cfg->dl_ul_lag_symbols, -RADAR_MAX_LAG_SYMBOLS, RADAR_MAX_LAG_SYMBOLS);

    if (cfg->division != RADAR_DIV_DL_OVER_UL) {
        cfg->division = RADAR_DIV_UL_OVER_DL;
    }

    cfg->ifft_size = snap_pow2(cfg->ifft_size);

    // Re-clamped against the (possibly reduced) transform size, so shrinking
    // the IFFT cannot leave a depth that reads past the end of a CIR.
    cfg->cir_depth_taps = clamp_int(cfg->cir_depth_taps, 1, cfg->ifft_size);

    if (cfg->range_window != RADAR_WINDOW_HANN) {
        cfg->range_window = RADAR_WINDOW_RECT;
    }
    if (cfg->doppler_window != RADAR_WINDOW_HANN) {
        cfg->doppler_window = RADAR_WINDOW_RECT;
    }
    // Same NaN-safe shape as db_range below.
    if (!(cfg->carrier_freq_hz >= RADAR_MIN_CARRIER_HZ && cfg->carrier_freq_hz <= RADAR_MAX_CARRIER_HZ)) {
        cfg->carrier_freq_hz = (cfg->carrier_freq_hz > RADAR_MAX_CARRIER_HZ) ? RADAR_MAX_CARRIER_HZ
                             : (cfg->carrier_freq_hz < RADAR_MIN_CARRIER_HZ) ? RADAR_MIN_CARRIER_HZ
                             : RADAR_DEFAULT_CARRIER_HZ;
    }

    cfg->mti_enabled = cfg->mti_enabled ? 1 : 0;
    cfg->blank_taps_enabled = cfg->blank_taps_enabled ? 1 : 0;
    cfg->blank_taps = clamp_int(cfg->blank_taps, 0, cfg->ifft_size - 1);

    cfg->db_scale = cfg->db_scale ? 1 : 0;
    // Written so a NaN from a widget lands on the default rather than passing
    // both comparisons.
    if (!(cfg->db_range >= RADAR_MIN_DB_RANGE && cfg->db_range <= RADAR_MAX_DB_RANGE)) {
        cfg->db_range = (cfg->db_range > RADAR_MAX_DB_RANGE) ? RADAR_MAX_DB_RANGE
                      : (cfg->db_range < RADAR_MIN_DB_RANGE) ? RADAR_MIN_DB_RANGE
                      : RADAR_DEFAULT_DB_RANGE;
    }

    cfg->persist_enabled = cfg->persist_enabled ? 1 : 0;
    cfg->persist_period = clamp_int(cfg->persist_period, 1, FH_RADAR_MAX_PERSIST_PERIOD);
    cfg->persist_n = clamp_int(cfg->persist_n, 1, FH_RADAR_MAX_PERSIST_N);

    if (cfg->output != RADAR_OUT_RANGE_TIME && cfg->output != RADAR_OUT_RANGE_DOPPLER) {
        cfg->output = RADAR_OUT_CFR;
    }
}

// --- Ring access ----------------------------------------------------------

// Rounds toward minus infinity, so batch indices stay right below symbol 0.
static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static int ring_empty(const fh_oran_memory_t* memory) {
    return memory->symbol_highest < memory->symbol_lowest;
}

// Resolve the ring row holding an absolute symbol number, verifying the row's
// tag still matches so an evicted symbol is reported missing rather than read
// as whatever overwrote it. Mirrors ring_row_for_symbol in the visualizer.
// Wide, so a UL symbol a cycle offset away cannot overflow on the way here.
static int ring_row_for_symbol(const fh_oran_memory_t* memory, int64_t symbol_num, size_t* out_idx) {
    if (memory == NULL || memory->symbols == NULL) {
        return 0;
    }
    if (symbol_num < memory->symbol_lowest || symbol_num > memory->symbol_highest) {
        return 0;
    }
    // The decode side moves index_symbol_highest and symbol_highest one after
    // the other, so a lookup that lands between the two stores computes the
    // row next to the right one, and the tag check reports a symbol that is
    // there as missing - a fraction of a symbol per pass on average, all of
    // it near the write head. A second look sees both stores. The tag still
    // vouches for every row returned, so retrying can only recover a miss.
    size_t idx = 0;
    for (int attempt = 0;; attempt++) {
        const int highest = __atomic_load_n(&memory->symbol_highest, __ATOMIC_RELAXED);
        const int index_highest = __atomic_load_n(&memory->index_symbol_highest, __ATOMIC_RELAXED);
        const size_t age = (size_t)((int64_t)highest - symbol_num);
        idx = ((size_t)index_highest + memory->max_symbols - age % memory->max_symbols) % memory->max_symbols;
        if (memory->row_symbol_numbers == NULL || memory->row_symbol_numbers[idx] == symbol_num) {
            break;
        }
        if (attempt == 1) {
            return 0;
        }
    }
    if (memory->symbols[idx] == NULL) {
        return 0;
    }
    *out_idx = idx;
    return 1;
}

// Symbols per 256-frame cycle for a DL/UL pair: the DL's spacing, or the UL's
// while the DL has not reported one. 0 while neither has.
static int64_t pair_cycle_symbols(const fh_oran_memory_t* dl, const fh_oran_memory_t* ul) {
    const int64_t cycle = fh_oran_memory_cycle_symbols(dl);
    return (cycle > 0) ? cycle : fh_oran_memory_cycle_symbols(ul);
}

int64_t fh_radar_ul_cycle_offset(const fh_oran_memory_t* dl, const fh_oran_memory_t* ul) {
    if (dl == NULL || ul == NULL || ring_empty(dl) || ring_empty(ul)) {
        return 0;
    }
    const int64_t cycle = pair_cycle_symbols(dl, ul);
    if (cycle <= 0) {
        return 0;
    }
    // The two streams run side by side, so their newest symbols are a whole
    // number of cycles apart give or take a TDD period; rounding to the nearest
    // cycle recovers that number.
    const int64_t gap = (int64_t)ul->symbol_highest - (int64_t)dl->symbol_highest;
    return floor_div(gap + cycle / 2, cycle) * cycle;
}

static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static float complex_abs2(fh_complex_t v) {
    const float re = crealf(v);
    const float im = cimagf(v);
    return re * re + im * im;
}

// --- CFR stage ------------------------------------------------------------

// Where a CFR pass writes. The CFR view takes |H| on the 3276-wide RE grid.
// The transform outputs take the complex, windowed H of the used REs packed
// from bin 0 of an ifft_size-wide row, so the delay IFFT runs in place on it.
// Exactly one of the two is set.
typedef struct cfr_sink {
    float* magnitude;         // rows x FH_RADAR_RE_COUNT
    fh_complex_t* cplx;       // rows x cplx_cols
    int cplx_cols;
    int used_start;           // First RE packed into bin 0
    int used_count;           // REs packed; never more than cplx_cols
    const float* window;      // used_count weights, or NULL for rectangular
    unsigned char* row_valid; // rows; written alongside cplx
} cfr_sink_t;

// Clears one output row: a symbol without a channel estimate.
static void cfr_sink_clear_row(const cfr_sink_t* sink, int row) {
    if (sink->magnitude != NULL) {
        memset(sink->magnitude + (size_t)row * FH_RADAR_RE_COUNT, 0, FH_RADAR_RE_COUNT * sizeof(float));
    } else {
        memset(sink->cplx + (size_t)row * (size_t)sink->cplx_cols, 0, (size_t)sink->cplx_cols * sizeof(fh_complex_t));
        sink->row_valid[row] = 0;
    }
}

// |H| of one paired row over [re_start, re_end] into out_row, which arrives
// zeroed. Returns whether the DL carried anything in that range.
static int cfr_row_magnitude(const fh_complex_t* dl_row,
                             const fh_complex_t* ul_row,
                             int re_start,
                             int re_end,
                             int ul_over_dl,
                             float* out_row,
                             double* mag_sum,
                             uint64_t* mag_count) {
    int occupied_in_row = 0;

    for (int re = re_start; re <= re_end; re++) {
        const float dl_p = complex_abs2(dl_row[re]);
        const float ul_p = complex_abs2(ul_row[re]);

        if (dl_p > 0.0f) {
            occupied_in_row = 1;
        }

        // |H| = |numerator| / |denominator|; the CFR view needs no phase.
        const float num_p = ul_over_dl ? ul_p : dl_p;
        const float den_p = ul_over_dl ? dl_p : ul_p;

        // An unoccupied resource element divides 0 by 0. That is a normal,
        // expected intermediate here, so it is detected and skipped rather
        // than allowed to put a NaN into the display buffer.
        if (den_p <= 0.0f) {
            continue;
        }

        const float mag = sqrtf(num_p / den_p);
        out_row[re] = mag;

        // Only resource elements occupied in both directions say anything
        // about the channel, so only those feed the mean.
        if (num_p > 0.0f) {
            *mag_sum += (double)mag;
            (*mag_count)++;
        }
    }
    return occupied_in_row;
}

// Complex H of one paired row into the sink's row: the used REs from bin 0,
// windowed, and zeros up to cplx_cols. Returns whether the DL carried
// anything in the used range.
static int cfr_row_complex(const fh_complex_t* dl_row,
                           const fh_complex_t* ul_row,
                           const cfr_sink_t* sink,
                           int ul_over_dl,
                           fh_complex_t* out_row,
                           double* mag_sum,
                           uint64_t* mag_count) {
    // C11 lays a complex out as float[2], re then im. Working on the halves
    // keeps the divide a few multiplies: complex '*' and '/' would go through
    // libgcc's NaN-handling helpers, a call per resource element.
    const float* num = (const float*)(ul_over_dl ? ul_row : dl_row);
    const float* den = (const float*)(ul_over_dl ? dl_row : ul_row);
    const float* dl = (const float*)dl_row;
    float* out = (float*)out_row;

    int occupied_in_row = 0;

    for (int i = 0; i < sink->used_count; i++) {
        const int re = sink->used_start + i;
        const float nr = num[2 * re];
        const float ni = num[2 * re + 1];
        const float dr = den[2 * re];
        const float di = den[2 * re + 1];
        const float num_p = nr * nr + ni * ni;
        const float den_p = dr * dr + di * di;

        if (dl[2 * re] != 0.0f || dl[2 * re + 1] != 0.0f) {
            occupied_in_row = 1;
        }

        float hr = 0.0f;
        float hi = 0.0f;
        // A positive test rather than '<= 0 skip', so a NaN denominator is
        // skipped as well as 0/0.
        if (den_p > 0.0f) {
            // H = num * conj(den) / |den|^2
            const float inv = 1.0f / den_p;
            hr = (nr * dr + ni * di) * inv;
            hi = (ni * dr - nr * di) * inv;

            if (num_p > 0.0f) {
                *mag_sum += (double)sqrtf(num_p * inv);
                (*mag_count)++;
            }

            if (sink->window != NULL) {
                hr *= sink->window[i];
                hi *= sink->window[i];
            }
        }
        out[2 * i] = hr;
        out[2 * i + 1] = hi;
    }

    // Zero padding up to the transform size.
    memset(out_row + sink->used_count, 0, (size_t)(sink->cplx_cols - sink->used_count) * sizeof(fh_complex_t));
    return occupied_in_row;
}

int fh_radar_cfr_symbol(const fh_radar_config_t* cfg,
                        const fh_oran_memory_t* dl,
                        const fh_oran_memory_t* ul,
                        int dl_symbol,
                        int re_first,
                        int re_last,
                        fh_complex_t* out,
                        int* out_re) {
    if (cfg == NULL || out == NULL) {
        return -1;
    }
    size_t dl_idx = 0;
    size_t ul_idx = 0;
    if (!ring_row_for_symbol(dl, dl_symbol, &dl_idx) ||
        !ring_row_for_symbol(ul, (int64_t)dl_symbol + cfg->dl_ul_lag_symbols + fh_radar_ul_cycle_offset(dl, ul),
                             &ul_idx)) {
        return -1;
    }

    const int start = (re_first > cfg->freq_start_re) ? re_first : cfg->freq_start_re;
    const int end = (re_last < cfg->freq_end_re) ? re_last : cfg->freq_end_re;
    const int lo = (start < 0) ? 0 : start;
    const int hi = (end > FH_RADAR_RE_COUNT - 1) ? FH_RADAR_RE_COUNT - 1 : end;

    // The same float-halves arithmetic as cfr_row_complex, without the window.
    const int ul_over_dl = (cfg->division == RADAR_DIV_UL_OVER_DL);
    const float* num = (const float*)(ul_over_dl ? ul->symbols[ul_idx] : dl->symbols[dl_idx]);
    const float* den = (const float*)(ul_over_dl ? dl->symbols[dl_idx] : ul->symbols[ul_idx]);
    const float* dl_row = (const float*)dl->symbols[dl_idx];
    float* dst = (float*)out;

    int occupied = 0;
    int written = 0;
    for (int re = lo; re <= hi; re++) {
        if (dl_row[2 * re] != 0.0f || dl_row[2 * re + 1] != 0.0f) {
            occupied = 1;
        }
        const float nr = num[2 * re];
        const float ni = num[2 * re + 1];
        const float dr = den[2 * re];
        const float di = den[2 * re + 1];
        const float den_p = dr * dr + di * di;
        // Positive test, so a NaN denominator is skipped as well as 0/0.
        if (!(den_p > 0.0f)) {
            continue;
        }
        const float inv = 1.0f / den_p;
        dst[2 * written] = (nr * dr + ni * di) * inv;
        dst[2 * written + 1] = (ni * dr - nr * di) * inv;
        if (out_re != NULL) {
            out_re[written] = re;
        }
        written++;
    }
    return occupied ? written : -1;
}

// One full pass: pair each symbol of the window - rows symbols from oldest -
// across the two rings, divide, and write the result into the sink. DL symbol
// n pairs with UL symbol n + ul_shift: the lag plus fh_radar_ul_cycle_offset.
// Every row of the sink is written, whether or not its symbol paired.
// have_window is 0 while there is nothing to place a window on yet.
//
// The rings are read without taking the decode-side data lock, matching how
// the renderer already samples the same memory every frame. A row torn by a
// concurrent write shows up as one noisy symbol, not as a crash.
static void compute_cfr(const fh_radar_config_t* cfg,
                        const fh_oran_memory_t* dl,
                        const fh_oran_memory_t* ul,
                        int64_t ul_shift,
                        int have_window,
                        int oldest,
                        int rows,
                        const cfr_sink_t* sink,
                        fh_radar_stats_t* stats) {
    stats->paired_symbols = 0;
    stats->window_symbols = rows;
    stats->window_first_symbol = 0;
    stats->window_first_valid = 0;
    stats->skipped_dl_empty = 0;
    stats->skipped_ul_missing = 0;
    stats->mean_cfr_magnitude = 0.0f;

    // Resource elements outside the configured frequency range stay zero,
    // which keeps the CFR view on the same 3276-wide grid the heatmap and its
    // RE axis already use.
    if (sink->magnitude != NULL) {
        memset(sink->magnitude, 0, (size_t)rows * FH_RADAR_RE_COUNT * sizeof(float));
    }

    if (dl == NULL || ul == NULL || !have_window) {
        // No pair, or nothing captured yet.
        if (sink->cplx != NULL) {
            for (int row = 0; row < rows; row++) {
                cfr_sink_clear_row(sink, row);
            }
        }
        return;
    }

    const int newest = oldest + rows - 1;

    // Lets the vertical axis label real symbol numbers rather than row indices.
    stats->window_first_symbol = oldest;
    stats->window_first_valid = 1;

    const int ul_over_dl = (cfg->division == RADAR_DIV_UL_OVER_DL);

    double mag_sum = 0.0;
    uint64_t mag_count = 0;

    for (int sym = oldest; sym <= newest; sym++) {
        // Row 0 is the oldest symbol in the window, matching the waterfall's
        // top-to-bottom time order.
        const int row = sym - oldest;

        size_t dl_idx = 0;
        if (!ring_row_for_symbol(dl, sym, &dl_idx)) {
            stats->skipped_dl_empty++;
            cfr_sink_clear_row(sink, row);
            continue;
        }

        size_t ul_idx = 0;
        if (!ring_row_for_symbol(ul, (int64_t)sym + ul_shift, &ul_idx)) {
            stats->skipped_ul_missing++;
            cfr_sink_clear_row(sink, row);
            continue;
        }

        const fh_complex_t* dl_row = dl->symbols[dl_idx];
        const fh_complex_t* ul_row = ul->symbols[ul_idx];

        int occupied_in_row;
        if (sink->magnitude != NULL) {
            occupied_in_row = cfr_row_magnitude(dl_row, ul_row, cfg->freq_start_re, cfg->freq_end_re, ul_over_dl,
                                                sink->magnitude + (size_t)row * FH_RADAR_RE_COUNT,
                                                &mag_sum, &mag_count);
        } else {
            occupied_in_row = cfr_row_complex(dl_row, ul_row, sink, ul_over_dl,
                                              sink->cplx + (size_t)row * (size_t)sink->cplx_cols,
                                              &mag_sum, &mag_count);
        }

        if (!occupied_in_row) {
            // The rows paired, but the downlink carried nothing to divide by,
            // so this symbol contributes no channel estimate.
            cfr_sink_clear_row(sink, row);
            stats->skipped_dl_empty++;
            continue;
        }

        if (sink->cplx != NULL) {
            sink->row_valid[row] = 1;
        }
        stats->paired_symbols++;
    }

    if (mag_count > 0) {
        stats->mean_cfr_magnitude = (float)(mag_sum / (double)mag_count);
    }
}

// A Range-Time or Range-Doppler pass: the CFR stage into the delay matrix,
// then the transform stages into dst (rows x cols).
static void compute_transform_frame(fh_radar_dsp_t* dsp,
                                    const fh_radar_config_t* cfg,
                                    const fh_oran_memory_t* dl,
                                    const fh_oran_memory_t* ul,
                                    int64_t ul_shift,
                                    int have_window,
                                    int oldest,
                                    float* dst,
                                    int rows,
                                    int out_rows,
                                    int cols,
                                    fh_radar_stats_t* stats) {
    int used_start = 0;
    int used_end = 0;
    fh_radar_used_re_range(cfg, &used_start, &used_end);
    stats->used_re_start = used_start;
    stats->used_re_end = used_end;
    stats->window_symbols = rows;

    const double started = monotonic_ms();

    unsigned char* row_valid = NULL;
    fh_complex_t* cir = fh_radar_dsp_begin(dsp, rows, cfg->ifft_size, &row_valid);
    if (cir == NULL) {
        memset(dst, 0, (size_t)out_rows * (size_t)cols * sizeof(float));
        return;
    }

    cfr_sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.cplx = cir;
    sink.cplx_cols = cfg->ifft_size;
    sink.used_start = used_start;
    sink.used_count = used_end - used_start + 1;
    sink.window = fh_radar_dsp_range_window(dsp, cfg->range_window, sink.used_count);
    sink.row_valid = row_valid;

    compute_cfr(cfg, dl, ul, ul_shift, have_window, oldest, rows, &sink, stats);

    const double cfr_done = monotonic_ms();
    stats->cfr_ms = cfr_done - started;

    // Run even when nothing paired: the transforms are skipped then, but a
    // rolling mean still has to be drawn.
    fh_radar_dsp_pass_t pass;
    memset(&pass, 0, sizeof(pass));
    pass.used_re = sink.used_count;
    pass.paired_symbols = stats->paired_symbols;
    pass.first_symbol = oldest;
    pass.epoch = stats->epoch;
    pass.out_rows = out_rows;
    fh_radar_dsp_run(dsp, cfg, &pass, dst);
    stats->transform_ms = monotonic_ms() - cfr_done;

    if (cfg->persist_enabled) {
        stats->persist_phase_rows = (cfg->output == RADAR_OUT_RANGE_TIME);
        stats->persist_period = stats->persist_phase_rows ? out_rows : 0;
        stats->persist_folds = pass.persist_folds;
    }
}

// --- Worker ---------------------------------------------------------------

// Grows the scratch buffer to the requested shape. Only ever called from the
// worker, which is what keeps a shape change from freeing a borrowed buffer.
static int ensure_back_buffer(fh_radar_t* r, int rows, int cols) {
    if (r->back != NULL && r->back_rows == rows && r->back_cols == cols) {
        return 1;
    }
    float* buf = (float*)calloc((size_t)rows * (size_t)cols, sizeof(float));
    if (buf == NULL) {
        return 0;
    }
    free(r->back);
    r->back = buf;
    r->back_rows = rows;
    r->back_cols = cols;
    return 1;
}

// Waits on r->cv for at most ms, with r->lock held. Returns early when woken:
// by a config change, a reset, a new pair, stop, or a released frame.
static void wait_locked(fh_radar_t* r, int ms) {
    if (r->stop) {
        return;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)ms * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }
    pthread_cond_timedwait(&r->cv, &r->lock, &deadline);
}

// The newest batch of rows symbols that has settled in both rings: the DL up
// to its last symbol, and the UL up to its partner, that symbol plus
// ul_shift, each with RADAR_BATCH_SETTLE_SYMBOLS to spare. Batches are
// numbered in the DL's symbols. Returns 0 while there is none.
static int newest_complete_batch(const fh_oran_memory_t* dl,
                                 const fh_oran_memory_t* ul,
                                 int64_t ul_shift,
                                 int rows,
                                 int64_t* out_batch) {
    if (ring_empty(dl) || ring_empty(ul)) {
        return 0; // nothing captured yet
    }
    const int64_t dl_settled = (int64_t)dl->symbol_highest - RADAR_BATCH_SETTLE_SYMBOLS;
    const int64_t ul_settled = (int64_t)ul->symbol_highest - ul_shift - RADAR_BATCH_SETTLE_SYMBOLS;
    const int64_t settled = (dl_settled < ul_settled) ? dl_settled : ul_settled;

    // Batch k ends at (k + 1) * rows - 1, so the newest one ending at or
    // before the settled symbol is:
    const int64_t batch = floor_div(settled + 1, rows) - 1;
    if (batch < 0) {
        return 0;
    }
    *out_batch = batch;
    return 1;
}

// Feeds the stall guard the rings' newest symbols and reports which ring, if
// either, has stood still while the other ran on by more than
// RADAR_STALL_FRAMES. Each ring is measured in its own numbering, so a cycle
// offset between them does not matter. With r->lock held.
static void track_stall(fh_radar_t* r,
                        const fh_oran_memory_t* dl,
                        const fh_oran_memory_t* ul,
                        int64_t cycle,
                        int* out_dl_stalled,
                        int* out_ul_stalled) {
    *out_dl_stalled = 0;
    *out_ul_stalled = 0;
    if (ring_empty(dl) || ring_empty(ul)) {
        r->stall_valid = 0; // wiped, or nothing yet: start watching afresh
        return;
    }
    const int dl_high = dl->symbol_highest;
    const int ul_high = ul->symbol_highest;
    if (!r->stall_valid) {
        r->stall_valid = 1;
        r->stall_dl_seen = dl_high;
        r->stall_ul_seen = ul_high;
        r->stall_dl_at_ul_move = dl_high;
        r->stall_ul_at_dl_move = ul_high;
        return;
    }
    if (ul_high != r->stall_ul_seen) {
        r->stall_ul_seen = ul_high;
        r->stall_dl_at_ul_move = dl_high;
    }
    if (dl_high != r->stall_dl_seen) {
        r->stall_dl_seen = dl_high;
        r->stall_ul_at_dl_move = ul_high;
    }
    if (cycle <= 0) {
        return;
    }
    const int64_t limit = (int64_t)RADAR_STALL_FRAMES * (cycle / 256);
    *out_ul_stalled = ((int64_t)dl_high - r->stall_dl_at_ul_move) > limit;
    *out_dl_stalled = ((int64_t)ul_high - r->stall_ul_at_dl_move) > limit;
}

// How much of a new value a rolling mean last folded at last_ms takes in:
// FH_RADAR_STATS_MEAN_TAU_MS of wall time, however many passes that is.
static double mean_weight(double now_ms, double last_ms) {
    const double dt = now_ms - last_ms;
    return (dt > 0.0) ? 1.0 - exp(-dt / FH_RADAR_STATS_MEAN_TAU_MS) : 0.0;
}

// Folds a pass about to be published into the rolling means and writes them
// into it. With r->lock held.
static void fold_stats_means(fh_radar_t* r, fh_radar_stats_t* pass, double now_ms) {
    // Counts out of a different window depth, or values measured another way
    // (a new epoch), do not average with the ones before.
    if (!r->avg_pair_valid || r->avg_pair_epoch != pass->epoch || r->avg_pair_window != pass->window_symbols) {
        r->avg_pair_valid = 1;
        r->avg_pair_epoch = pass->epoch;
        r->avg_pair_window = pass->window_symbols;
        r->avg_paired = (float)pass->paired_symbols;
        r->avg_skipped_dl = (float)pass->skipped_dl_empty;
        r->avg_skipped_ul = (float)pass->skipped_ul_missing;
        r->avg_mag_valid = 0;
    } else {
        const float w = (float)mean_weight(now_ms, r->avg_pair_ms);
        r->avg_paired += w * ((float)pass->paired_symbols - r->avg_paired);
        r->avg_skipped_dl += w * ((float)pass->skipped_dl_empty - r->avg_skipped_dl);
        r->avg_skipped_ul += w * ((float)pass->skipped_ul_missing - r->avg_skipped_ul);
    }
    r->avg_pair_ms = now_ms;

    // A pass that paired nothing has no |H| to report, not an |H| of 0.
    if (pass->paired_symbols > 0) {
        if (!r->avg_mag_valid) {
            r->avg_mag_valid = 1;
            r->avg_mag = pass->mean_cfr_magnitude;
        } else {
            r->avg_mag += (float)mean_weight(now_ms, r->avg_mag_ms) * (pass->mean_cfr_magnitude - r->avg_mag);
        }
        r->avg_mag_ms = now_ms;
    }

    pass->avg_paired_symbols = r->avg_paired;
    pass->avg_skipped_dl_empty = r->avg_skipped_dl;
    pass->avg_skipped_ul_missing = r->avg_skipped_ul;
    pass->avg_cfr_magnitude = r->avg_mag_valid ? r->avg_mag : 0.0f;

    // Each output costs its own, so switching starts the timings over.
    if (!r->avg_time_valid || r->avg_time_output != pass->output) {
        r->avg_time_valid = 1;
        r->avg_time_output = pass->output;
        r->avg_refresh = pass->last_refresh_ms;
        r->avg_cfr = pass->cfr_ms;
        r->avg_transform = pass->transform_ms;
    } else {
        const double w = mean_weight(now_ms, r->avg_time_ms);
        r->avg_refresh += w * (pass->last_refresh_ms - r->avg_refresh);
        r->avg_cfr += w * (pass->cfr_ms - r->avg_cfr);
        r->avg_transform += w * (pass->transform_ms - r->avg_transform);
    }
    r->avg_time_ms = now_ms;

    pass->avg_refresh_ms = r->avg_refresh;
    pass->avg_cfr_ms = r->avg_cfr;
    pass->avg_transform_ms = r->avg_transform;
}

static void* radar_worker(void* arg) {
    fh_radar_t* r = (fh_radar_t*)arg;

    for (;;) {
        pthread_mutex_lock(&r->lock);
        if (r->stop) {
            pthread_mutex_unlock(&r->lock);
            break;
        }

        if (r->reset_requested) {
            r->reset_requested = 0;
            r->have_result = 0;
            memset(&r->stats, 0, sizeof(r->stats));
            r->batch_valid = 0;
            r->batches_skipped = 0;
            r->stall_valid = 0;
            r->avg_pair_valid = 0;
            r->avg_mag_valid = 0;
            r->avg_time_valid = 0;
        }

        // Nobody is looking: sleep until something changes rather than run
        // transforms whose frames would never be drawn.
        if (!r->active) {
            pthread_cond_wait(&r->cv, &r->lock);
            pthread_mutex_unlock(&r->lock);
            continue;
        }

        const fh_radar_config_t cfg = r->cfg;
        const uint64_t epoch = r->epoch;
        const uint64_t cfg_generation = r->cfg_generation;
        fh_oran_memory_t* dl = r->dl;
        fh_oran_memory_t* ul = r->ul;

        int rows = cfg.time_symbols;
        // The window can never be deeper than the ring it reads from.
        if (dl != NULL && dl->max_symbols > 0 && (size_t)rows > dl->max_symbols) {
            rows = (int)dl->max_symbols;
        }

        // Where each DL symbol's UL partner sits, taken once so the batch check
        // and the pass agree on it: the lag, on the rolling frame_id.
        const int have_pair = dl != NULL && ul != NULL;
        const int64_t cycle = have_pair ? pair_cycle_symbols(dl, ul) : 0;
        const int64_t ul_offset = have_pair ? fh_radar_ul_cycle_offset(dl, ul) : 0;
        const int64_t ul_shift = (int64_t)cfg.dl_ul_lag_symbols + ul_offset;
        const int ul_cycle_offset = (cycle > 0) ? (int)(ul_offset / cycle) : 0;

        // That pairing cannot tell a live ring from a stalled one whose old
        // rows sit at the same place in the cycle, so a stalled ring holds
        // the last frame on screen instead.
        int dl_stalled = 0;
        int ul_stalled = 0;
        if (have_pair) {
            track_stall(r, dl, ul, cycle, &dl_stalled, &ul_stalled);
        }
        r->stats.ul_cycle_offset = ul_cycle_offset;
        r->stats.dl_stalled = dl_stalled;
        r->stats.ul_stalled = ul_stalled;
        if (dl_stalled || ul_stalled) {
            r->stats.have_dl = 1;
            r->stats.have_ul = 1;
            wait_locked(r, cfg.batch_aligned ? RADAR_BATCH_POLL_MS : RADAR_IDLE_SLEEP_MS);
            pthread_mutex_unlock(&r->lock);
            continue;
        }

        // Where the window sits, settled before the pass so the batch check
        // and the frame agree on it. Without a pair there is nothing to align
        // to, and the pass below only reports what is missing.
        const int batched = cfg.batch_aligned && have_pair;
        int have_window = 0;
        int oldest = 0;
        int64_t batch = 0;
        if (batched) {
            if (!newest_complete_batch(dl, ul, ul_shift, rows, &batch)) {
                r->stats.batch_waiting = 1;
                r->stats.have_dl = 1;
                r->stats.have_ul = 1;
                wait_locked(r, RADAR_BATCH_POLL_MS);
                pthread_mutex_unlock(&r->lock);
                continue;
            }
            if (r->batch_valid && batch == r->batch_last && cfg_generation == r->batch_cfg_generation) {
                // Already on screen, computed with these settings.
                wait_locked(r, RADAR_BATCH_POLL_MS);
                pthread_mutex_unlock(&r->lock);
                continue;
            }
            oldest = (int)(batch * (int64_t)rows);
            have_window = 1;
        } else if (dl != NULL && dl->symbol_highest >= dl->symbol_lowest) {
            // Sliding: the window ends at the newest downlink symbol.
            oldest = dl->symbol_highest - rows + 1;
            have_window = 1;
        }

        // Every output has one row per symbol of the window (Range-Doppler's
        // are Doppler bins, as many as there are symbols) - except a
        // Range-Time mean, which has one per phase of its period. The width
        // is what differs between outputs.
        int out_rows = rows;
        int cols = FH_RADAR_RE_COUNT;
        if (cfg.output == RADAR_OUT_RANGE_TIME) {
            cols = cfg.ifft_size;
            if (cfg.persist_enabled) {
                out_rows = cfg.persist_period;
            }
        } else if (cfg.output == RADAR_OUT_RANGE_DOPPLER) {
            cols = cfg.cir_depth_taps;
        }

        const int persist_reset = r->persist_reset_requested;
        r->persist_reset_requested = 0;

        if (!ensure_back_buffer(r, out_rows, cols)) {
            pthread_mutex_unlock(&r->lock);
            break; // out of memory; stop rather than spin
        }
        float* back = r->back;
        pthread_mutex_unlock(&r->lock);

        // Heavy work runs unlocked so the render thread never blocks on it.
        const double started = monotonic_ms();
        fh_radar_stats_t pass;
        memset(&pass, 0, sizeof(pass));
        pass.have_dl = (dl != NULL);
        pass.have_ul = (ul != NULL);
        // Taken with the config under the same lock, so the frame is labelled
        // with what it was actually computed from, not what is current by the
        // time it is published.
        pass.output = cfg.output;
        pass.epoch = epoch;
        pass.ul_cycle_offset = ul_cycle_offset;

        if (persist_reset) {
            fh_radar_dsp_reset_persistence(r->dsp);
        }

        if (cfg.output == RADAR_OUT_CFR) {
            cfr_sink_t sink;
            memset(&sink, 0, sizeof(sink));
            sink.magnitude = back;
            compute_cfr(&cfg, dl, ul, ul_shift, have_window, oldest, rows, &sink, &pass);
            pass.cfr_ms = monotonic_ms() - started;
        } else {
            compute_transform_frame(r->dsp, &cfg, dl, ul, ul_shift, have_window, oldest, back, rows, out_rows, cols,
                                    &pass);
        }
        pass.last_refresh_ms = monotonic_ms() - started;

        pthread_mutex_lock(&r->lock);
        // Publishing swaps buffers, so it has to wait out any borrow the render
        // thread holds; that borrow lasts a single frame.
        while (r->borrowed && !r->stop) {
            pthread_cond_wait(&r->cv, &r->lock);
        }
        if (r->stop) {
            pthread_mutex_unlock(&r->lock);
            break;
        }

        // Swap: the freshly computed buffer becomes the published one, and the
        // retired front becomes scratch. The retired buffer keeps whatever
        // shape it was published at, so ensure_back_buffer can reallocate it
        // next pass if the window changed size in between.
        float* const old_front = r->front;
        const int old_rows = r->rows;
        const int old_cols = r->cols;

        r->front = r->back;
        r->rows = out_rows;
        r->cols = cols;

        r->back = old_front;
        r->back_rows = (old_front != NULL) ? old_rows : 0;
        r->back_cols = (old_front != NULL) ? old_cols : 0;

        r->have_result = 1;

        if (batched) {
            // Only a step under the same settings is a skip: a new window depth
            // renumbers every batch.
            if (r->batch_valid && cfg_generation == r->batch_cfg_generation && batch > r->batch_last + 1) {
                r->batches_skipped += (uint64_t)(batch - r->batch_last - 1);
            }
            r->batch_valid = 1;
            r->batch_last = batch;
            r->batch_cfg_generation = cfg_generation;
            pass.batch_index = batch;
        }
        pass.batches_skipped = r->batches_skipped;
        pass.refresh_count = r->stats.refresh_count + 1;
        fold_stats_means(r, &pass, monotonic_ms());
        r->stats = pass;

        // A sliding window idles between passes. A batch goes straight back to
        // watching for the next one, which the check above turns into a short
        // wait while there is none.
        if (!batched) {
            wait_locked(r, RADAR_IDLE_SLEEP_MS);
        }
        pthread_mutex_unlock(&r->lock);
    }

    return NULL;
}

// --- Lifecycle ------------------------------------------------------------

fh_radar_t* fh_radar_create(size_t fft_threads) {
    fh_radar_t* r = (fh_radar_t*)calloc(1, sizeof(fh_radar_t));
    if (r == NULL) {
        return NULL;
    }

    r->fft_threads = (fft_threads < 1) ? 1 : fft_threads;

    // Set up FFTW's threading now so a missing or mismatched threads library
    // fails here rather than on the first transform. The planner itself is not
    // thread-safe, so all planning stays on the worker under r->lock.
    if (fftwf_init_threads() != 0) {
        fftwf_plan_with_nthreads((int)r->fft_threads);
    } else {
        r->fft_threads = 1;
    }

    fh_radar_config_defaults(&r->cfg);

    r->dsp = fh_radar_dsp_create((int)r->fft_threads);
    if (r->dsp == NULL) {
        free(r);
        return NULL;
    }

    if (pthread_mutex_init(&r->lock, NULL) != 0) {
        fh_radar_dsp_destroy(r->dsp);
        free(r);
        return NULL;
    }
    if (pthread_cond_init(&r->cv, NULL) != 0) {
        pthread_mutex_destroy(&r->lock);
        fh_radar_dsp_destroy(r->dsp);
        free(r);
        return NULL;
    }

    if (pthread_create(&r->worker, NULL, radar_worker, r) != 0) {
        pthread_cond_destroy(&r->cv);
        pthread_mutex_destroy(&r->lock);
        fh_radar_dsp_destroy(r->dsp);
        free(r);
        return NULL;
    }
    r->worker_started = 1;

    return r;
}

void fh_radar_destroy(fh_radar_t* r) {
    if (r == NULL) {
        return;
    }

    pthread_mutex_lock(&r->lock);
    r->stop = 1;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->lock);

    if (r->worker_started) {
        pthread_join(r->worker, NULL);
    }

    free(r->front);
    free(r->back);

    // Its plans have to go before FFTW's threads do.
    fh_radar_dsp_destroy(r->dsp);

    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->lock);

    fftwf_cleanup_threads();

    free(r);
}

size_t fh_radar_get_fft_threads(const fh_radar_t* r) {
    return (r != NULL) ? r->fft_threads : 0;
}

// --- Public accessors -----------------------------------------------------

void fh_radar_get_config(const fh_radar_t* r, fh_radar_config_t* out) {
    if (r == NULL || out == NULL) {
        return;
    }
    fh_radar_t* mut = (fh_radar_t*)r;
    pthread_mutex_lock(&mut->lock);
    *out = r->cfg;
    pthread_mutex_unlock(&mut->lock);
}

void fh_radar_set_config(fh_radar_t* r, const fh_radar_config_t* cfg) {
    if (r == NULL || cfg == NULL) {
        return;
    }
    fh_radar_config_t sane = *cfg;
    sanitize_config(&sane);

    pthread_mutex_lock(&r->lock);
    // Only the fields that change what a cell holds move the epoch. The window
    // depth and the transform settings reshape the frame, but a symbol's |H|
    // stays the same number, so an average across them is still an average.
    if (sane.freq_start_re != r->cfg.freq_start_re ||
        sane.freq_end_re != r->cfg.freq_end_re ||
        sane.dl_ul_lag_symbols != r->cfg.dl_ul_lag_symbols ||
        sane.division != r->cfg.division) {
        r->epoch++;
    }
    r->cfg = sane;
    r->cfg_generation++;
    // Wake the worker so an edit takes effect on the next pass rather than
    // after the current idle interval expires.
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->lock);
}

void fh_radar_get_stats(const fh_radar_t* r, fh_radar_stats_t* out) {
    if (out == NULL) {
        return;
    }
    if (r == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    fh_radar_t* mut = (fh_radar_t*)r;
    pthread_mutex_lock(&mut->lock);
    *out = r->stats;
    pthread_mutex_unlock(&mut->lock);
}

void fh_radar_reset(fh_radar_t* r) {
    if (r == NULL) {
        return;
    }
    pthread_mutex_lock(&r->lock);
    r->reset_requested = 1;
    r->epoch++;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->lock);
}

void fh_radar_reset_persistence(fh_radar_t* r) {
    if (r == NULL) {
        return;
    }
    pthread_mutex_lock(&r->lock);
    r->persist_reset_requested = 1;
    // Counts as a settings change, so a batch already on screen is computed
    // again and the emptied mean shows straight away.
    r->cfg_generation++;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->lock);
}

void fh_radar_set_active(fh_radar_t* r, int active) {
    if (r == NULL) {
        return;
    }
    active = active ? 1 : 0;
    pthread_mutex_lock(&r->lock);
    if (r->active != active) {
        r->active = active;
        pthread_cond_broadcast(&r->cv);
    }
    pthread_mutex_unlock(&r->lock);
}

void fh_radar_submit(fh_radar_t* r, fh_oran_memory_t* dl, fh_oran_memory_t* ul) {
    if (r == NULL) {
        return;
    }
    pthread_mutex_lock(&r->lock);
    if (r->dl != dl || r->ul != ul) {
        // A different pair no longer describes the published frame.
        r->dl = dl;
        r->ul = ul;
        r->reset_requested = 1;
        r->epoch++;
        pthread_cond_broadcast(&r->cv);
    }
    pthread_mutex_unlock(&r->lock);
}

void fh_radar_get_pair(fh_radar_t* r, const fh_oran_memory_t** out_dl, const fh_oran_memory_t** out_ul) {
    const fh_oran_memory_t* dl = NULL;
    const fh_oran_memory_t* ul = NULL;
    if (r != NULL) {
        pthread_mutex_lock(&r->lock);
        dl = r->dl;
        ul = r->ul;
        pthread_mutex_unlock(&r->lock);
    }
    if (out_dl != NULL) {
        *out_dl = dl;
    }
    if (out_ul != NULL) {
        *out_ul = ul;
    }
}

const float* fh_radar_acquire_result(fh_radar_t* r, int* out_rows, int* out_cols) {
    if (r == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&r->lock);
    if (!r->have_result || r->front == NULL) {
        pthread_mutex_unlock(&r->lock);
        return NULL;
    }
    r->borrowed = 1;
    const float* result = r->front;
    if (out_rows != NULL) {
        *out_rows = r->rows;
    }
    if (out_cols != NULL) {
        *out_cols = r->cols;
    }
    pthread_mutex_unlock(&r->lock);
    return result;
}

void fh_radar_release_result(fh_radar_t* r) {
    if (r == NULL) {
        return;
    }
    pthread_mutex_lock(&r->lock);
    r->borrowed = 0;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->lock);
}
