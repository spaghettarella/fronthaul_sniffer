#include "opengl_visualizer.h"
#include "core/c_oran_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GLFW_INCLUDE_GLU
#include <GLFW/glfw3.h>

// Constants for heatmap layout
#define HEATMAP_WIDTH_PRBS 273
#define HEATMAP_SAMPLES_PER_PRB 12
#define HEATMAP_TEXTURE_WIDTH (HEATMAP_WIDTH_PRBS * HEATMAP_SAMPLES_PER_PRB)  // 3276 - all IQ samples
#define HEATMAP_DEFAULT_TEXTURE_HEIGHT 200  // Default display height (symbols)

#define PERSIST_DEFAULT_PERIOD 1120  // matches default memory size
#define PERSIST_DEFAULT_N 8
#define PERSIST_EMPTY_CELL (-1.0f)   // sentinel: cell never folded yet

#define CURVE_TRACE_DEFAULT_COUNT 8
#define CURVE_MAX_TRACES 256          // bounds per-frame vertex count

#define CURVE_MEAN_DEFAULT_PERIOD PERSIST_DEFAULT_PERIOD
#define CURVE_MEAN_DEFAULT_N 8

// Levels are % of an EMA of the same peak statistic being tested, so ~100%
// means "as strong as a typical recent symbol"; see the header for the full
// scale. The max is generous so a level can always be set high enough to
// stop firing entirely, whatever the signal's dynamic range.
#define TRIGGER_DEFAULT_LEVEL_PCT 50.0f
#define TRIGGER_MAX_LEVEL_PCT 10000.0f
#define TRIGGER_BASELINE_ALPHA 0.02f      // slow both rise and fall - a typical/baseline level, not a peak tracker
#define TRIGGER_NO_SYMBOL (-1)  // sentinel: no capture has fired yet (symbol numbers are always >= 0)
#define TRIGGER_DEFAULT_CAPTURE_DEPTH 200  // deliberately small and independent of Memory size; see header comment
#define TRIGGER_DEFAULT_RESET_LEVEL_PCT 30.0f  // opt-in hysteresis reset level; see visualizer_set_trigger_hysteresis_enabled
// Bounds how far back a single scan will look when catching up on a backlog,
// so a first arm (or a long stall) can't spend a whole frame scanning the
// entire ring. Normal per-frame arrival is far below this.
#define TRIGGER_MAX_SCAN_SYMBOLS 4000
// How many recent symbols a fresh arm samples to establish its baseline
// before it starts looking for crossings.
#define TRIGGER_SEED_SYMBOLS 256

// Margins reserved around the heatmap for axes. Left is wide enough for a
// frame label carrying both the absolute and the wrapped frame id; bottom
// fits two stacked rows of horizontal labels (PRB, then absolute RE).
#define PLOT_MARGIN_LEFT 96.0f
#define PLOT_MARGIN_BOTTOM 56.0f
// Tall enough for the vertical axis title, which is drawn above the plot.
#define PLOT_MARGIN_TOP 24.0f
#define PLOT_MARGIN_RIGHT 12.0f

// A tick is only worth drawing once its neighbours are this far apart, and
// only worth labelling at the larger spacing. Horizontal labels need more
// room than vertical ones because they run along the axis rather than
// stacking across it.
#define AXIS_MIN_TICK_PX 6.0f
#define AXIS_MIN_LABEL_PX_Y 30.0f
#define AXIS_MIN_LABEL_PX_X 60.0f

#define SYMBOLS_PER_SLOT 14

typedef struct opengl_visualizer {
    GLFWwindow* window;
    GLuint texture_id;
    int width;
    int height;
    int tex_height; // number of symbol rows in the texture
    int tex_max_height; // maximum texture height supported by GL
    int tex_requested_height; // CLI-configurable requested height before clamping
    float magnitude_scale_ema; // Smoothed normalization to reduce per-frame color flicker
    int magnitude_scale_initialized;
    int is_running;
    uint8_t* texture_data;  // RGBA framebuffer

    // Rolling-mean persistence mode
    int persist_enabled;
    int persist_period;             // averaging period, in symbols
    int persist_n;                  // EMA smoothing factor (approx. periods averaged)
    int persist_last_folded_symbol; // highest absolute symbol number already folded in
    int persist_has_last_folded;    // whether persist_last_folded_symbol is valid yet
    int persist_first_folded_symbol; // oldest symbol folded since the last clear, so the
    int persist_has_first_folded;    // number of accumulated periods can be measured
    float* persist_accum;           // persist_period * HEATMAP_TEXTURE_WIDTH magnitudes

    visualizer_view_mode_t view_mode;
    const fh_oran_memory_t* last_seen_memory; // detects stream switches to reset accumulators

    // Curve view: trailing raw traces (read live from the memory ring, no storage of their own)
    int curve_trace_enabled;
    int curve_trace_count;

    // Curve view: rolling-mean curve (own accumulator, independent of persist_accum)
    int curve_mean_enabled;
    visualizer_curve_mean_mode_t curve_mean_mode;
    int curve_mean_period;              // effective period driving the accumulator (1 when Running-average)
    int curve_mean_phase_locked_period; // remembers the user's period across a Running-average round-trip
    int curve_mean_n;
    int curve_mean_last_folded_symbol;
    int curve_mean_has_last_folded;
    float* curve_mean_accum;            // curve_mean_period * HEATMAP_TEXTURE_WIDTH magnitudes

    // Curve view's own magnitude scale, independent of the waterfall's so
    // switching view modes doesn't cause a jarring color/height jump.
    float curve_magnitude_scale_ema;
    int curve_magnitude_scale_initialized;

    // Waterfall trigger
    visualizer_trigger_mode_t trigger_mode;
    visualizer_trigger_edge_t trigger_edge;
    float trigger_level_pct;
    int trigger_freq_start;      // trigger search range, resource-element indices [start, end)
    int trigger_freq_end;
    int trigger_hysteresis_enabled; // opt-in: reset condition uses trigger_reset_level_pct instead of trigger_level_pct
    float trigger_reset_level_pct;
    int trigger_capture_depth;   // rows used by a triggered capture; independent of tex_height/Memory size
    int trigger_captured_symbol; // symbol currently centered/displayed; TRIGGER_NO_SYMBOL = none yet
    int trigger_capture_active;  // mid-capture: fired, still waiting for the post-trigger half to arrive
    int trigger_single_done;     // Single mode only: a capture completed and needs an explicit rearm
    int trigger_armed;           // ready to fire on the next qualifying crossing (edge/hysteresis state)
    int trigger_needs_seed;      // fresh arm: sample existing data for a baseline before detecting
    unsigned long trigger_fire_count;
    int trigger_last_checked_symbol;
    int trigger_has_last_checked;
    float trigger_baseline_ema;  // slow EMA of per-symbol peak - the trigger's own reference, decoupled
    int trigger_baseline_initialized; // from the display's fast-adapting magnitude_scale_ema
    float trigger_last_peak;           // last evaluated symbol's peak, for UI diagnostics
    float trigger_last_fire_threshold; // and the threshold it was tested against

    // What the rows currently on screen represent, captured while rendering
    // so the axis code doesn't have to re-derive which view branch ran.
    int axes_enabled;      // margins reserved for axes; off makes the view full-bleed
    int axis_valid;
    int axis_rows;         // texture rows spanning the plot height
    int axis_phase_mode;   // rows are phases within the averaging period, not absolute symbols
    int axis_curve_mode;   // rows are magnitude, not time
    int axis_row0_value;   // absolute symbol (or phase) shown by row 0
    int axis_period;       // averaging period, phase mode only

    // Permanent per-row capture of a triggered window, independent of the
    // live ring: once a row is seen it's locked in here and never re-read,
    // so a completed (or still-filling) capture survives ring eviction/pcap
    // loops that happen afterward. Sized to trigger_capture_depth.
    int* trigger_snapshot_symbol;      // absolute symbol number per row; TRIGGER_NO_SYMBOL = not captured yet
    float* trigger_snapshot_magnitude; // trigger_capture_depth * HEATMAP_TEXTURE_WIDTH magnitudes
} opengl_visualizer_t;

// Helper to extract real and imaginary parts from complex type
static void extract_iq(fh_complex_t val, float& i_val, float& q_val) {
#ifdef __cplusplus
    // C++ std::complex<float>
    i_val = val.real();
    q_val = val.imag();
#else
    // C99 complex float
    i_val = crealf(val);
    q_val = cimagf(val);
#endif
}

static void error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// Map magnitude to RGB heatmap color (0 = dark, max = white-hot)
static void magnitude_to_rgb(float magnitude, float max_magnitude, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (max_magnitude < 1e-6f) {
        *r = *g = *b = 0;
        return;
    }
    
    float norm = magnitude / max_magnitude;
    norm = (norm > 1.0f) ? 1.0f : norm;
    
    // Heatmap: dark blue → cyan → green → yellow → red → white
    if (norm < 0.25f) {
        // Dark blue to cyan
        float t = norm / 0.25f;
        *r = 0;
        *g = (uint8_t)(255 * t);
        *b = (uint8_t)(128 + 127 * t);
    } else if (norm < 0.5f) {
        // Cyan to green
        float t = (norm - 0.25f) / 0.25f;
        *r = 0;
        *g = 255;
        *b = (uint8_t)(255 * (1.0f - t));
    } else if (norm < 0.75f) {
        // Green to yellow
        float t = (norm - 0.5f) / 0.25f;
        *r = (uint8_t)(255 * t);
        *g = 255;
        *b = 0;
    } else {
        // Yellow to red to white
        float t = (norm - 0.75f) / 0.25f;
        *r = 255;
        *g = (uint8_t)(255 * (1.0f - t));
        *b = (uint8_t)(255 * t);
    }
}

// Fill the persistence accumulator with the "never folded" sentinel and
// forget which symbols have been folded, without touching allocation.
static void persist_clear_values(opengl_visualizer_t* vis) {
    if (!vis->persist_accum) {
        return;
    }
    size_t cells = (size_t)vis->persist_period * HEATMAP_TEXTURE_WIDTH;
    for (size_t i = 0; i < cells; i++) {
        vis->persist_accum[i] = PERSIST_EMPTY_CELL;
    }
    vis->persist_has_last_folded = 0;
    vis->persist_has_first_folded = 0;
}

// (Re)allocate the accumulator for the current persist_period and clear it.
static void persist_realloc(opengl_visualizer_t* vis) {
    if (vis->persist_accum) {
        free(vis->persist_accum);
        vis->persist_accum = NULL;
    }
    if (vis->persist_period <= 0) {
        return;
    }
    size_t cells = (size_t)vis->persist_period * HEATMAP_TEXTURE_WIDTH;
    vis->persist_accum = (float*)malloc(cells * sizeof(float));
    if (!vis->persist_accum) {
        return;
    }
    persist_clear_values(vis);
}

// Same shape as persist_clear_values/persist_realloc, operating on the
// curve view's own mean accumulator. Kept as separate, simple helpers
// rather than a shared/genericized version - each is a trivial malloc/fill.
static void curve_mean_clear_values(opengl_visualizer_t* vis) {
    if (!vis->curve_mean_accum) {
        return;
    }
    size_t cells = (size_t)vis->curve_mean_period * HEATMAP_TEXTURE_WIDTH;
    for (size_t i = 0; i < cells; i++) {
        vis->curve_mean_accum[i] = PERSIST_EMPTY_CELL;
    }
    vis->curve_mean_has_last_folded = 0;
}

static void curve_mean_realloc(opengl_visualizer_t* vis) {
    if (vis->curve_mean_accum) {
        free(vis->curve_mean_accum);
        vis->curve_mean_accum = NULL;
    }
    if (vis->curve_mean_period <= 0) {
        return;
    }
    size_t cells = (size_t)vis->curve_mean_period * HEATMAP_TEXTURE_WIDTH;
    vis->curve_mean_accum = (float*)malloc(cells * sizeof(float));
    if (!vis->curve_mean_accum) {
        return;
    }
    curve_mean_clear_values(vis);
}

