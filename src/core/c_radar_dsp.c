#include "c_radar_dsp.h"

// After c_radar_dsp.h, which brings in <complex.h>: FFTW then defines
// fftwf_complex as float _Complex, the same type as fh_complex_t, so the
// matrices go to the planner without a cast.
#include <fftw3.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Rolling mean: a cell nothing has been folded into yet. Negative, which no
// power can be, so the visualizer draws it black like its own means' empty
// cells.
#define PERSIST_EMPTY_CELL (-1.0f)

// Slots in the Range-Time mean's folded-symbol tag ring. Has to exceed the
// deepest window (RADAR_MAX_TIME_SYMBOLS, 4096) so two symbols of one pass can
// never share a slot.
#define PERSIST_TAG_SLOTS 8192

typedef struct radar_window_cache {
    fh_radar_window_t type;
    int len;
    float* weights; // NULL for rectangular
    // Coherent gain: what a constant input sums to through the window, which
    // is what a single path peaks at after the transform.
    double sum;
} radar_window_cache_t;

// Everything that decides what a rolling-mean cell measures. Any change and
// the mean starts over: averaging across it would blend two different
// quantities. The dB settings are absent on purpose - they apply after the
// mean.
typedef struct radar_persist_key {
    int output;
    int rows; // accumulator rows: persist_period for Range-Time, the window for Range-Doppler
    int cols;
    int ifft_size;
    int used_re;
    int range_window;
    int doppler_window;
    int mti_enabled;
    int blank_taps; // 0 when blanking is off
    uint64_t epoch;
} radar_persist_key_t;

typedef struct radar_persist {
    int have_key;
    radar_persist_key_t key;
    float* accum; // key.rows x key.cols mean powers, PERSIST_EMPTY_CELL where none yet

    // Range-Time: the symbols folded so far, keyed symbol % PERSIST_TAG_SLOTS,
    // so a window that overlaps the last one - or the same batch computed
    // again after a settings change - folds each symbol once.
    int* tags;
    int has_folded;
    int first_symbol;
    int last_symbol;

    // Range-Doppler: the window last folded, for the same reason, and how
    // many maps went in.
    int has_last_window;
    int last_window_first;
    int maps_folded;
} radar_persist_t;

struct fh_radar_dsp {
    int fft_threads;

    // Delay stage: rows x n, transformed in place, one CIR per row.
    fh_complex_t* cir;
    unsigned char* row_valid;
    int rows;
    int n;
    fftwf_plan delay_plan;

    // Doppler stage: taps x rows. Tap-major, so each slow-time FFT runs over
    // contiguous memory instead of striding through the CIR matrix.
    fh_complex_t* slow;
    int slow_rows;
    int slow_taps;
    fftwf_plan doppler_plan;

    // MTI's per-tap sum, as interleaved re/im doubles: the direct path can
    // sit tens of dB above the targets, and float accumulation over a long
    // window would leave its residual above them.
    double* tap_sum;
    int tap_sum_len;

    radar_window_cache_t range_window;
    radar_window_cache_t doppler_window;

    radar_persist_t persist;
};

// --- Windows --------------------------------------------------------------

// Brings the cache to (type, len). Returns 0 when the weights could not be
// allocated; the cache is then rectangular, which still gives a valid frame.
static int window_cache_get(radar_window_cache_t* c, fh_radar_window_t type, int len) {
    if (c->type == type && c->len == len) {
        return 1;
    }
    free(c->weights);
    c->weights = NULL;
    c->type = type;
    c->len = len;
    c->sum = (double)len;

    if (type == RADAR_WINDOW_RECT || len <= 1) {
        return 1;
    }

    float* w = (float*)malloc((size_t)len * sizeof(float));
    if (w == NULL) {
        // Recorded as rectangular so the gain matches what is applied; the
        // next pass asks for Hann again and retries.
        c->type = RADAR_WINDOW_RECT;
        return 0;
    }
    double sum = 0.0;
    for (int k = 0; k < len; k++) {
        const double v = 0.5 - 0.5 * cos(2.0 * M_PI * (double)k / (double)(len - 1));
        w[k] = (float)v;
        sum += v;
    }
    c->weights = w;
    c->sum = sum;
    return 1;
}

