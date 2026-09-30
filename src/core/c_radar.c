#include "c_radar.h"

#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- Defaults -------------------------------------------------------------
// The processing window matches the symbol ring's own default depth, so the
// radar spans everything the ring holds unless told otherwise.
#define RADAR_DEFAULT_TIME_SYMBOLS 1120
#define RADAR_DEFAULT_IFFT_SIZE 4096
#define RADAR_DEFAULT_CIR_DEPTH 128
#define RADAR_DEFAULT_BLANK_TAPS 2

// Hard ceiling on the window. Each of the two result buffers costs
// time_symbols * 3276 * sizeof(float), so this bounds the radar at roughly
// 54 MB per buffer rather than letting a typo allocate gigabytes.
#define RADAR_MAX_TIME_SYMBOLS 4096
#define RADAR_MIN_TIME_SYMBOLS 14

#define RADAR_MIN_IFFT_SIZE 512
#define RADAR_MAX_IFFT_SIZE 8192

#define RADAR_MAX_LAG_SYMBOLS 4096

// How long the worker idles between passes when nothing new has arrived.
#define RADAR_IDLE_SLEEP_MS 25

struct fh_radar {
    size_t fft_threads;

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
    int stop;

    // See fh_radar_stats::epoch. Kept outside stats because a reset wipes
    // those, and the count has to survive exactly the events it records.
    uint64_t epoch;

    pthread_t worker;
    int worker_started;
};

// --- Config ---------------------------------------------------------------

void fh_radar_config_defaults(fh_radar_config_t* cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->time_symbols = RADAR_DEFAULT_TIME_SYMBOLS;
    cfg->freq_start_re = 0;
    cfg->freq_end_re = FH_RADAR_RE_COUNT - 1;
    cfg->dl_ul_lag_symbols = 0;
    cfg->division = RADAR_DIV_UL_OVER_DL;
    cfg->ifft_size = RADAR_DEFAULT_IFFT_SIZE;
    cfg->cir_depth_taps = RADAR_DEFAULT_CIR_DEPTH;
    cfg->mti_enabled = 1;
    cfg->blank_taps_enabled = 0;
    cfg->blank_taps = RADAR_DEFAULT_BLANK_TAPS;
    cfg->output = RADAR_OUT_CFR;
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

    cfg->mti_enabled = cfg->mti_enabled ? 1 : 0;
    cfg->blank_taps_enabled = cfg->blank_taps_enabled ? 1 : 0;
    cfg->blank_taps = clamp_int(cfg->blank_taps, 0, cfg->ifft_size - 1);

    if (cfg->output != RADAR_OUT_RANGE_TIME && cfg->output != RADAR_OUT_RANGE_DOPPLER) {
        cfg->output = RADAR_OUT_CFR;
    }
}

// --- Ring access ----------------------------------------------------------