void visualizer_set_persistence_enabled(opengl_visualizer_t* vis, int enabled) {
    if (!vis) {
        return;
    }
    vis->persist_enabled = enabled ? 1 : 0;
    if (vis->persist_enabled && !vis->persist_accum) {
        persist_realloc(vis);
    }
}

int visualizer_get_persistence_enabled(const opengl_visualizer_t* vis) {
    return vis ? vis->persist_enabled : 0;
}

void visualizer_set_persistence_period(opengl_visualizer_t* vis, int period_symbols) {
    if (!vis) {
        return;
    }
    if (period_symbols < 1) {
        period_symbols = 1;
    }
    if (period_symbols == vis->persist_period && vis->persist_accum) {
        return;
    }
    vis->persist_period = period_symbols;
    persist_realloc(vis);
}

int visualizer_get_persistence_period(const opengl_visualizer_t* vis) {
    return vis ? vis->persist_period : 0;
}

void visualizer_set_persistence_n(opengl_visualizer_t* vis, int n) {
    if (!vis) {
        return;
    }
    vis->persist_n = (n < 1) ? 1 : n;
}

int visualizer_get_persistence_n(const opengl_visualizer_t* vis) {
    return vis ? vis->persist_n : 0;
}

void visualizer_reset_persistence(opengl_visualizer_t* vis) {
    if (!vis) {
        return;
    }
    if (!vis->persist_accum) {
        persist_realloc(vis);
    } else {
        persist_clear_values(vis);
    }
}

void visualizer_set_view_mode(opengl_visualizer_t* vis, visualizer_view_mode_t mode) {
    if (!vis) {
        return;
    }
    vis->view_mode = mode;
}

visualizer_view_mode_t visualizer_get_view_mode(const opengl_visualizer_t* vis) {
    return vis ? vis->view_mode : VIEW_MODE_WATERFALL;
}

void visualizer_set_curve_trace_enabled(opengl_visualizer_t* vis, int enabled) {
    if (!vis) {
        return;
    }
    vis->curve_trace_enabled = enabled ? 1 : 0;
}

int visualizer_get_curve_trace_enabled(const opengl_visualizer_t* vis) {
    return vis ? vis->curve_trace_enabled : 0;
}

void visualizer_set_curve_trace_count(opengl_visualizer_t* vis, int count) {
    if (!vis) {
        return;
    }
    if (count < 1) {
        count = 1;
    }
    if (count > CURVE_MAX_TRACES) {
        count = CURVE_MAX_TRACES;
    }
    vis->curve_trace_count = count;
}

int visualizer_get_curve_trace_count(const opengl_visualizer_t* vis) {
    return vis ? vis->curve_trace_count : 0;
}

void visualizer_set_curve_mean_enabled(opengl_visualizer_t* vis, int enabled) {
    if (!vis) {
        return;
    }
    vis->curve_mean_enabled = enabled ? 1 : 0;
    if (vis->curve_mean_enabled && !vis->curve_mean_accum) {
        curve_mean_realloc(vis);
    }
}

int visualizer_get_curve_mean_enabled(const opengl_visualizer_t* vis) {
    return vis ? vis->curve_mean_enabled : 0;
}

void visualizer_set_curve_mean_period(opengl_visualizer_t* vis, int period_symbols) {
    if (!vis) {
        return;
    }
    if (period_symbols < 1) {
        period_symbols = 1;
    }
    if (vis->curve_mean_mode == CURVE_MEAN_MODE_PHASE_LOCKED) {
        vis->curve_mean_phase_locked_period = period_symbols;
    }
    if (period_symbols == vis->curve_mean_period && vis->curve_mean_accum) {
        return;
    }
    vis->curve_mean_period = period_symbols;
    curve_mean_realloc(vis);
}

int visualizer_get_curve_mean_period(const opengl_visualizer_t* vis) {
    return vis ? vis->curve_mean_period : 0;
}

void visualizer_set_curve_mean_mode(opengl_visualizer_t* vis, visualizer_curve_mean_mode_t mode) {
    if (!vis || mode == vis->curve_mean_mode) {
        return;
    }
    vis->curve_mean_mode = mode;
    int new_period = (mode == CURVE_MEAN_MODE_RUNNING_AVERAGE) ? 1 : vis->curve_mean_phase_locked_period;
    visualizer_set_curve_mean_period(vis, new_period);
}

visualizer_curve_mean_mode_t visualizer_get_curve_mean_mode(const opengl_visualizer_t* vis) {
    return vis ? vis->curve_mean_mode : CURVE_MEAN_MODE_PHASE_LOCKED;
}

void visualizer_set_curve_mean_n(opengl_visualizer_t* vis, int n) {
    if (!vis) {
        return;
    }
    vis->curve_mean_n = (n < 1) ? 1 : n;
}

int visualizer_get_curve_mean_n(const opengl_visualizer_t* vis) {
    return vis ? vis->curve_mean_n : 0;
}

void visualizer_reset_curve_mean(opengl_visualizer_t* vis) {
    if (!vis) {
        return;
    }
    if (!vis->curve_mean_accum) {
        curve_mean_realloc(vis);
    } else {
        curve_mean_clear_values(vis);
    }
}

// Marks every row of the trigger snapshot as "not captured yet" without
// touching allocation.
static void trigger_snapshot_clear_values(opengl_visualizer_t* vis) {
    if (!vis->trigger_snapshot_symbol) {
        return;
    }
    for (int i = 0; i < vis->trigger_capture_depth; i++) {
        vis->trigger_snapshot_symbol[i] = TRIGGER_NO_SYMBOL;
    }
}

// (Re)allocates the snapshot for the current trigger_capture_depth and
// clears it. Both buffers are always allocated/freed together.
static void trigger_snapshot_realloc(opengl_visualizer_t* vis) {
    if (vis->trigger_snapshot_symbol) {
        free(vis->trigger_snapshot_symbol);
        vis->trigger_snapshot_symbol = NULL;
    }
    if (vis->trigger_snapshot_magnitude) {
        free(vis->trigger_snapshot_magnitude);
        vis->trigger_snapshot_magnitude = NULL;
    }
    if (vis->trigger_capture_depth <= 0) {
        return;
    }
    vis->trigger_snapshot_symbol = (int*)malloc((size_t)vis->trigger_capture_depth * sizeof(int));
    vis->trigger_snapshot_magnitude =
        (float*)malloc((size_t)vis->trigger_capture_depth * HEATMAP_TEXTURE_WIDTH * sizeof(float));
    if (!vis->trigger_snapshot_symbol || !vis->trigger_snapshot_magnitude) {
        if (vis->trigger_snapshot_symbol) {
            free(vis->trigger_snapshot_symbol);
            vis->trigger_snapshot_symbol = NULL;
        }
        if (vis->trigger_snapshot_magnitude) {
            free(vis->trigger_snapshot_magnitude);
            vis->trigger_snapshot_magnitude = NULL;
        }
        return;
    }
    trigger_snapshot_clear_values(vis);
}

// Resets to "waiting for the first crossing" - used whenever the trigger
// mode changes or the stream switches, so switching never reuses a stale
// captured symbol or a baseline measured against a different stream.
static void trigger_reset_state(opengl_visualizer_t* vis) {
    vis->trigger_captured_symbol = TRIGGER_NO_SYMBOL;
    vis->trigger_capture_active = 0;
    vis->trigger_single_done = 0;
    vis->trigger_armed = 1;
    vis->trigger_needs_seed = 1;
    vis->trigger_fire_count = 0;
    vis->trigger_has_last_checked = 0;
    vis->trigger_baseline_initialized = 0;
    trigger_snapshot_clear_values(vis);
}

void visualizer_set_trigger_mode(opengl_visualizer_t* vis, visualizer_trigger_mode_t mode) {
    if (!vis || mode == vis->trigger_mode) {
        return;
    }
    vis->trigger_mode = mode;
    trigger_reset_state(vis);
}

visualizer_trigger_mode_t visualizer_get_trigger_mode(const opengl_visualizer_t* vis) {
    return vis ? vis->trigger_mode : TRIGGER_OFF;
}

void visualizer_set_trigger_level_pct(opengl_visualizer_t* vis, float level_pct) {
    if (!vis) {
        return;
    }
    if (level_pct < 0.0f) {
        level_pct = 0.0f;
    }
    if (level_pct > TRIGGER_MAX_LEVEL_PCT) {
        level_pct = TRIGGER_MAX_LEVEL_PCT;
    }
    vis->trigger_level_pct = level_pct;
}

float visualizer_get_trigger_level_pct(const opengl_visualizer_t* vis) {
    return vis ? vis->trigger_level_pct : 0.0f;
}

void visualizer_get_trigger_levels(const opengl_visualizer_t* vis,
                                   float* out_last_peak,
                                   float* out_baseline,
                                   float* out_fire_threshold) {
    if (out_last_peak) {
        *out_last_peak = vis ? vis->trigger_last_peak : 0.0f;
    }
    if (out_baseline) {
        *out_baseline = (vis && vis->trigger_baseline_initialized) ? vis->trigger_baseline_ema : 0.0f;
    }
    if (out_fire_threshold) {
        *out_fire_threshold = vis ? vis->trigger_last_fire_threshold : 0.0f;
    }
}

void visualizer_set_trigger_freq_range(opengl_visualizer_t* vis, int re_start, int re_end) {
    if (!vis) {
        return;
    }
    if (re_start < 0) {
        re_start = 0;
    }
    if (re_end > HEATMAP_TEXTURE_WIDTH) {
        re_end = HEATMAP_TEXTURE_WIDTH;
    }
    if (re_end <= re_start) {
        re_end = re_start + 1;
    }
    if (re_end > HEATMAP_TEXTURE_WIDTH) {
        // re_start must have been at/past the top edge; pull both back.
        re_end = HEATMAP_TEXTURE_WIDTH;
        re_start = re_end - 1;
    }
    vis->trigger_freq_start = re_start;
    vis->trigger_freq_end = re_end;
}

int visualizer_get_trigger_freq_start(const opengl_visualizer_t* vis) {
    return vis ? vis->trigger_freq_start : 0;
}

int visualizer_get_trigger_freq_end(const opengl_visualizer_t* vis) {
    return vis ? vis->trigger_freq_end : 0;
}

void visualizer_set_trigger_edge(opengl_visualizer_t* vis, visualizer_trigger_edge_t edge) {
    if (!vis) {
        return;
    }
    vis->trigger_edge = edge;
}

visualizer_trigger_edge_t visualizer_get_trigger_edge(const opengl_visualizer_t* vis) {
    return vis ? vis->trigger_edge : TRIGGER_EDGE_RISING;
}

void visualizer_set_trigger_hysteresis_enabled(opengl_visualizer_t* vis, int enabled) {
    if (!vis) {
        return;
    }
    vis->trigger_hysteresis_enabled = enabled ? 1 : 0;
}

int visualizer_get_trigger_hysteresis_enabled(const opengl_visualizer_t* vis) {
    return vis ? vis->trigger_hysteresis_enabled : 0;
}

void visualizer_set_trigger_reset_level_pct(opengl_visualizer_t* vis, float level_pct) {
    if (!vis) {
        return;
    }
    if (level_pct < 0.0f) {
        level_pct = 0.0f;
    }
    if (level_pct > TRIGGER_MAX_LEVEL_PCT) {
        level_pct = TRIGGER_MAX_LEVEL_PCT;
    }
    vis->trigger_reset_level_pct = level_pct;
}

float visualizer_get_trigger_reset_level_pct(const opengl_visualizer_t* vis) {
    return vis ? vis->trigger_reset_level_pct : 0.0f;
}