// --- Lifecycle ------------------------------------------------------------

fh_radar_dsp_t* fh_radar_dsp_create(int fft_threads) {
    fh_radar_dsp_t* dsp = (fh_radar_dsp_t*)calloc(1, sizeof(fh_radar_dsp_t));
    if (dsp == NULL) {
        return NULL;
    }
    dsp->fft_threads = (fft_threads < 1) ? 1 : fft_threads;
    return dsp;
}

static void free_delay_stage(fh_radar_dsp_t* dsp) {
    if (dsp->delay_plan != NULL) {
        fftwf_destroy_plan(dsp->delay_plan);
        dsp->delay_plan = NULL;
    }
    fftwf_free(dsp->cir);
    dsp->cir = NULL;
    free(dsp->row_valid);
    dsp->row_valid = NULL;
    dsp->rows = 0;
    dsp->n = 0;
}

static void free_doppler_stage(fh_radar_dsp_t* dsp) {
    if (dsp->doppler_plan != NULL) {
        fftwf_destroy_plan(dsp->doppler_plan);
        dsp->doppler_plan = NULL;
    }
    fftwf_free(dsp->slow);
    dsp->slow = NULL;
    dsp->slow_rows = 0;
    dsp->slow_taps = 0;
}

void fh_radar_dsp_destroy(fh_radar_dsp_t* dsp) {
    if (dsp == NULL) {
        return;
    }
    free_delay_stage(dsp);
    free_doppler_stage(dsp);
    free(dsp->tap_sum);
    free(dsp->range_window.weights);
    free(dsp->doppler_window.weights);
    free(dsp->persist.accum);
    free(dsp->persist.tags);
    free(dsp);
}

// --- Buffers and plans ----------------------------------------------------

fh_complex_t* fh_radar_dsp_begin(fh_radar_dsp_t* dsp, int rows, int ifft_size, unsigned char** out_row_valid) {
    if (dsp == NULL || rows <= 0 || ifft_size <= 0) {
        return NULL;
    }

    if (dsp->cir == NULL || dsp->rows != rows || dsp->n != ifft_size) {
        free_delay_stage(dsp);

        const size_t cells = (size_t)rows * (size_t)ifft_size;
        // fftwf_malloc, not malloc: FFTW only takes its SIMD paths on arrays
        // aligned the way it allocates them.
        fh_complex_t* cir = (fh_complex_t*)fftwf_malloc(cells * sizeof(fh_complex_t));
        unsigned char* row_valid = (unsigned char*)malloc((size_t)rows);
        if (cir == NULL || row_valid == NULL) {
            fftwf_free(cir);
            free(row_valid);
            return NULL;
        }

        // One IFFT per row, in place. FFTW_ESTIMATE never touches the arrays,
        // so the plan can be made before the CFR stage has filled them, and it
        // takes microseconds where FFTW_MEASURE would take seconds at 8192.
        const int n = ifft_size;
        fftwf_plan_with_nthreads(dsp->fft_threads);
        fftwf_plan plan = fftwf_plan_many_dft(1, &n, rows,
                                              cir, NULL, 1, n,
                                              cir, NULL, 1, n,
                                              FFTW_BACKWARD, FFTW_ESTIMATE);
        if (plan == NULL) {
            fftwf_free(cir);
            free(row_valid);
            return NULL;
        }

        dsp->cir = cir;
        dsp->row_valid = row_valid;
        dsp->rows = rows;
        dsp->n = ifft_size;
        dsp->delay_plan = plan;
    }

    if (out_row_valid != NULL) {
        *out_row_valid = dsp->row_valid;
    }
    return dsp->cir;
}