// Resolve the ring row holding an absolute symbol number, verifying the row's
// tag still matches so an evicted symbol is reported missing rather than read
// as whatever overwrote it. Mirrors ring_row_for_symbol in the visualizer.
static int ring_row_for_symbol(const fh_oran_memory_t* memory, int symbol_num, size_t* out_idx) {
    if (memory == NULL || memory->symbols == NULL) {
        return 0;
    }
    if (symbol_num < memory->symbol_lowest || symbol_num > memory->symbol_highest) {
        return 0;
    }
    size_t age = (size_t)(memory->symbol_highest - symbol_num);
    size_t idx = (memory->index_symbol_highest + memory->max_symbols - age) % memory->max_symbols;
    if (memory->symbols[idx] == NULL) {
        return 0;
    }
    if (memory->row_symbol_numbers != NULL && memory->row_symbol_numbers[idx] != symbol_num) {
        return 0;
    }
    *out_idx = idx;
    return 1;
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

// One full pass: pair each symbol of the window across the two rings, divide,
// and write |H| into dst. Resource elements outside the configured frequency
// range stay zero, which keeps the output on the same 3276-wide grid the
// heatmap and its RE axis already use.
//
// The rings are read without taking the decode-side data lock, matching how
// the renderer already samples the same memory every frame. A row torn by a
// concurrent write shows up as one noisy symbol, not as a crash.
static void compute_cfr(const fh_radar_config_t* cfg,
                        const fh_oran_memory_t* dl,
                        const fh_oran_memory_t* ul,
                        float* dst,
                        int rows,
                        int cols,
                        fh_radar_stats_t* stats) {
    memset(dst, 0, (size_t)rows * (size_t)cols * sizeof(float));

    stats->paired_symbols = 0;
    stats->window_symbols = rows;
    stats->window_first_symbol = 0;
    stats->window_first_valid = 0;
    stats->skipped_dl_empty = 0;
    stats->skipped_ul_missing = 0;
    stats->mean_cfr_magnitude = 0.0f;

    if (dl == NULL || ul == NULL) {
        return;
    }
    if (dl->symbol_highest < dl->symbol_lowest) {
        return; // nothing captured yet
    }

    // The window ends at the newest downlink symbol and runs backwards.
    const int newest = dl->symbol_highest;
    const int oldest = newest - rows + 1;

    // Lets the vertical axis label real symbol numbers rather than row indices.
    stats->window_first_symbol = oldest;
    stats->window_first_valid = 1;

    const int re_start = cfg->freq_start_re;
    const int re_end = cfg->freq_end_re;

    double mag_sum = 0.0;
    uint64_t mag_count = 0;

    for (int sym = oldest; sym <= newest; sym++) {
        // Row 0 is the oldest symbol in the window, matching the waterfall's
        // top-to-bottom time order.
        const int row = sym - oldest;

        size_t dl_idx = 0;
        if (!ring_row_for_symbol(dl, sym, &dl_idx)) {
            stats->skipped_dl_empty++;
            continue;
        }

        size_t ul_idx = 0;
        if (!ring_row_for_symbol(ul, sym + cfg->dl_ul_lag_symbols, &ul_idx)) {
            stats->skipped_ul_missing++;
            continue;
        }

        const fh_complex_t* dl_row = dl->symbols[dl_idx];
        const fh_complex_t* ul_row = ul->symbols[ul_idx];
        float* out_row = dst + (size_t)row * (size_t)cols;

        int occupied_in_row = 0;

        const int ul_over_dl = (cfg->division == RADAR_DIV_UL_OVER_DL);

        for (int re = re_start; re <= re_end; re++) {
            const float dl_p = complex_abs2(dl_row[re]);
            const float ul_p = complex_abs2(ul_row[re]);

            if (dl_p > 0.0f) {
                occupied_in_row = 1;
            }

            // |H| = |numerator| / |denominator|; the complex quotient itself is
            // only needed once the delay IFFT stage lands.
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
                mag_sum += (double)mag;
                mag_count++;
            }
        }

        if (!occupied_in_row) {
            // The rows paired, but the downlink carried nothing to divide by,
            // so this symbol contributes no channel estimate.
            memset(out_row, 0, (size_t)cols * sizeof(float));
            stats->skipped_dl_empty++;
            continue;
        }

        stats->paired_symbols++;
    }

    if (mag_count > 0) {
        stats->mean_cfr_magnitude = (float)(mag_sum / (double)mag_count);
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
        }

        const fh_radar_config_t cfg = r->cfg;
        const uint64_t epoch = r->epoch;
        fh_oran_memory_t* dl = r->dl;
        fh_oran_memory_t* ul = r->ul;

        int rows = cfg.time_symbols;
        // The window can never be deeper than the ring it reads from.
        if (dl != NULL && dl->max_symbols > 0 && (size_t)rows > dl->max_symbols) {
            rows = (int)dl->max_symbols;
        }
        const int cols = FH_RADAR_RE_COUNT;

        if (!ensure_back_buffer(r, rows, cols)) {
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

        if (cfg.output == RADAR_OUT_CFR) {
            compute_cfr(&cfg, dl, ul, back, rows, cols, &pass);
        } else {
            // Range-Time and Range-Doppler arrive with the transform stages.
            memset(back, 0, (size_t)rows * (size_t)cols * sizeof(float));
            pass.window_symbols = rows;
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
        r->rows = rows;
        r->cols = cols;

        r->back = old_front;
        r->back_rows = (old_front != NULL) ? old_rows : 0;
        r->back_cols = (old_front != NULL) ? old_cols : 0;

        r->have_result = 1;

        pass.refresh_count = r->stats.refresh_count + 1;
        r->stats = pass;

        // Idle until the next pass, but stay responsive to stop and reset.
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += (long)RADAR_IDLE_SLEEP_MS * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += deadline.tv_nsec / 1000000000L;
            deadline.tv_nsec %= 1000000000L;
        }
        if (!r->stop) {
            pthread_cond_timedwait(&r->cv, &r->lock, &deadline);
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

    if (pthread_mutex_init(&r->lock, NULL) != 0) {
        free(r);
        return NULL;
    }
    if (pthread_cond_init(&r->cv, NULL) != 0) {
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }

    if (pthread_create(&r->worker, NULL, radar_worker, r) != 0) {
        pthread_cond_destroy(&r->cv);
        pthread_mutex_destroy(&r->lock);
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