void visualizer_set_trigger_capture_depth(opengl_visualizer_t* vis, int depth_symbols) {
    if (!vis) {
        return;
    }
    if (depth_symbols < 2) {
        depth_symbols = 2;
    }
    if (depth_symbols == vis->trigger_capture_depth && vis->trigger_snapshot_symbol) {
        return;
    }
    vis->trigger_capture_depth = depth_symbols;
    trigger_snapshot_realloc(vis);
}

int visualizer_get_trigger_capture_depth(const opengl_visualizer_t* vis) {
    return vis ? vis->trigger_capture_depth : 0;
}

void visualizer_trigger_rearm(opengl_visualizer_t* vis) {
    if (!vis || vis->trigger_mode == TRIGGER_OFF) {
        return;
    }
    vis->trigger_single_done = 0;
}

static int persist_periods_elapsed(const opengl_visualizer_t* vis);

visualizer_trigger_status_t visualizer_get_trigger_status(const opengl_visualizer_t* vis) {
    if (!vis || vis->trigger_mode == TRIGGER_OFF) {
        return TRIGGER_STATUS_OFF;
    }
    if (vis->persist_enabled && persist_periods_elapsed(vis) < vis->persist_n) {
        return TRIGGER_STATUS_WARMING_UP;
    }
    if (vis->trigger_capture_active) {
        return TRIGGER_STATUS_CAPTURING;
    }
    if (vis->trigger_mode == TRIGGER_SINGLE && vis->trigger_single_done) {
        return TRIGGER_STATUS_HOLDING;
    }
    return vis->trigger_armed ? TRIGGER_STATUS_ARMED : TRIGGER_STATUS_WAIT_RESET;
}

unsigned long visualizer_get_trigger_fire_count(const opengl_visualizer_t* vis) {
    return vis ? vis->trigger_fire_count : 0UL;
}

int visualizer_get_trigger_mean_progress(const opengl_visualizer_t* vis,
                                         int* out_periods_elapsed,
                                         int* out_periods_required) {
    if (!vis || !vis->persist_enabled) {
        if (out_periods_elapsed) {
            *out_periods_elapsed = 0;
        }
        if (out_periods_required) {
            *out_periods_required = 0;
        }
        return 0;
    }
    if (out_periods_elapsed) {
        *out_periods_elapsed = persist_periods_elapsed(vis);
    }
    if (out_periods_required) {
        *out_periods_required = vis->persist_n;
    }
    return 1;
}

opengl_visualizer_t* visualizer_create(int width, int height, int texture_height) {
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return NULL;
    }
    
    glfwSetErrorCallback(error_callback);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    
    GLFWwindow* window = glfwCreateWindow(width, height, "ORAN Fronthaul Waterfall", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return NULL;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // Enable vsync
    
    opengl_visualizer_t* vis = (opengl_visualizer_t*)malloc(sizeof(*vis));
    if (!vis) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return NULL;
    }
    
    memset(vis, 0, sizeof(*vis));
    vis->window = window;
    vis->width = width;
    vis->height = height;
    vis->tex_requested_height = (texture_height > 0) ? texture_height : HEATMAP_DEFAULT_TEXTURE_HEIGHT;
        // Query GL maximum texture size (applies to width/height); use it to bound our texture height
        GLint max_tex_size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);
        if (max_tex_size <= 0) {
            max_tex_size = 4096; // fallback conservative default
        }
        vis->tex_height = vis->tex_requested_height;
        if (vis->tex_height > max_tex_size) vis->tex_height = max_tex_size;
        vis->tex_max_height = max_tex_size;
        vis->magnitude_scale_ema = 1e-6f;
        vis->magnitude_scale_initialized = 0;
    vis->is_running = 1;
    vis->persist_enabled = 0;
    vis->persist_period = PERSIST_DEFAULT_PERIOD;
    vis->persist_n = PERSIST_DEFAULT_N;
    vis->persist_has_last_folded = 0;
    vis->persist_has_first_folded = 0;
    vis->persist_accum = NULL;

    vis->view_mode = VIEW_MODE_WATERFALL;
    vis->last_seen_memory = NULL;
    vis->curve_trace_enabled = 0;
    vis->curve_trace_count = CURVE_TRACE_DEFAULT_COUNT;
    vis->curve_mean_enabled = 0;
    vis->curve_mean_mode = CURVE_MEAN_MODE_PHASE_LOCKED;
    vis->curve_mean_period = CURVE_MEAN_DEFAULT_PERIOD;
    vis->curve_mean_phase_locked_period = CURVE_MEAN_DEFAULT_PERIOD;
    vis->curve_mean_n = CURVE_MEAN_DEFAULT_N;
    vis->curve_mean_has_last_folded = 0;
    vis->curve_mean_accum = NULL;
    vis->curve_magnitude_scale_ema = 1e-6f;
    vis->curve_magnitude_scale_initialized = 0;

    vis->trigger_mode = TRIGGER_OFF;
    vis->trigger_edge = TRIGGER_EDGE_RISING;
    vis->trigger_level_pct = TRIGGER_DEFAULT_LEVEL_PCT;
    vis->trigger_freq_start = 0;
    vis->trigger_freq_end = HEATMAP_TEXTURE_WIDTH;
    vis->trigger_hysteresis_enabled = 0;
    vis->trigger_reset_level_pct = TRIGGER_DEFAULT_RESET_LEVEL_PCT;
    vis->trigger_capture_depth = TRIGGER_DEFAULT_CAPTURE_DEPTH;
    vis->trigger_captured_symbol = TRIGGER_NO_SYMBOL;
    vis->trigger_capture_active = 0;
    vis->trigger_single_done = 0;
    vis->trigger_armed = 1;
    vis->trigger_needs_seed = 1;
    vis->trigger_fire_count = 0;
    vis->trigger_has_last_checked = 0;
    vis->trigger_baseline_ema = 1e-6f;
    vis->trigger_baseline_initialized = 0;
    vis->trigger_last_peak = 0.0f;
    vis->trigger_last_fire_threshold = 0.0f;
    vis->axes_enabled = 1;
    vis->axis_valid = 0;
    vis->axis_rows = 0;
    vis->axis_phase_mode = 0;
    vis->axis_curve_mode = 0;
    vis->axis_row0_value = 0;
    vis->axis_period = 0;
    vis->trigger_snapshot_symbol = NULL;
    vis->trigger_snapshot_magnitude = NULL;
    trigger_snapshot_realloc(vis);

    // Allocate texture buffer (width x tex_height)
    vis->texture_data = (uint8_t*)calloc(HEATMAP_TEXTURE_WIDTH * vis->tex_height * 4, sizeof(uint8_t));
    if (!vis->texture_data) {
        free(vis);
        glfwDestroyWindow(window);
        glfwTerminate();
        return NULL;
    }
    
    // Create OpenGL texture
    glGenTextures(1, &vis->texture_id);
    glBindTexture(GL_TEXTURE_2D, vis->texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, HEATMAP_TEXTURE_WIDTH, vis->tex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    
    // Set up OpenGL state for 2D rendering
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    return vis;
}

// Resolve the ring-buffer row for an absolute symbol number, verifying its
// tag still matches (hasn't been evicted/overwritten by ring wraparound).
// Returns 1 and sets *out_idx on success, 0 if unavailable/out of range.
static int ring_row_for_symbol(const fh_oran_memory_t* memory, int symbol_num, size_t* out_idx) {
    if (symbol_num < memory->symbol_lowest || symbol_num > memory->symbol_highest) {
        return 0;
    }
    int age = memory->symbol_highest - symbol_num;
    size_t idx = (memory->index_symbol_highest + memory->max_symbols - (size_t)age) % memory->max_symbols;
    if (memory->symbols[idx] == NULL) {
        return 0;
    }
    if (memory->row_symbol_numbers != NULL && memory->row_symbol_numbers[idx] != symbol_num) {
        return 0;
    }
    *out_idx = idx;
    return 1;
}

// Mutable state + settings for one EMA fold accumulator. Lets Waterfall
// persistence and Curve mean share the fold/phase/EMA logic below without
// duplicating it, while each keeps its own accumulator buffer/settings.
struct ema_fold_target {
    float* accum;
    int period;
    int n;
    int last_folded_symbol; // in/out
    int has_last_folded;    // in/out
    int first_folded_symbol; // in/out - oldest symbol ever folded, for measuring
    int has_first_folded;    // in/out   how many periods have accumulated
};

// Fold newly-arrived symbols (since the last call) into an accumulator,
// keyed by phase = absolute_symbol_number % period. Each symbol is folded
// exactly once, regardless of partial writes it may have received before
// settling in the ring.
static void ema_fold_new_symbols(struct ema_fold_target* t, const fh_oran_memory_t* memory) {
    if (!t->accum || t->period <= 0) {
        return;
    }
    if (memory->symbol_highest < memory->symbol_lowest) {
        return;  // no data yet
    }

    int start_symbol;
    if (!t->has_last_folded) {
        start_symbol = memory->symbol_lowest;
    } else {
        start_symbol = t->last_folded_symbol + 1;
        if (start_symbol < memory->symbol_lowest) {
            // Gap since we last folded is bigger than the ring itself;
            // whatever fell out of the window is gone, start from what remains.
            start_symbol = memory->symbol_lowest;
        }
    }

    const float alpha = 1.0f / (float)t->n;

    for (int symbol_num = start_symbol; symbol_num <= memory->symbol_highest; symbol_num++) {
        size_t idx;
        if (!ring_row_for_symbol(memory, symbol_num, &idx)) {
            continue;
        }

        if (!t->has_first_folded) {
            t->first_folded_symbol = symbol_num;
            t->has_first_folded = 1;
        }

        int phase = symbol_num % t->period;
        if (phase < 0) {
            phase += t->period;
        }
        float* accum_row = t->accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH;

        for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
            float i_val, q_val;
            extract_iq(memory->symbols[idx][col], i_val, q_val);
            float magnitude = sqrtf(i_val * i_val + q_val * q_val);

            if (accum_row[col] < 0.0f) {
                accum_row[col] = magnitude;  // first sample ever seen at this phase
            } else {
                accum_row[col] += (magnitude - accum_row[col]) * alpha;
            }
        }
    }

    t->last_folded_symbol = memory->symbol_highest;
    t->has_last_folded = 1;
}

static void persist_fold_new_symbols(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    struct ema_fold_target t = { vis->persist_accum, vis->persist_period, vis->persist_n,
                                  vis->persist_last_folded_symbol, vis->persist_has_last_folded,
                                  vis->persist_first_folded_symbol, vis->persist_has_first_folded };
    ema_fold_new_symbols(&t, memory);
    vis->persist_last_folded_symbol = t.last_folded_symbol;
    vis->persist_has_last_folded = t.has_last_folded;
    vis->persist_first_folded_symbol = t.first_folded_symbol;
    vis->persist_has_first_folded = t.has_first_folded;
}

// How many averaging periods have accumulated since the rolling mean was
// last cleared - the basis for the trigger's warm-up gate, so it doesn't
// fire on an average that has only seen one or two passes.
static int persist_periods_elapsed(const opengl_visualizer_t* vis) {
    if (!vis->persist_has_first_folded || !vis->persist_has_last_folded || vis->persist_period <= 0) {
        return 0;
    }
    const int span = vis->persist_last_folded_symbol - vis->persist_first_folded_symbol;
    if (span < 0) {
        return 0;
    }
    return (span / vis->persist_period) + 1;
}

static void curve_mean_fold_new_symbols(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    struct ema_fold_target t = { vis->curve_mean_accum, vis->curve_mean_period, vis->curve_mean_n,
                                  vis->curve_mean_last_folded_symbol, vis->curve_mean_has_last_folded,
                                  0, 0 };
    ema_fold_new_symbols(&t, memory);
    vis->curve_mean_last_folded_symbol = t.last_folded_symbol;
    vis->curve_mean_has_last_folded = t.has_last_folded;
}

#define CURVE_TRACE_MIN_ALPHA 0.08f