static int ensure_doppler_stage(fh_radar_dsp_t* dsp, int rows, int taps) {
    if (dsp->slow != NULL && dsp->slow_rows == rows && dsp->slow_taps == taps) {
        return 1;
    }
    free_doppler_stage(dsp);

    fh_complex_t* slow = (fh_complex_t*)fftwf_malloc((size_t)rows * (size_t)taps * sizeof(fh_complex_t));
    if (slow == NULL) {
        return 0;
    }

    // One forward FFT per tap over its contiguous run of symbols.
    const int n = rows;
    fftwf_plan_with_nthreads(dsp->fft_threads);
    fftwf_plan plan = fftwf_plan_many_dft(1, &n, taps,
                                          slow, NULL, 1, n,
                                          slow, NULL, 1, n,
                                          FFTW_FORWARD, FFTW_ESTIMATE);
    if (plan == NULL) {
        fftwf_free(slow);
        return 0;
    }

    dsp->slow = slow;
    dsp->slow_rows = rows;
    dsp->slow_taps = taps;
    dsp->doppler_plan = plan;
    return 1;
}

static int ensure_tap_sum(fh_radar_dsp_t* dsp, int taps) {
    if (dsp->tap_sum != NULL && dsp->tap_sum_len >= taps) {
        return 1;
    }
    double* sum = (double*)malloc((size_t)taps * 2 * sizeof(double));
    if (sum == NULL) {
        return 0;
    }
    free(dsp->tap_sum);
    dsp->tap_sum = sum;
    dsp->tap_sum_len = taps;
    return 1;
}

const float* fh_radar_dsp_range_window(fh_radar_dsp_t* dsp, fh_radar_window_t type, int len) {
    if (dsp == NULL) {
        return NULL;
    }
    window_cache_get(&dsp->range_window, type, len);
    return dsp->range_window.weights;
}

// --- Stages ---------------------------------------------------------------

// Blanking and MTI over the leading 'taps' taps of every paired row; nothing
// past them is read again. Rows that did not pair stay all-zero: subtracting
// the mean from them would paint the clutter estimate into symbols that never
// measured anything.
static int cancel_taps(fh_radar_dsp_t* dsp, const fh_radar_config_t* cfg, int taps) {
    const int rows = dsp->rows;
    const int n = dsp->n;

    if (cfg->blank_taps_enabled && cfg->blank_taps > 0) {
        const int blank = (cfg->blank_taps < taps) ? cfg->blank_taps : taps;
        for (int row = 0; row < rows; row++) {
            if (dsp->row_valid[row]) {
                memset(dsp->cir + (size_t)row * (size_t)n, 0, (size_t)blank * sizeof(fh_complex_t));
            }
        }
    }

    if (!cfg->mti_enabled) {
        return 1;
    }

    int valid = 0;
    for (int row = 0; row < rows; row++) {
        valid += dsp->row_valid[row] ? 1 : 0;
    }
    if (valid == 0) {
        return 1;
    }
    if (!ensure_tap_sum(dsp, taps)) {
        return 0;
    }

    // Row-major passes: summing down the columns directly would stride the
    // whole matrix once per tap.
    double* sum = dsp->tap_sum;
    memset(sum, 0, (size_t)taps * 2 * sizeof(double));
    for (int row = 0; row < rows; row++) {
        if (!dsp->row_valid[row]) {
            continue;
        }
        // C11 lays a complex out as float[2], re then im.
        const float* x = (const float*)(dsp->cir + (size_t)row * (size_t)n);
        for (int k = 0; k < 2 * taps; k++) {
            sum[k] += (double)x[k];
        }
    }

    const double inv = 1.0 / (double)valid;
    for (int k = 0; k < 2 * taps; k++) {
        sum[k] *= inv;
    }

    for (int row = 0; row < rows; row++) {
        if (!dsp->row_valid[row]) {
            continue;
        }
        float* x = (float*)(dsp->cir + (size_t)row * (size_t)n);
        for (int k = 0; k < 2 * taps; k++) {
            x[k] -= (float)sum[k];
        }
    }
    return 1;
}

// |CIR|^2 of every tap of every row, scaled so a single path through a unit
// channel reads 1.
static void range_time_power(const fh_radar_dsp_t* dsp, double gain, float* dst) {
    const int rows = dsp->rows;
    const int n = dsp->n;
    const float g2 = (float)(1.0 / (gain * gain));

    for (int row = 0; row < rows; row++) {
        const float* x = (const float*)(dsp->cir + (size_t)row * (size_t)n);
        float* out = dst + (size_t)row * (size_t)n;
        for (int k = 0; k < n; k++) {
            const float re = x[2 * k];
            const float im = x[2 * k + 1];
            out[k] = (re * re + im * im) * g2;
        }
    }
}

// Slow-time FFT over each of the leading 'taps' taps, then |.|^2 into dst as
// rows x taps. Row r holds Doppler bin (rows / 2 - r): positive Doppler -
// approaching, for UL/DL - at the top, zero on the centre row, matching the
// visualizer's Doppler axis.
static int range_doppler_power(fh_radar_dsp_t* dsp, const fh_radar_config_t* cfg, int taps,
                               double range_gain, float* dst) {
    const int rows = dsp->rows;
    const int n = dsp->n;

    if (!ensure_doppler_stage(dsp, rows, taps)) {
        return 0;
    }
    window_cache_get(&dsp->doppler_window, cfg->doppler_window, rows);
    const float* wd = dsp->doppler_window.weights;
    const double doppler_gain = (dsp->doppler_window.sum > 0.0) ? dsp->doppler_window.sum : 1.0;

    // Transpose into tap-major order, applying the slow-time window on the way.
    for (int row = 0; row < rows; row++) {
        const float w = (wd != NULL) ? wd[row] : 1.0f;
        const float* x = (const float*)(dsp->cir + (size_t)row * (size_t)n);
        for (int k = 0; k < taps; k++) {
            float* y = (float*)(dsp->slow + (size_t)k * (size_t)rows + (size_t)row);
            y[0] = x[2 * k] * w;
            y[1] = x[2 * k + 1] * w;
        }
    }

    fftwf_execute(dsp->doppler_plan);

    const double gain = range_gain * doppler_gain;
    const float g2 = (float)(1.0 / (gain * gain));
    const int centre = rows / 2;
    for (int r = 0; r < rows; r++) {
        int bin = centre - r;
        if (bin < 0) {
            bin += rows;
        }
        float* out = dst + (size_t)r * (size_t)taps;
        for (int k = 0; k < taps; k++) {
            const float* y = (const float*)(dsp->slow + (size_t)k * (size_t)rows + (size_t)bin);
            out[k] = (y[0] * y[0] + y[1] * y[1]) * g2;
        }
    }
    return 1;
}

// --- Rolling mean ---------------------------------------------------------

static int floor_mod(int a, int b) {
    int m = a % b;
    return (m < 0) ? m + b : m;
}

// Empties the mean without touching its allocation.
static void persist_clear(radar_persist_t* ps) {
    if (ps->accum != NULL) {
        const size_t cells = (size_t)ps->key.rows * (size_t)ps->key.cols;
        for (size_t i = 0; i < cells; i++) {
            ps->accum[i] = PERSIST_EMPTY_CELL;
        }
    }
    if (ps->tags != NULL) {
        for (int i = 0; i < PERSIST_TAG_SLOTS; i++) {
            ps->tags[i] = INT_MIN;
        }
    }
    ps->has_folded = 0;
    ps->has_last_window = 0;
    ps->maps_folded = 0;
}

void fh_radar_dsp_reset_persistence(fh_radar_dsp_t* dsp) {
    if (dsp != NULL) {
        persist_clear(&dsp->persist);
    }
}