// Draw the Curve view: a 2D plot of magnitude vs. resource-element index for
// the newest symbol (or, with trailing-trace persistence on, the last N
// symbols read live from the ring, older ones fading out), plus an optional
// bold rolling-mean curve. Replaces the heatmap quad draw when view_mode ==
// VIEW_MODE_CURVE; called from visualizer_update() after glClear.
static void draw_curve_view(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis || !memory || !memory->symbols) {
        return;
    }
    if (memory->symbol_highest < memory->symbol_lowest) {
        return;  // no data yet
    }

    if (vis->curve_mean_enabled) {
        curve_mean_fold_new_symbols(vis, memory);
    }

    int trace_count = vis->curve_trace_enabled ? vis->curve_trace_count : 1;
    if (trace_count < 1) {
        trace_count = 1;
    }
    if (trace_count > CURVE_MAX_TRACES) {
        trace_count = CURVE_MAX_TRACES;
    }

    // When trailing and mean are both on, the trail itself is built from the
    // mean accumulator's recent phase history - "a trail of averages" - in
    // place of raw traces plus one separate static mean line. With trailing
    // off, mean still shows as a single current line as before.
    const int show_mean_trail = vis->curve_mean_enabled && vis->curve_trace_enabled && vis->curve_mean_accum != NULL;
    const int show_single_mean_line = vis->curve_mean_enabled && !vis->curve_trace_enabled && vis->curve_mean_accum != NULL;
    const int mean_period = vis->curve_mean_period > 0 ? vis->curve_mean_period : 1;

    size_t trace_idx[CURVE_MAX_TRACES];             // raw ring rows, when !show_mean_trail
    const float* trail_mean_rows[CURVE_MAX_TRACES];  // mean accumulator rows, when show_mean_trail
    int resolved = 0;

    if (show_mean_trail) {
        // Each age steps back through the phase the mean accumulator held at
        // that point - e.g. age 0 is the current phase, age 1 the previous
        // symbol's phase, etc. Not bounded by the raw ring, since the mean
        // accumulator retains state independent of ring depth. In Running
        // Average mode (period forced to 1) every age maps to the same row,
        // so the "trail" degenerates to one visible line - expected, since
        // that mode has no history to show a trail of.
        for (int age = 0; age < trace_count; age++) {
            int phase = (memory->symbol_highest - age) % mean_period;
            if (phase < 0) {
                phase += mean_period;
            }
            trail_mean_rows[resolved++] = vis->curve_mean_accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH;
        }
    } else {
        for (int age = 0; age < trace_count; age++) {
            size_t idx;
            if (!ring_row_for_symbol(memory, memory->symbol_highest - age, &idx)) {
                break;
            }
            trace_idx[resolved++] = idx;
        }
    }

    const float* single_mean_row = NULL;
    if (show_single_mean_line) {
        int phase = memory->symbol_highest % mean_period;
        if (phase < 0) {
            phase += mean_period;
        }
        single_mean_row = vis->curve_mean_accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH;
    }

    if (resolved == 0 && single_mean_row == NULL) {
        return;
    }

    // Pass 1: max magnitude across everything about to be drawn, for scaling.
    float max_magnitude = 1e-9f;
    for (int t = 0; t < resolved; t++) {
        for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
            float magnitude;
            if (show_mean_trail) {
                magnitude = trail_mean_rows[t][col];
                if (magnitude < 0.0f) {
                    magnitude = 0.0f;  // never-folded sentinel
                }
            } else {
                float i_val, q_val;
                extract_iq(memory->symbols[trace_idx[t]][col], i_val, q_val);
                magnitude = sqrtf(i_val * i_val + q_val * q_val);
            }
            if (magnitude > max_magnitude) {
                max_magnitude = magnitude;
            }
        }
    }
    if (single_mean_row != NULL) {
        for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
            if (single_mean_row[col] > max_magnitude) {
                max_magnitude = single_mean_row[col];
            }
        }
    }

    // Smooth magnitude scaling, same rise/fall EMA convention as the
    // waterfall's magnitude_scale_ema, but on the curve's own independent field.
    if (!vis->curve_magnitude_scale_initialized) {
        vis->curve_magnitude_scale_ema = max_magnitude;
        vis->curve_magnitude_scale_initialized = 1;
    } else {
        const float alpha_rise = 0.35f;
        const float alpha_fall = 0.08f;
        const float alpha = (max_magnitude > vis->curve_magnitude_scale_ema) ? alpha_rise : alpha_fall;
        vis->curve_magnitude_scale_ema = alpha * max_magnitude + (1.0f - alpha) * vis->curve_magnitude_scale_ema;
    }
    const float render_scale = (vis->curve_magnitude_scale_ema > 1e-6f) ? vis->curve_magnitude_scale_ema : 1e-6f;

    // Pass 2: draw. glLineWidth persists across frames (unlike blend/texture
    // enable state, which ImGui's own glPushAttrib/glPopAttrib restores), so
    // it is set explicitly before every glBegin(GL_LINE_STRIP) call below.
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 1.0f, plot_h = 1.0f;
    visualizer_get_plot_rect(vis, &plot_x, &plot_y, &plot_w, &plot_h);

    const float x_step = plot_w / (float)(HEATMAP_TEXTURE_WIDTH - 1);
    const float margin_y = plot_y + 0.05f * plot_h;
    const float usable_y = plot_h * 0.90f;

    vis->axis_valid = 1;
    vis->axis_rows = vis->tex_height > 0 ? vis->tex_height : 1;
    vis->axis_phase_mode = 0;
    vis->axis_curve_mode = 1;
    vis->axis_period = 0;

    // Width now runs the opposite way from brightness/opacity: the newest
    // trace is the sharpest (thinnest) line, older ones thicken as they fade,
    // so the highlighted newest line reads as a crisp line on top of a thick,
    // soft, dissolving stack of afterglow beneath it. Driver-dependent aliased
    // line width maximums vary; if 10px isn't supported it'll silently clamp
    // to whatever the driver's max is.
    const float TRACE_NEWEST_WIDTH = 1.0f;
    const float TRACE_OLDEST_WIDTH = 10.0f;

    // Single phosphor-style hue, like an oscilloscope's persistence/afterglow
    // mode: all traces share one color; only its brightness and opacity decay
    // with age, so older traces read as dissolving into the black background
    // rather than shifting through a rainbow. The newest trace is the
    // exception - drawn in red instead, so the live line stands out from its
    // own afterglow.
    const float TRACE_HUE_R = 0.35f;
    const float TRACE_HUE_G = 1.00f;
    const float TRACE_HUE_B = 0.50f;
    const float TRACE_MIN_BRIGHTNESS = 0.12f;
    const float TRACE_NEWEST_R = 1.0f;
    const float TRACE_NEWEST_G = 0.0f;
    const float TRACE_NEWEST_B = 0.0f;

    // Oldest-to-newest so the newest trace's blend ends up "on top" where traces overlap.
    for (int t = resolved - 1; t >= 0; t--) {
        int age = t;
        float freshness = 1.0f - (float)age / (float)(trace_count > 1 ? trace_count - 1 : 1);
        float alpha = CURVE_TRACE_MIN_ALPHA + (1.0f - CURVE_TRACE_MIN_ALPHA) * freshness;
        float line_width = TRACE_NEWEST_WIDTH + (TRACE_OLDEST_WIDTH - TRACE_NEWEST_WIDTH) * (1.0f - freshness);

        float r, g, b;
        if (age == 0) {
            r = TRACE_NEWEST_R;
            g = TRACE_NEWEST_G;
            b = TRACE_NEWEST_B;
        } else {
            float brightness = TRACE_MIN_BRIGHTNESS + (1.0f - TRACE_MIN_BRIGHTNESS) * freshness;
            r = TRACE_HUE_R * brightness;
            g = TRACE_HUE_G * brightness;
            b = TRACE_HUE_B * brightness;
        }

        glLineWidth(line_width);
        glColor4f(r, g, b, alpha);
        glBegin(GL_LINE_STRIP);
        for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
            float magnitude;
            if (show_mean_trail) {
                magnitude = trail_mean_rows[t][col];
                if (magnitude < 0.0f) {
                    magnitude = 0.0f;  // never-folded sentinel
                }
            } else {
                float i_val, q_val;
                extract_iq(memory->symbols[trace_idx[t]][col], i_val, q_val);
                magnitude = sqrtf(i_val * i_val + q_val * q_val);
            }
            float norm = magnitude / render_scale;
            if (norm > 1.0f) {
                norm = 1.0f;
            }
            glVertex2f(plot_x + (float)col * x_step, margin_y + (1.0f - norm) * usable_y);
        }
        glEnd();
    }

    if (single_mean_row != NULL) {
        glLineWidth(2.5f);
        glColor4f(1.00f, 0.85f, 0.25f, 1.0f);
        glBegin(GL_LINE_STRIP);
        for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
            float magnitude = single_mean_row[col];
            if (magnitude < 0.0f) {
                magnitude = 0.0f;  // never-folded sentinel
            }
            float norm = magnitude / render_scale;
            if (norm > 1.0f) {
                norm = 1.0f;
            }
            glVertex2f(plot_x + (float)col * x_step, margin_y + (1.0f - norm) * usable_y);
        }
        glEnd();
    }

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glDisable(GL_BLEND);
}