// Brings the mean in line with this pass's settings: kept as it is when they
// match what it was built under, started over - and reallocated if the shape
// moved - when they do not. Returns 0 when out of memory.
static int persist_sync(fh_radar_dsp_t* dsp, const fh_radar_config_t* cfg, const fh_radar_dsp_pass_t* pass,
                        int rows, int cols) {
    radar_persist_t* ps = &dsp->persist;

    radar_persist_key_t key;
    // Cleared first so the padding compares equal too.
    memset(&key, 0, sizeof(key));
    key.output = (int)cfg->output;
    key.rows = rows;
    key.cols = cols;
    key.ifft_size = cfg->ifft_size;
    key.used_re = pass->used_re;
    key.range_window = (int)cfg->range_window;
    key.doppler_window = (cfg->output == RADAR_OUT_RANGE_DOPPLER) ? (int)cfg->doppler_window : 0;
    key.mti_enabled = cfg->mti_enabled;
    key.blank_taps = cfg->blank_taps_enabled ? cfg->blank_taps : 0;
    key.epoch = pass->epoch;

    if (ps->have_key && ps->accum != NULL && memcmp(&key, &ps->key, sizeof(key)) == 0) {
        return 1;
    }

    const int same_shape = ps->accum != NULL && ps->key.rows == rows && ps->key.cols == cols;
    if (!same_shape) {
        free(ps->accum);
        ps->accum = (float*)malloc((size_t)rows * (size_t)cols * sizeof(float));
        if (ps->accum == NULL) {
            ps->have_key = 0;
            return 0;
        }
    }
    if (ps->tags == NULL) {
        ps->tags = (int*)malloc(PERSIST_TAG_SLOTS * sizeof(int));
        if (ps->tags == NULL) {
            ps->have_key = 0;
            return 0;
        }
    }
    ps->key = key;
    ps->have_key = 1;
    persist_clear(ps);
    return 1;
}

// Folds each paired symbol of the CIR matrix that is not in the mean yet into
// row (symbol % period), as power scaled like range_time_power. Unpaired rows
// are left untagged, so a symbol whose UL had not arrived yet is folded by a
// later pass instead of being lost.
static void persist_fold_range_time(fh_radar_dsp_t* dsp, const fh_radar_config_t* cfg, double gain,
                                    int first_symbol) {
    radar_persist_t* ps = &dsp->persist;
    const int n = dsp->n;
    const int period = ps->key.rows;
    const float g2 = (float)(1.0 / (gain * gain));
    const float alpha = 1.0f / (float)cfg->persist_n;

    for (int row = 0; row < dsp->rows; row++) {
        if (!dsp->row_valid[row]) {
            continue;
        }
        const int symbol = first_symbol + row;
        const int slot = floor_mod(symbol, PERSIST_TAG_SLOTS);
        if (ps->tags[slot] == symbol) {
            continue;
        }

        float* acc = ps->accum + (size_t)floor_mod(symbol, period) * (size_t)n;
        const float* x = (const float*)(dsp->cir + (size_t)row * (size_t)n);
        for (int k = 0; k < n; k++) {
            const float p = (x[2 * k] * x[2 * k] + x[2 * k + 1] * x[2 * k + 1]) * g2;
            acc[k] = (acc[k] < 0.0f) ? p : acc[k] + (p - acc[k]) * alpha;
        }

        ps->tags[slot] = symbol;
        if (!ps->has_folded) {
            ps->first_symbol = symbol;
            ps->last_symbol = symbol;
            ps->has_folded = 1;
        } else {
            if (symbol < ps->first_symbol) {
                ps->first_symbol = symbol;
            }
            if (symbol > ps->last_symbol) {
                ps->last_symbol = symbol;
            }
        }
    }
}

// Folds a whole Range-Doppler power map, unless it is the window folded last.
static void persist_fold_range_doppler(radar_persist_t* ps, const fh_radar_config_t* cfg, const float* map,
                                       int first_symbol) {
    if (ps->has_last_window && ps->last_window_first == first_symbol) {
        return;
    }
    const size_t cells = (size_t)ps->key.rows * (size_t)ps->key.cols;
    const float alpha = 1.0f / (float)cfg->persist_n;
    for (size_t i = 0; i < cells; i++) {
        const float p = map[i];
        ps->accum[i] = (ps->accum[i] < 0.0f) ? p : ps->accum[i] + (p - ps->accum[i]) * alpha;
    }
    ps->has_last_window = 1;
    ps->last_window_first = first_symbol;
    ps->maps_folded++;
}

static int persist_folds(const radar_persist_t* ps, int range_time) {
    if (range_time) {
        if (!ps->has_folded || ps->key.rows <= 0) {
            return 0;
        }
        return (ps->last_symbol - ps->first_symbol) / ps->key.rows + 1;
    }
    return ps->maps_folded;
}