// Scans newly-arrived symbols since the last check for a trigger crossing,
// searching only the configured resource-element range. Each symbol updates
// a slow baseline EMA (mean magnitude within that range) that the trigger
// level is measured against - deliberately decoupled from the waterfall's
// fast-adapting display scale, which would otherwise make "crossing the
// threshold" almost always true (see visualizer_set_trigger_level_pct).
// Starts a new capture on a crossing in the configured direction, unless one
// is already active, or (Single mode) the previous capture finished and
// hasn't been rearmed yet - see visualizer_trigger_rearm(). Per
// visualizer_trigger_edge_t, once fired it won't fire again until the signal
// crosses back the other way first, so a sustained high (rising) or
// sustained low (falling) doesn't repeatedly retrigger.
static float symbol_peak_in_range(const fh_oran_memory_t* memory, size_t idx, int col_start, int col_end) {
    float peak = 0.0f;
    for (int col = col_start; col < col_end; col++) {
        float i_val, q_val;
        extract_iq(memory->symbols[idx][col], i_val, q_val);
        const float magnitude = sqrtf(i_val * i_val + q_val * q_val);
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

// Peak of one rolling-mean accumulator row, ignoring cells that have never
// received a symbol (the -1 sentinel).
static float accum_row_peak_in_range(const float* row, int col_start, int col_end) {
    float peak = 0.0f;
    for (int col = col_start; col < col_end; col++) {
        if (row[col] > peak) {
            peak = row[col];
        }
    }
    return peak;
}

// Trigger variant used while the rolling mean is on: identical crossing
// logic to the raw scan, but each newly-arrived symbol is judged by the
// *averaged* row for its phase rather than its own raw samples. Detection
// is gated until the mean has accumulated n full periods, so it never fires
// on a half-formed average. Averaging first is what makes this stable when
// individual symbols are noisy or packets are being dropped.
static void trigger_scan_mean_new_symbols(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (vis->trigger_mode == TRIGGER_OFF || !vis->persist_accum || vis->persist_period <= 0) {
        return;
    }
    if (memory->symbol_highest < memory->symbol_lowest) {
        return;
    }
    if (persist_periods_elapsed(vis) < vis->persist_n) {
        return;  // still warming up; status reports progress
    }

    const int col_start = vis->trigger_freq_start;
    const int col_end = (vis->trigger_freq_end <= HEATMAP_TEXTURE_WIDTH) ? vis->trigger_freq_end : HEATMAP_TEXTURE_WIDTH;
    const int period = vis->persist_period;

    // Seed from the accumulator itself: its rows are already a settled
    // average over the warm-up, so the baseline is meaningful immediately.
    if (vis->trigger_needs_seed) {
        float sum = 0.0f;
        int count = 0;
        for (int phase = 0; phase < period; phase++) {
            const float peak = accum_row_peak_in_range(
                vis->persist_accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH, col_start, col_end);
            if (peak > 0.0f) {
                sum += peak;
                count++;
            }
        }
        if (count > 0) {
            vis->trigger_baseline_ema = sum / (float)count;
            vis->trigger_baseline_initialized = 1;
        }
        vis->trigger_armed = (vis->trigger_edge == TRIGGER_EDGE_RISING) ? 1 : 0;
        vis->trigger_last_checked_symbol = memory->symbol_highest;
        vis->trigger_has_last_checked = 1;
        vis->trigger_needs_seed = 0;
        return;
    }

    int start_symbol;
    if (!vis->trigger_has_last_checked) {
        start_symbol = memory->symbol_highest;
    } else {
        start_symbol = vis->trigger_last_checked_symbol + 1;
    }
    if (memory->symbol_highest - start_symbol > TRIGGER_MAX_SCAN_SYMBOLS) {
        start_symbol = memory->symbol_highest - TRIGGER_MAX_SCAN_SYMBOLS;
    }

    int can_start_capture = !(vis->trigger_mode == TRIGGER_SINGLE && vis->trigger_single_done);

    for (int symbol_num = start_symbol; symbol_num <= memory->symbol_highest; symbol_num++) {
        int phase = symbol_num % period;
        if (phase < 0) {
            phase += period;
        }
        const float peak_magnitude = accum_row_peak_in_range(
            vis->persist_accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH, col_start, col_end);

        if (!vis->trigger_baseline_initialized) {
            vis->trigger_baseline_ema = peak_magnitude;
            vis->trigger_baseline_initialized = 1;
        } else {
            vis->trigger_baseline_ema = TRIGGER_BASELINE_ALPHA * peak_magnitude +
                                         (1.0f - TRIGGER_BASELINE_ALPHA) * vis->trigger_baseline_ema;
        }

        const float baseline_ref = (vis->trigger_baseline_ema > 1e-6f) ? vis->trigger_baseline_ema : 1e-6f;
        const float fire_threshold = (vis->trigger_level_pct / 100.0f) * baseline_ref;
        const float reset_threshold = vis->trigger_hysteresis_enabled
                                           ? (vis->trigger_reset_level_pct / 100.0f) * baseline_ref
                                           : fire_threshold;

        vis->trigger_last_peak = peak_magnitude;
        vis->trigger_last_fire_threshold = fire_threshold;

        int crossed = 0;
        if (vis->trigger_edge == TRIGGER_EDGE_RISING) {
            if (vis->trigger_armed && peak_magnitude >= fire_threshold) {
                crossed = 1;
                vis->trigger_armed = 0;
            } else if (!vis->trigger_armed && peak_magnitude < reset_threshold) {
                vis->trigger_armed = 1;
            }
        } else {
            if (vis->trigger_armed && peak_magnitude <= fire_threshold) {
                crossed = 1;
                vis->trigger_armed = 0;
            } else if (!vis->trigger_armed && peak_magnitude > reset_threshold) {
                vis->trigger_armed = 1;
            }
        }

        if (crossed && can_start_capture) {
            // Every phase already holds a settled average, so unlike the raw
            // path there is nothing to wait for - the window is complete the
            // moment it locks on.
            vis->trigger_captured_symbol = symbol_num;
            vis->trigger_capture_active = 0;
            vis->trigger_fire_count++;
            if (vis->trigger_mode == TRIGGER_SINGLE) {
                vis->trigger_single_done = 1;
            }
            can_start_capture = 0;
        }
    }

    vis->trigger_last_checked_symbol = memory->symbol_highest;
    vis->trigger_has_last_checked = 1;
}

static void trigger_scan_new_symbols(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (vis->trigger_mode == TRIGGER_OFF) {
        return;
    }
    if (memory->symbol_highest < memory->symbol_lowest) {
        return;  // no data yet
    }

    const int col_start = vis->trigger_freq_start;
    const int col_end = (vis->trigger_freq_end <= HEATMAP_TEXTURE_WIDTH) ? vis->trigger_freq_end : HEATMAP_TEXTURE_WIDTH;

    // A fresh arm (startup, mode change, or a channel switch) faces a ring
    // already full of history belonging to whatever was selected before.
    // Detecting across that backlog would fire on data that is already at
    // the ring's oldest end - in Single mode that lands straight in Holding
    // on a stale window, which looks like the trigger refusing to work on
    // the newly selected channel. So sample the existing data for a
    // baseline, adopt the armed state implied by the newest symbol, and
    // start detecting only from what arrives next.
    if (vis->trigger_needs_seed) {
        float seed_sum = 0.0f;
        int seed_count = 0;
        float newest_peak = 0.0f;
        int have_newest = 0;

        for (int back = 0; back < TRIGGER_SEED_SYMBOLS; back++) {
            const int symbol_num = memory->symbol_highest - back;
            size_t idx;
            if (!ring_row_for_symbol(memory, symbol_num, &idx)) {
                continue;
            }
            const float peak = symbol_peak_in_range(memory, idx, col_start, col_end);
            seed_sum += peak;
            seed_count++;
            if (!have_newest) {
                newest_peak = peak;
                have_newest = 1;
            }
        }

        if (seed_count > 0) {
            vis->trigger_baseline_ema = seed_sum / (float)seed_count;
            vis->trigger_baseline_initialized = 1;

            const float baseline_ref = (vis->trigger_baseline_ema > 1e-6f) ? vis->trigger_baseline_ema : 1e-6f;
            const float fire_threshold = (vis->trigger_level_pct / 100.0f) * baseline_ref;
            const int above = newest_peak >= fire_threshold;
            // Already past the fire level means a rising trigger has in
            // effect just happened and needs a reset first; falling is the
            // mirror image.
            vis->trigger_armed = (vis->trigger_edge == TRIGGER_EDGE_RISING) ? !above : above;
            vis->trigger_last_peak = newest_peak;
            vis->trigger_last_fire_threshold = fire_threshold;
        }

        vis->trigger_last_checked_symbol = memory->symbol_highest;
        vis->trigger_has_last_checked = 1;
        vis->trigger_needs_seed = 0;
        return;  // detection begins with the next batch of genuinely new symbols
    }

    int start_symbol;
    if (!vis->trigger_has_last_checked) {
        start_symbol = memory->symbol_lowest;
    } else {
        start_symbol = vis->trigger_last_checked_symbol + 1;
        if (start_symbol < memory->symbol_lowest) {
            start_symbol = memory->symbol_lowest;
        }
    }
    // Bound how far back a single scan looks, so a first arm or a long stall
    // can't spend a whole frame walking the entire ring. Unlike the previous
    // approach this never skips detection outright - it only trims the oldest
    // end of the backlog, so newly-arrived symbols are always evaluated.
    if (memory->symbol_highest - start_symbol > TRIGGER_MAX_SCAN_SYMBOLS) {
        start_symbol = memory->symbol_highest - TRIGGER_MAX_SCAN_SYMBOLS;
    }

    // A capture can only be centered on a symbol that still has its
    // pre-trigger half available in the ring; anything older would render
    // half-blank immediately. This replaces the old "skip the whole backlog"
    // guard with a precise constraint on *firing* rather than on scanning.
    const int oldest_firable_symbol = memory->symbol_lowest + (vis->trigger_capture_depth / 2);

    int can_start_capture = !vis->trigger_capture_active &&
                             !(vis->trigger_mode == TRIGGER_SINGLE && vis->trigger_single_done);

    for (int symbol_num = start_symbol; symbol_num <= memory->symbol_highest; symbol_num++) {
        size_t idx;
        const int have_data = ring_row_for_symbol(memory, symbol_num, &idx);

        // Symbols with no data are not skipped: in a TDD stream the opposite
        // direction's periods leave real gaps in the numbering, and those
        // gaps genuinely mean "no energy here". Treating them as absent
        // instead would leave the state machine seeing one unbroken run of
        // high values across every burst, so it could never re-arm between
        // them. Counting them as zero is what lets a rising-edge trigger
        // line up on the start of each burst.
        float peak_magnitude = 0.0f;
        if (have_data) {
            peak_magnitude = symbol_peak_in_range(memory, idx, col_start, col_end);

            // The baseline tracks the same statistic the crossing test uses
            // (peak, not mean). Mixing the two would leave a steady signal
            // permanently above threshold by whatever its peak-to-mean ratio
            // happens to be - a ratio that swings with how sparsely the
            // allocation is spread across the resource elements, so it
            // differs per direction and makes the percentage meaningless.
            //
            // Only symbols carrying data feed it; folding in the gaps would
            // drag it toward zero in proportion to the TDD duty cycle and
            // make the threshold track the gap ratio rather than the signal.
            if (!vis->trigger_baseline_initialized) {
                vis->trigger_baseline_ema = peak_magnitude;
                vis->trigger_baseline_initialized = 1;
            } else {
                vis->trigger_baseline_ema = TRIGGER_BASELINE_ALPHA * peak_magnitude +
                                             (1.0f - TRIGGER_BASELINE_ALPHA) * vis->trigger_baseline_ema;
            }
        } else if (!vis->trigger_baseline_initialized) {
            continue;  // nothing to compare against yet
        }

        const float baseline_ref = (vis->trigger_baseline_ema > 1e-6f) ? vis->trigger_baseline_ema : 1e-6f;
        const float fire_threshold = (vis->trigger_level_pct / 100.0f) * baseline_ref;
        // Without hysteresis, resetting uses the same level as firing
        // (original behavior). With it, a separate, more-conservative level
        // must be cleared first, so a brief dip/spike right at the trigger
        // level doesn't look like a fresh crossing and refire immediately.
        const float reset_threshold = vis->trigger_hysteresis_enabled
                                           ? (vis->trigger_reset_level_pct / 100.0f) * baseline_ref
                                           : fire_threshold;

        vis->trigger_last_peak = peak_magnitude;
        vis->trigger_last_fire_threshold = fire_threshold;

        int crossed = 0;
        if (vis->trigger_edge == TRIGGER_EDGE_RISING) {
            if (vis->trigger_armed && peak_magnitude >= fire_threshold) {
                crossed = 1;
                vis->trigger_armed = 0;
            } else if (!vis->trigger_armed && peak_magnitude < reset_threshold) {
                vis->trigger_armed = 1;
            }
        } else {
            if (vis->trigger_armed && peak_magnitude <= fire_threshold) {
                crossed = 1;
                vis->trigger_armed = 0;
            } else if (!vis->trigger_armed && peak_magnitude > reset_threshold) {
                vis->trigger_armed = 1;
            }
        }

        if (crossed && can_start_capture && symbol_num >= oldest_firable_symbol) {
            vis->trigger_captured_symbol = symbol_num;
            vis->trigger_capture_active = 1;
            vis->trigger_fire_count++;
            can_start_capture = 0;  // don't fire again within this same scan
            trigger_snapshot_clear_values(vis);  // start this window's permanent capture fresh
            fprintf(stderr,
                    "[trigger] FIRED symbol=%d (symbol_lowest=%d symbol_highest=%d) "
                    "peak=%.4f baseline=%.4f fire_threshold=%.4f reset_threshold=%.4f\n",
                    symbol_num, memory->symbol_lowest, memory->symbol_highest,
                    (double)peak_magnitude, (double)vis->trigger_baseline_ema, (double)fire_threshold,
                    (double)reset_threshold);
        }
    }

    vis->trigger_last_checked_symbol = memory->symbol_highest;
    vis->trigger_has_last_checked = 1;
}

// Permanently locks in any not-yet-captured row of the current trigger
// window that the live ring currently has data for. Once a row is captured
// here it is never re-read or overwritten, so it survives anything that
// happens to the live ring afterward (eviction, pcap loop resets, etc.) -
// both while a capture is still filling in and after it's complete.
static void trigger_snapshot_capture_new_rows(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis->trigger_snapshot_symbol || !vis->trigger_snapshot_magnitude) {
        return;
    }
    if (vis->trigger_captured_symbol == TRIGGER_NO_SYMBOL) {
        return;
    }

    int half_height = vis->trigger_capture_depth / 2;
    for (int row = 0; row < vis->trigger_capture_depth; row++) {
        if (vis->trigger_snapshot_symbol[row] != TRIGGER_NO_SYMBOL) {
            continue;  // already permanently captured
        }
        int symbol_num = vis->trigger_captured_symbol + (row - half_height);
        size_t idx;
        if (!ring_row_for_symbol(memory, symbol_num, &idx)) {
            continue;  // not available yet, or a sparse gap that may never arrive
        }
        float* dst = vis->trigger_snapshot_magnitude + (size_t)row * HEATMAP_TEXTURE_WIDTH;
        for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
            float i_val, q_val;
            extract_iq(memory->symbols[idx][col], i_val, q_val);
            dst[col] = sqrtf(i_val * i_val + q_val * q_val);
        }
        vis->trigger_snapshot_symbol[row] = symbol_num;
    }
}

void visualizer_get_framebuffer_size(const opengl_visualizer_t* vis, int* out_width, int* out_height) {
    if (out_width) {
        *out_width = vis ? vis->width : 0;
    }
    if (out_height) {
        *out_height = vis ? vis->height : 0;
    }
}

void visualizer_set_axes_enabled(opengl_visualizer_t* vis, int enabled) {
    if (vis) {
        vis->axes_enabled = enabled ? 1 : 0;
    }
}

int visualizer_get_axes_enabled(const opengl_visualizer_t* vis) {
    return vis ? vis->axes_enabled : 0;
}

void visualizer_get_plot_rect(const opengl_visualizer_t* vis,
                              float* out_x, float* out_y, float* out_w, float* out_h) {
    float x = PLOT_MARGIN_LEFT;
    float y = PLOT_MARGIN_TOP;
    float w = 1.0f;
    float h = 1.0f;
    if (vis && !vis->axes_enabled) {
        // No axes to make room for: fill the window.
        x = 0.0f;
        y = 0.0f;
        w = (float)vis->width;
        h = (float)vis->height;
        if (w < 1.0f) {
            w = 1.0f;
        }
        if (h < 1.0f) {
            h = 1.0f;
        }
    } else if (vis) {
        w = (float)vis->width - PLOT_MARGIN_LEFT - PLOT_MARGIN_RIGHT;
        h = (float)vis->height - PLOT_MARGIN_TOP - PLOT_MARGIN_BOTTOM;
        if (w < 1.0f) {
            w = 1.0f;
        }
        if (h < 1.0f) {
            h = 1.0f;
        }
    }
    if (out_x) { *out_x = x; }
    if (out_y) { *out_y = y; }
    if (out_w) { *out_w = w; }
    if (out_h) { *out_h = h; }
}

// Smallest multiplier from a round-number ladder that spreads ticks at least
// min_px apart, so zooming out thins the axis instead of crowding it.
static int axis_nice_multiple(float unit_px, float min_px) {
    static const int ladder[] = { 1, 2, 5, 10, 20, 25, 50, 100, 200, 500, 1000, 2000, 5000 };
    const int count = (int)(sizeof(ladder) / sizeof(ladder[0]));
    for (int i = 0; i < count; i++) {
        if (unit_px * (float)ladder[i] >= min_px) {
            return ladder[i];
        }
    }
    return ladder[count - 1];
}

void visualizer_get_axis_titles(const opengl_visualizer_t* vis,
                                const char** out_y_title,
                                const char** out_x_title) {
    const char* y_title = "Symbol";
    if (vis) {
        if (vis->axis_curve_mode) {
            y_title = "Magnitude";
        } else if (vis->axis_phase_mode) {
            y_title = "Offset in period";
        } else {
            y_title = "Frame / slot";
        }
    }
    if (out_y_title) {
        *out_y_title = y_title;
    }
    if (out_x_title) {
        *out_x_title = "PRB / RE";
    }
}

int visualizer_get_y_ticks(const opengl_visualizer_t* vis,
                           const fh_oran_memory_t* memory,
                           visualizer_axis_tick_t* out_ticks,
                           int max_ticks) {
    if (!vis || !out_ticks || max_ticks <= 0 || !vis->axis_valid || vis->axis_rows <= 0) {
        return 0;
    }

    float plot_y = 0.0f, plot_h = 1.0f;
    visualizer_get_plot_rect(vis, NULL, &plot_y, NULL, &plot_h);
    const float px_per_row = plot_h / (float)vis->axis_rows;
    int count = 0;

    // Curve view: the vertical axis is normalised magnitude, so it just gets
    // evenly spaced percentage gridlines.
    if (vis->axis_curve_mode) {
        for (int pct = 0; pct <= 100 && count < max_ticks; pct += 25) {
            visualizer_axis_tick_t* t = &out_ticks[count++];
            // The curve render insets its trace by 5% top and bottom.
            const float frac = 0.05f + (1.0f - (float)pct / 100.0f) * 0.90f;
            t->position_px = plot_y + frac * plot_h;
            t->level = (pct == 0 || pct == 100) ? 2 : 1;
            t->has_label = 1;
            snprintf(t->label, sizeof(t->label), "%d%%", pct);
            t->label2[0] = '\0';
        }
        return count;
    }

    if (vis->axis_phase_mode) {
        // Rows are positions inside the averaging period. Absolute frame
        // numbering is meaningless for an average spanning many frames, so
        // only offsets relative to the period start are labelled.
        const int period = vis->axis_period > 0 ? vis->axis_period : 1;
        const int scs_khz = (memory && memory->scs_khz > 0) ? memory->scs_khz : 0;
        const int slots_per_subframe = (scs_khz > 0 && scs_khz / 15 > 0) ? scs_khz / 15 : 0;
        const int symbols_per_subframe = slots_per_subframe > 0 ? slots_per_subframe * SYMBOLS_PER_SLOT : 0;
        // Subframe offsets only make sense if the period divides into whole subframes.
        const int subframe_useful = symbols_per_subframe > 0 && (period % symbols_per_subframe) == 0;

        const int label_symbol = px_per_row >= AXIS_MIN_LABEL_PX_Y;
        const int tick_symbol = px_per_row >= AXIS_MIN_TICK_PX;
        const float slot_px = px_per_row * (float)SYMBOLS_PER_SLOT;
        const int label_slot = slot_px >= AXIS_MIN_LABEL_PX_Y;
        const int tick_slot = slot_px >= AXIS_MIN_TICK_PX;

        // The bottom row always carries a fully spelled-out label, so the
        // view is never left without a position readout when it spans less
        // than one division.
        const int anchor_row = vis->axis_rows - 1;
        if (anchor_row >= 0 && count < max_ticks) {
            int phase = (vis->axis_row0_value + anchor_row) % period;
            if (phase < 0) {
                phase += period;
            }
            visualizer_axis_tick_t* t = &out_ticks[count++];
            t->position_px = plot_y + (float)anchor_row * px_per_row;
            t->level = 3;
            t->has_label = 1;
            snprintf(t->label, sizeof(t->label), "phase +%d", phase);
            if (subframe_useful) {
                snprintf(t->label2, sizeof(t->label2), "sf+%d sl+%d sym%d",
                         phase / symbols_per_subframe,
                         (phase % symbols_per_subframe) / SYMBOLS_PER_SLOT,
                         phase % SYMBOLS_PER_SLOT);
            } else {
                snprintf(t->label2, sizeof(t->label2), "sl+%d sym%d",
                         phase / SYMBOLS_PER_SLOT, phase % SYMBOLS_PER_SLOT);
            }
        }

        for (int row = 0; row < vis->axis_rows && count < max_ticks; row++) {
            if (row == anchor_row) {
                continue;  // already emitted with a full label
            }
            int phase = (vis->axis_row0_value + row) % period;
            if (phase < 0) {
                phase += period;
            }

            int level;
            int labelled;
            if (subframe_useful && (phase % symbols_per_subframe) == 0) {
                level = 2;
                labelled = 1;
            } else if ((phase % SYMBOLS_PER_SLOT) == 0) {
                if (!tick_slot) {
                    continue;
                }
                level = 1;
                labelled = label_slot;
            } else {
                if (!tick_symbol) {
                    continue;
                }
                level = 0;
                labelled = label_symbol;
            }

            visualizer_axis_tick_t* t = &out_ticks[count++];
            t->position_px = plot_y + (float)row * px_per_row;
            t->level = level;
            t->has_label = labelled;
            t->label2[0] = '\0';
            if (!labelled) {
                t->label[0] = '\0';
            } else if (level == 2) {
                snprintf(t->label, sizeof(t->label), "sf+%d", phase / symbols_per_subframe);
            } else if (level == 1) {
                snprintf(t->label, sizeof(t->label), "sl+%d", phase / SYMBOLS_PER_SLOT);
            } else {
                snprintf(t->label, sizeof(t->label), "%d", phase % SYMBOLS_PER_SLOT);
            }
        }
        return count;
    }

    // Live or triggered view: rows are absolute symbol numbers.
    const int scs_khz = (memory && memory->scs_khz > 0) ? memory->scs_khz : 0;
    int slots_per_subframe = scs_khz > 0 ? scs_khz / 15 : 0;
    if (slots_per_subframe <= 0) {
        slots_per_subframe = 0;  // unknown spacing: fall back to slot/symbol ticks only
    }
    const int symbols_per_subframe = slots_per_subframe > 0 ? slots_per_subframe * SYMBOLS_PER_SLOT : 0;
    const int symbols_per_frame = symbols_per_subframe > 0 ? symbols_per_subframe * 10 : 0;

    const int tick_symbol = px_per_row >= AXIS_MIN_TICK_PX;
    const int label_symbol = px_per_row >= AXIS_MIN_LABEL_PX_Y;
    const float slot_px = px_per_row * (float)SYMBOLS_PER_SLOT;
    const int tick_slot = slot_px >= AXIS_MIN_TICK_PX;
    const int label_slot = slot_px >= AXIS_MIN_LABEL_PX_Y;
    const float subframe_px = symbols_per_subframe > 0 ? px_per_row * (float)symbols_per_subframe : 0.0f;
    const int tick_subframe = symbols_per_subframe > 0 && subframe_px >= AXIS_MIN_TICK_PX;
    const int label_subframe = symbols_per_subframe > 0 && subframe_px >= AXIS_MIN_LABEL_PX_Y;

    // Frames always stay labelled; when they would crowd, step by a round
    // number of frames instead of dropping the level entirely.
    int frame_step = 1;
    if (symbols_per_frame > 0) {
        frame_step = axis_nice_multiple(px_per_row * (float)symbols_per_frame, AXIS_MIN_LABEL_PX_Y);
    }

    // The newest symbol (bottom row) always gets the complete position spelled
    // out, so a view spanning less than a whole frame still shows which frame,
    // subframe, slot and symbol it is looking at.
    const int anchor_row = vis->axis_rows - 1;
    const int anchor_symbol = vis->axis_row0_value + anchor_row;
    if (anchor_row >= 0 && anchor_symbol >= 0 && count < max_ticks) {
        visualizer_axis_tick_t* t = &out_ticks[count++];
        t->position_px = plot_y + (float)anchor_row * px_per_row;
        t->level = 3;
        t->has_label = 1;
        if (symbols_per_frame > 0) {
            const unsigned long long frame = (unsigned long long)(anchor_symbol / symbols_per_frame);
            const int in_frame = anchor_symbol % symbols_per_frame;
            snprintf(t->label, sizeof(t->label), "F%llu (%llu)", frame, frame % 256ULL);
            snprintf(t->label2, sizeof(t->label2), "sf%d sl%d sym%d",
                     in_frame / symbols_per_subframe,
                     (in_frame % symbols_per_subframe) / SYMBOLS_PER_SLOT,
                     anchor_symbol % SYMBOLS_PER_SLOT);
        } else {
            // Subcarrier spacing unknown, so frame structure can't be resolved.
            snprintf(t->label, sizeof(t->label), "sym %d", anchor_symbol);
            snprintf(t->label2, sizeof(t->label2), "sl%d sym%d",
                     anchor_symbol / SYMBOLS_PER_SLOT, anchor_symbol % SYMBOLS_PER_SLOT);
        }
    }

    for (int row = 0; row < vis->axis_rows && count < max_ticks; row++) {
        if (row == anchor_row) {
            continue;  // already emitted with a full label
        }
        const int symbol = vis->axis_row0_value + row;
        if (symbol < 0) {
            continue;
        }

        int level;
        int labelled;
        if (symbols_per_frame > 0 && (symbol % symbols_per_frame) == 0 &&
            ((symbol / symbols_per_frame) % frame_step) == 0) {
            level = 3;
            labelled = 1;
        } else if (symbols_per_subframe > 0 && (symbol % symbols_per_subframe) == 0) {
            if (!tick_subframe) {
                continue;
            }
            level = 2;
            labelled = label_subframe;
        } else if ((symbol % SYMBOLS_PER_SLOT) == 0) {
            if (!tick_slot) {
                continue;
            }
            level = 1;
            labelled = label_slot;
        } else {
            if (!tick_symbol) {
                continue;
            }
            level = 0;
            labelled = label_symbol;
        }

        visualizer_axis_tick_t* t = &out_ticks[count++];
        t->position_px = plot_y + (float)row * px_per_row;
        t->level = level;
        t->has_label = labelled;
        t->label2[0] = '\0';

        if (!labelled) {
            t->label[0] = '\0';
        } else if (level == 3) {
            const unsigned long long frame = (unsigned long long)(symbol / symbols_per_frame);
            // Both numbers matter: the absolute one keeps counting, the
            // parenthesised one is the 8-bit id actually carried on the wire.
            snprintf(t->label, sizeof(t->label), "F%llu (%llu)", frame, frame % 256ULL);
        } else if (level == 2) {
            snprintf(t->label, sizeof(t->label), "sf%d",
                     (symbol % symbols_per_frame) / symbols_per_subframe);
        } else if (level == 1) {
            const int in_subframe = symbols_per_subframe > 0 ? (symbol % symbols_per_subframe)
                                                              : (symbol % SYMBOLS_PER_SLOT);
            snprintf(t->label, sizeof(t->label), "sl%d", in_subframe / SYMBOLS_PER_SLOT);
        } else {
            snprintf(t->label, sizeof(t->label), "%d", symbol % SYMBOLS_PER_SLOT);
        }
    }

    return count;
}