// Turns a frame of powers into what is published: linear magnitude, or dB
// above a floor db_range below the frame's peak. Rolling-mean cells nothing
// has reached yet stay PERSIST_EMPTY_CELL, and the peak ignores them.
static void finish_frame(float* dst, size_t cells, int db_scale, float db_range) {
    if (!db_scale) {
        for (size_t i = 0; i < cells; i++) {
            // Written so a NaN lands on the empty marker too.
            dst[i] = (dst[i] >= 0.0f) ? sqrtf(dst[i]) : PERSIST_EMPTY_CELL;
        }
        return;
    }

    float peak = 0.0f;
    for (size_t i = 0; i < cells; i++) {
        if (dst[i] > peak) {
            peak = dst[i];
        }
    }

    // Everything at or under the floor is skipped before the log, which is
    // most of a frame after MTI and by far the expensive part of this loop.
    const float floor_power = peak * powf(10.0f, -db_range / 10.0f);
    const float inv_peak = (peak > 0.0f) ? 1.0f / peak : 0.0f;
    for (size_t i = 0; i < cells; i++) {
        const float p = dst[i];
        if (!(p >= 0.0f)) {
            dst[i] = PERSIST_EMPTY_CELL;
        } else if (peak > 0.0f && p > floor_power) {
            dst[i] = 10.0f * log10f(p * inv_peak) + db_range;
        } else {
            dst[i] = 0.0f;
        }
    }
}

int fh_radar_dsp_run(fh_radar_dsp_t* dsp, const fh_radar_config_t* cfg, fh_radar_dsp_pass_t* pass, float* dst) {
    if (dsp == NULL || cfg == NULL || pass == NULL || dst == NULL || dsp->cir == NULL || dsp->delay_plan == NULL) {
        return -1;
    }
    const int rows = dsp->rows;
    const int n = dsp->n;
    pass->persist_folds = 0;

    const int doppler = (cfg->output == RADAR_OUT_RANGE_DOPPLER);
    int taps = doppler ? cfg->cir_depth_taps : n;
    if (taps < 1) {
        taps = 1;
    }
    if (taps > n) {
        taps = n;
    }
    const size_t out_cells = (size_t)pass->out_rows * (size_t)taps;

    // Off means the next switch-on starts from nothing, not from whatever
    // was left over.
    int persist = cfg->persist_enabled;
    if (!persist) {
        dsp->persist.have_key = 0;
    } else if (!persist_sync(dsp, cfg, pass, doppler ? rows : pass->out_rows, taps)) {
        memset(dst, 0, out_cells * sizeof(float));
        return -1;
    }

    if (pass->paired_symbols > 0) {
        fftwf_execute(dsp->delay_plan);

        if (!cancel_taps(dsp, cfg, taps)) {
            memset(dst, 0, out_cells * sizeof(float));
            return -1;
        }

        // The same window the CFR stage applied this pass, so already cached.
        window_cache_get(&dsp->range_window, cfg->range_window, pass->used_re);
        const double range_gain = (dsp->range_window.sum > 0.0) ? dsp->range_window.sum : 1.0;

        if (doppler) {
            // The map goes into dst first either way: live it is the frame,
            // with the mean on it is what gets folded.
            if (!range_doppler_power(dsp, cfg, taps, range_gain, dst)) {
                memset(dst, 0, out_cells * sizeof(float));
                return -1;
            }
            if (persist) {
                persist_fold_range_doppler(&dsp->persist, cfg, dst, pass->first_symbol);
            }
        } else if (persist) {
            persist_fold_range_time(dsp, cfg, range_gain, pass->first_symbol);
        } else {
            range_time_power(dsp, range_gain, dst);
        }
    } else if (!persist) {
        // Nothing paired: the transforms would only turn zeros into zeros.
        memset(dst, 0, out_cells * sizeof(float));
    }

    if (persist) {
        memcpy(dst, dsp->persist.accum, out_cells * sizeof(float));
        pass->persist_folds = persist_folds(&dsp->persist, !doppler);
    }

    finish_frame(dst, out_cells, cfg->db_scale, cfg->db_range);
    return 0;
}