int visualizer_get_x_ticks(const opengl_visualizer_t* vis,
                           visualizer_axis_tick_t* out_ticks,
                           int max_ticks) {
    if (!vis || !out_ticks || max_ticks <= 0) {
        return 0;
    }

    float plot_x = 0.0f, plot_w = 1.0f;
    visualizer_get_plot_rect(vis, &plot_x, NULL, &plot_w, NULL);

    // The full grid is always in view for now; a zoom would narrow this range
    // and everything below adapts to it.
    const int re_first = 0;
    const int re_count = HEATMAP_TEXTURE_WIDTH;
    const float px_per_re = plot_w / (float)re_count;
    const float px_per_prb = px_per_re * (float)HEATMAP_SAMPLES_PER_PRB;

    int count = 0;

    // Individual resource elements only get their own ticks once a PRB is
    // wide enough to show its twelve subcarriers without them merging.
    const int show_re = px_per_re >= AXIS_MIN_TICK_PX;
    const int label_re = px_per_re >= 20.0f;

    const int prb_step = axis_nice_multiple(px_per_prb, AXIS_MIN_LABEL_PX_X);
    const int prb_first = re_first / HEATMAP_SAMPLES_PER_PRB;
    const int prb_last = (re_first + re_count - 1) / HEATMAP_SAMPLES_PER_PRB;

    for (int prb = prb_first; prb <= prb_last && count < max_ticks; prb++) {
        const int on_step = (prb % prb_step) == 0;
        if (!on_step && !show_re) {
            continue;
        }

        const int re = prb * HEATMAP_SAMPLES_PER_PRB;
        visualizer_axis_tick_t* t = &out_ticks[count++];
        t->position_px = plot_x + (float)(re - re_first) * px_per_re;
        t->level = 1;
        t->has_label = on_step;
        if (on_step) {
            snprintf(t->label, sizeof(t->label), "%d", prb);
            snprintf(t->label2, sizeof(t->label2), "%d", re);
        } else {
            t->label[0] = '\0';
            t->label2[0] = '\0';
        }

        if (!show_re) {
            continue;
        }
        for (int sub = 1; sub < HEATMAP_SAMPLES_PER_PRB && count < max_ticks; sub++) {
            visualizer_axis_tick_t* st = &out_ticks[count++];
            st->position_px = plot_x + (float)(re + sub - re_first) * px_per_re;
            st->level = 0;
            st->has_label = label_re;
            st->label2[0] = '\0';
            if (label_re) {
                snprintf(st->label, sizeof(st->label), "%d", sub);
            } else {
                st->label[0] = '\0';
            }
        }
    }

    return count;
}

// Update heatmap texture with IQ data from ring buffer (or, in persistence
// mode, from the rolling-mean accumulator).
static void update_heatmap_texture(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis || !vis->texture_data || !memory || !memory->symbols) {
        return;
    }

    if (vis->persist_enabled) {
        persist_fold_new_symbols(vis, memory);
        if (vis->trigger_mode != TRIGGER_OFF) {
            trigger_scan_mean_new_symbols(vis, memory);
        }
    } else if (vis->trigger_mode != TRIGGER_OFF) {
        // Scanned here (before sizing) so that a crossing detected this very
        // frame is already reflected in trigger_captured_symbol below.
        trigger_scan_new_symbols(vis, memory);
        trigger_snapshot_capture_new_rows(vis, memory);
    }

    // Render only a sliding window of the most recent symbols (raw mode), or
    // the full averaging period (persistence mode). Keeping the texture
    // height bounded prevents the visualizer from dominating CPU time.
    const int mean_triggered = vis->persist_enabled && vis->trigger_mode != TRIGGER_OFF &&
                                vis->trigger_captured_symbol != TRIGGER_NO_SYMBOL && vis->persist_accum != NULL;

    int desired_height;
    if (mean_triggered) {
        // A triggered mean view shows the same fixed depth as a raw capture,
        // just sourced from the averaged rows.
        desired_height = vis->trigger_capture_depth;
    } else if (vis->persist_enabled) {
        desired_height = vis->persist_period;
    } else if (vis->trigger_mode != TRIGGER_OFF && vis->trigger_captured_symbol != TRIGGER_NO_SYMBOL) {
        // A triggered capture uses its own small, independent depth (see
        // visualizer_set_trigger_capture_depth) rather than the live view's
        // memory-size-tied height - waiting for the post-trigger half to
        // arrive would otherwise evict the pre-trigger half by the time it's
        // done, since both draw from the same finite ring.
        desired_height = vis->trigger_capture_depth;
    } else {
        desired_height = vis->tex_requested_height > 0 ? vis->tex_requested_height : HEATMAP_DEFAULT_TEXTURE_HEIGHT;
        if (memory->max_symbols > 0 && memory->max_symbols < (size_t)desired_height) {
            desired_height = (int)memory->max_symbols;
        }
    }
    int cap = vis->tex_max_height > 0 ? vis->tex_max_height : 4096;
    if (desired_height > cap) {
        desired_height = cap;
    }

    if (desired_height != vis->tex_height) {
        // Reallocate CPU-side buffer and resize GPU texture
        free(vis->texture_data);
        vis->tex_height = desired_height;
        vis->texture_data = (uint8_t*)calloc(HEATMAP_TEXTURE_WIDTH * vis->tex_height * 4, sizeof(uint8_t));
        glBindTexture(GL_TEXTURE_2D, vis->texture_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, HEATMAP_TEXTURE_WIDTH, vis->tex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }

    // Clear texture
    memset(vis->texture_data, 0, HEATMAP_TEXTURE_WIDTH * vis->tex_height * 4);

    if (memory->symbol_highest < memory->symbol_lowest) {
        return;  // No data yet
    }

    if (vis->persist_enabled) {
        const int period = vis->persist_period;

        // Untriggered, the rows simply run phase 0 downward through the
        // period. Triggered, the same averaged rows are rotated so the phase
        // the crossing landed on sits at the middle row, under the red line -
        // the alignment is what makes the averaged picture hold still.
        vis->axis_valid = 1;
        vis->axis_rows = vis->tex_height;
        vis->axis_phase_mode = 1;
        vis->axis_curve_mode = 0;
        vis->axis_period = period;

        int display_height;
        int phase_offset = 0;
        if (mean_triggered) {
            display_height = (vis->trigger_capture_depth < vis->tex_height) ? vis->trigger_capture_depth
                                                                            : vis->tex_height;
            int captured_phase = vis->trigger_captured_symbol % period;
            if (captured_phase < 0) {
                captured_phase += period;
            }
            phase_offset = captured_phase - (vis->trigger_capture_depth / 2);
        } else {
            display_height = (period < vis->tex_height) ? period : vis->tex_height;
        }

        // Maps a texture row to the accumulator phase it displays.
        #define PERSIST_ROW_PHASE(row) (((((row) + phase_offset) % period) + period) % period)

        vis->axis_row0_value = PERSIST_ROW_PHASE(0);

        // Find max magnitude for normalization
        float max_magnitude = 1e-9f;
        for (int row = 0; row < display_height; row++) {
            const float* accum_row = vis->persist_accum + (size_t)PERSIST_ROW_PHASE(row) * HEATMAP_TEXTURE_WIDTH;
            for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                float magnitude = accum_row[col];
                if (magnitude > max_magnitude) {
                    max_magnitude = magnitude;
                }
            }
        }

        // Smooth magnitude scaling so rapid amplitude shifts do not cause color flicker.
        if (!vis->magnitude_scale_initialized) {
            vis->magnitude_scale_ema = max_magnitude;
            vis->magnitude_scale_initialized = 1;
        } else {
            const float alpha_rise = 0.35f;
            const float alpha_fall = 0.08f;
            const float alpha = (max_magnitude > vis->magnitude_scale_ema) ? alpha_rise : alpha_fall;
            vis->magnitude_scale_ema = alpha * max_magnitude + (1.0f - alpha) * vis->magnitude_scale_ema;
        }
        const float render_scale = (vis->magnitude_scale_ema > 1e-6f) ? vis->magnitude_scale_ema : 1e-6f;

        for (int tex_row = 0; tex_row < display_height; tex_row++) {
            const float* accum_row = vis->persist_accum + (size_t)PERSIST_ROW_PHASE(tex_row) * HEATMAP_TEXTURE_WIDTH;

            for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                const float magnitude = accum_row[col];

                // Cells that have never received a symbol (e.g. the other
                // direction's periods in a TDD stream) are left black, the
                // same as the raw view, rather than being folded to zero -
                // which magnitude_to_rgb would render as the dark blue
                // bottom of the gradient, i.e. indistinguishable from a real
                // zero-power measurement.
                uint8_t r = 0, g = 0, b = 0;
                if (magnitude >= 0.0f) {
                    magnitude_to_rgb(magnitude, render_scale, &r, &g, &b);
                }

                size_t pixel_idx = (tex_row * HEATMAP_TEXTURE_WIDTH + col) * 4;
                vis->texture_data[pixel_idx + 0] = r;
                vis->texture_data[pixel_idx + 1] = g;
                vis->texture_data[pixel_idx + 2] = b;
                vis->texture_data[pixel_idx + 3] = 255;
            }
        }

        #undef PERSIST_ROW_PHASE
    } else {
        // Trigger completion check: has an active capture's post-trigger
        // half now fully arrived (i.e. is the bottom of the centered window
        // available)? Uses tex_height *after* the resize above, so
        // half_height reflects trigger_capture_depth, not the live view's
        // memory-size-tied height. The crossing scan itself already ran
        // earlier, before sizing.
        int trigger_half_height = vis->trigger_capture_depth / 2;
        if (vis->trigger_mode != TRIGGER_OFF && vis->trigger_capture_active) {
            if (vis->trigger_captured_symbol + trigger_half_height <= memory->symbol_highest) {
                vis->trigger_capture_active = 0;
                if (vis->trigger_mode == TRIGGER_SINGLE) {
                    vis->trigger_single_done = 1;
                }

                int pre_valid = 0, post_valid = 0;
                if (vis->trigger_snapshot_symbol) {
                    for (int row = 0; row < trigger_half_height; row++) {
                        if (vis->trigger_snapshot_symbol[row] != TRIGGER_NO_SYMBOL) {
                            pre_valid++;
                        }
                    }
                    for (int row = trigger_half_height; row < vis->trigger_capture_depth; row++) {
                        if (vis->trigger_snapshot_symbol[row] != TRIGGER_NO_SYMBOL) {
                            post_valid++;
                        }
                    }
                }
                fprintf(stderr,
                        "[trigger] COMPLETE captured=%d capture_depth=%d symbol_lowest=%d symbol_highest=%d "
                        "pre_valid=%d/%d post_valid=%d/%d (permanent snapshot)\n",
                        vis->trigger_captured_symbol, vis->trigger_capture_depth, memory->symbol_lowest,
                        memory->symbol_highest, pre_valid, trigger_half_height, post_valid,
                        vis->trigger_capture_depth - trigger_half_height);
            }
        }

        int showing_triggered = (vis->trigger_mode != TRIGGER_OFF && vis->trigger_captured_symbol != TRIGGER_NO_SYMBOL);

        int symbol_count = memory->symbol_highest - memory->symbol_lowest + 1;
        int display_height = (symbol_count < vis->tex_height) ? symbol_count : vis->tex_height;

        vis->axis_valid = 1;
        vis->axis_rows = vis->tex_height;
        vis->axis_phase_mode = 0;
        vis->axis_curve_mode = 0;
        vis->axis_period = 0;
        // A triggered window is centred on the captured symbol; the live view
        // scrolls with the newest symbol pinned to the bottom row.
        vis->axis_row0_value = showing_triggered
                                   ? vis->trigger_captured_symbol - (vis->trigger_capture_depth / 2)
                                   : memory->symbol_highest - (vis->tex_height - 1);

        // Find max magnitude for normalization. While showing a triggered
        // window, scan its permanent snapshot instead of live ring content,
        // so the color scale stays consistent with what's actually on
        // screen regardless of what the live ring is doing in the
        // background. Otherwise (plain live scroll), scan the recent window
        // as before.
        float max_magnitude = 1e-9f;
        if (showing_triggered) {
            if (vis->trigger_snapshot_symbol) {
                for (int row = 0; row < vis->trigger_capture_depth; row++) {
                    if (vis->trigger_snapshot_symbol[row] == TRIGGER_NO_SYMBOL) {
                        continue;
                    }
                    const float* src = vis->trigger_snapshot_magnitude + (size_t)row * HEATMAP_TEXTURE_WIDTH;
                    for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                        if (src[col] > max_magnitude) {
                            max_magnitude = src[col];
                        }
                    }
                }
            }
        } else {
            for (int sym_offset = 0; sym_offset < display_height; sym_offset++) {
                int symbol_num = memory->symbol_highest - sym_offset;
                size_t idx;
                if (!ring_row_for_symbol(memory, symbol_num, &idx)) {
                    continue;
                }
                for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                    float i_val, q_val;
                    extract_iq(memory->symbols[idx][col], i_val, q_val);
                    float magnitude = sqrtf(i_val * i_val + q_val * q_val);
                    if (magnitude > max_magnitude) {
                        max_magnitude = magnitude;
                    }
                }
            }
        }

        // Smooth magnitude scaling so rapid amplitude shifts do not cause color flicker.
        if (!vis->magnitude_scale_initialized) {
            vis->magnitude_scale_ema = max_magnitude;
            vis->magnitude_scale_initialized = 1;
        } else {
            // Follow rising peaks quickly, decay slowly for a stable visual scale.
            const float alpha_rise = 0.35f;
            const float alpha_fall = 0.08f;
            const float alpha = (max_magnitude > vis->magnitude_scale_ema) ? alpha_rise : alpha_fall;
            vis->magnitude_scale_ema = alpha * max_magnitude + (1.0f - alpha) * vis->magnitude_scale_ema;
        }
        const float render_scale = (vis->magnitude_scale_ema > 1e-6f) ? vis->magnitude_scale_ema : 1e-6f;

        if (!showing_triggered) {
            // Render heatmap bottom-up (newest symbols at bottom) - the
            // usual continuous scroll, also shown before the first crossing.
            for (int sym_offset = 0; sym_offset < display_height; sym_offset++) {
                int symbol_num = memory->symbol_highest - sym_offset;
                size_t idx;
                if (!ring_row_for_symbol(memory, symbol_num, &idx)) {
                    continue;
                }

                // Texture row is inverted: newest at bottom
                int tex_row = vis->tex_height - 1 - sym_offset;

                for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                    float i_val, q_val;
                    extract_iq(memory->symbols[idx][col], i_val, q_val);
                    float magnitude = sqrtf(i_val * i_val + q_val * q_val);

                    uint8_t r, g, b;
                    magnitude_to_rgb(magnitude, render_scale, &r, &g, &b);

                    size_t pixel_idx = (tex_row * HEATMAP_TEXTURE_WIDTH + col) * 4;
                    vis->texture_data[pixel_idx + 0] = r;      // R
                    vis->texture_data[pixel_idx + 1] = g;      // G
                    vis->texture_data[pixel_idx + 2] = b;      // B
                    vis->texture_data[pixel_idx + 3] = 255;    // A
                }
            }
        } else if (vis->trigger_snapshot_symbol) {
            // Triggered: fixed window centered on the captured symbol,
            // rendered from the permanent snapshot rather than the live
            // ring, so it survives ring eviction/pcap loops once a row has
            // been captured. Rows not yet captured (or a sparse gap that
            // never gets data) are left blank.
            for (int tex_row = 0; tex_row < vis->tex_height && tex_row < vis->trigger_capture_depth; tex_row++) {
                if (vis->trigger_snapshot_symbol[tex_row] == TRIGGER_NO_SYMBOL) {
                    continue;
                }
                const float* src = vis->trigger_snapshot_magnitude + (size_t)tex_row * HEATMAP_TEXTURE_WIDTH;

                for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                    uint8_t r, g, b;
                    magnitude_to_rgb(src[col], render_scale, &r, &g, &b);

                    size_t pixel_idx = (tex_row * HEATMAP_TEXTURE_WIDTH + col) * 4;
                    vis->texture_data[pixel_idx + 0] = r;
                    vis->texture_data[pixel_idx + 1] = g;
                    vis->texture_data[pixel_idx + 2] = b;
                    vis->texture_data[pixel_idx + 3] = 255;
                }
            }
        }
    }

    // Upload texture to GPU
    glBindTexture(GL_TEXTURE_2D, vis->texture_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, HEATMAP_TEXTURE_WIDTH, vis->tex_height, GL_RGBA, GL_UNSIGNED_BYTE, vis->texture_data);
}

int visualizer_update(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis || !vis->window || !vis->is_running) {
        return -1;
    }
    
    if (glfwWindowShouldClose(vis->window)) {
        vis->is_running = 0;
        return -1;
    }
    
    glfwPollEvents();

    int fb_width = 0;
    int fb_height = 0;
    glfwGetFramebufferSize(vis->window, &fb_width, &fb_height);
    if (fb_width <= 0 || fb_height <= 0) {
        return 0;
    }

    if (fb_width != vis->width || fb_height != vis->height) {
        vis->width = fb_width;
        vis->height = fb_height;
        glViewport(0, 0, vis->width, vis->height);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, vis->width, vis->height, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }

    // A different stream (eAxC/direction) means accumulated persistence/mean
    // averages no longer correspond to what's about to be shown. Detected
    // here (rather than requiring a signal from the caller) since this
    // function already receives the memory pointer every frame.
    if (memory != vis->last_seen_memory) {
        if (vis->last_seen_memory != NULL) {
            if (vis->persist_accum) {
                persist_clear_values(vis);
            }
            if (vis->curve_mean_accum) {
                curve_mean_clear_values(vis);
            }
            trigger_reset_state(vis);
        }
        vis->last_seen_memory = memory;
    }

    glClear(GL_COLOR_BUFFER_BIT);

    if (vis->view_mode == VIEW_MODE_CURVE) {
        draw_curve_view(vis, memory);
    } else {
        // Update texture from memory
        update_heatmap_texture(vis, memory);

        // Render with immediate mode - draw fullscreen quad
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, vis->texture_id);

        // Confined to the plot rect so the surrounding margins stay clear for
        // the axes.
        float plot_x = 0.0f, plot_y = 0.0f, plot_w = 1.0f, plot_h = 1.0f;
        visualizer_get_plot_rect(vis, &plot_x, &plot_y, &plot_w, &plot_h);

        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(plot_x, plot_y);
        glTexCoord2f(1, 0); glVertex2f(plot_x + plot_w, plot_y);
        glTexCoord2f(1, 1); glVertex2f(plot_x + plot_w, plot_y + plot_h);
        glTexCoord2f(0, 1); glVertex2f(plot_x, plot_y + plot_h);
        glEnd();

        glDisable(GL_TEXTURE_2D);

        // Trigger line: marks the row carrying the captured symbol once a
        // capture has fired. Drawn in screen space (like the curve view's
        // lines) so it stays crisp regardless of the texture's row height.
        if (vis->trigger_mode != TRIGGER_OFF && vis->trigger_captured_symbol != TRIGGER_NO_SYMBOL) {
            const int rows = vis->tex_height > 0 ? vis->tex_height : 1;
            const int centre_row = vis->trigger_capture_depth / 2;
            const float line_y = plot_y + ((float)centre_row + 0.5f) * (plot_h / (float)rows);
            glColor4f(1.0f, 0.0f, 0.0f, 1.0f);
            glLineWidth(1.5f);
            glBegin(GL_LINES);
            glVertex2f(plot_x, line_y);
            glVertex2f(plot_x + plot_w, line_y);
            glEnd();
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

    return 0;
}

void visualizer_present(opengl_visualizer_t* vis) {
    if (!vis || !vis->window || !vis->is_running) {
        return;
    }

    glfwSwapBuffers(vis->window);
}

void visualizer_run(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis) {
        return;
    }
    
    while (vis->is_running && visualizer_update(vis, memory) == 0) {
        visualizer_present(vis);
        // Loop continues
    }
}

int visualizer_is_running(const opengl_visualizer_t* vis) {
    return vis ? vis->is_running : 0;
}

void visualizer_destroy(opengl_visualizer_t* vis) {
    if (!vis) {
        return;
    }
    
    if (vis->texture_id) {
        glDeleteTextures(1, &vis->texture_id);
    }
    if (vis->texture_data) {
        free(vis->texture_data);
    }
    if (vis->persist_accum) {
        free(vis->persist_accum);
    }
    if (vis->curve_mean_accum) {
        free(vis->curve_mean_accum);
    }
    if (vis->trigger_snapshot_symbol) {
        free(vis->trigger_snapshot_symbol);
    }
    if (vis->trigger_snapshot_magnitude) {
        free(vis->trigger_snapshot_magnitude);
    }
    if (vis->window) {
        glfwDestroyWindow(vis->window);
    }
    
    glfwTerminate();
    free(vis);
}

