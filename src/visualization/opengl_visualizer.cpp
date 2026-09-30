#include "opengl_visualizer.h"
#include "core/c_oran_memory.h"
#include "core/c_radar.h"

#include <limits.h>
#include <stdint.h>
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

#define RADAR_PERSIST_DEFAULT_PERIOD PERSIST_DEFAULT_PERIOD
#define RADAR_PERSIST_DEFAULT_N PERSIST_DEFAULT_N
// Slots in the radar rolling mean's folded-symbol tag ring. Has to exceed the
// deepest radar window (4096 symbols, RADAR_MAX_TIME_SYMBOLS in c_radar.c) so
// two symbols of one frame can never share a slot.
#define RADAR_PERSIST_TAG_SLOTS 8192

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
    // Columns in the texture. HEATMAP_TEXTURE_WIDTH for every view built from
    // the symbol ring; the radar sets its own, since a delay axis is as wide as
    // the transform rather than as wide as the resource grid.
    int tex_width;
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

    // Radar core, owned by main and borrowed here. NULL until handed over,
    // which is what the radar view treats as "not available yet".
    struct fh_radar* radar;

    // Radar rolling mean over the CFR frame. The radar recomputes its whole
    // window every pass, so one symbol turns up in many consecutive frames;
    // the tag ring records which symbols are already in, keyed by
    // symbol % RADAR_PERSIST_TAG_SLOTS, and is what keeps each to one fold.
    int radar_persist_enabled;
    int radar_persist_period;
    int radar_persist_n;
    int radar_persist_cols;              // frame width the accumulator was sized for; 0 = not allocated
    float* radar_persist_accum;          // radar_persist_period * radar_persist_cols |H| means
    int* radar_persist_tags;             // RADAR_PERSIST_TAG_SLOTS folded symbol numbers
    int radar_persist_has_epoch;         // whether radar_persist_epoch is valid yet
    uint64_t radar_persist_epoch;        // radar epoch every folded frame belonged to
    int radar_persist_frame_seen;        // whether radar_persist_last_refresh is valid yet
    uint64_t radar_persist_last_refresh; // last frame folded, so each is scanned once
    int radar_persist_has_folded;
    int radar_persist_first_symbol;      // oldest and newest symbol folded since the last
    int radar_persist_last_symbol;       // clear, for counting accumulated periods

    // Curve view: trailing raw traces (read live from the memory ring, no storage of their own)
    int curve_trace_enabled;
    int curve_trace_count;

    // What the curve view draws in one frame, gathered apart from the drawing.
    // Traces are magnitudes, newest first.
    float* curve_frame_traces;        // curve_frame_trace_count * HEATMAP_TEXTURE_WIDTH
    int curve_frame_trace_capacity;   // traces the buffer holds
    int curve_frame_trace_count;      // traces in it
    int curve_frame_trace_slots;      // trace count the fade is spread over
    float* curve_frame_mean;          // HEATMAP_TEXTURE_WIDTH, when curve_frame_has_mean
    int curve_frame_has_mean;
    float curve_frame_scale;

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
    // Markers, plus the mean magnitude of whatever is on screen that their
    // percentages are measured against.
    visualizer_marker_info_t markers[VISUALIZER_MAX_MARKERS];
    int marker_count;
    float display_mean_magnitude;
    int display_mean_valid;

    // Zoom: a normalized [0,1] view window per axis over the fully zoomed-out
    // extent. Display-only - see the header. Kept as fractions rather than row
    // or RE indices so it survives the vertical extent being resized by
    // Memory size, averaging period or trigger capture depth.
    float zoom_x_center;
    float zoom_x_span;
    float zoom_y_center;
    float zoom_y_span;
    // The vertical axis measures different things in the two views - symbol
    // rows against magnitude - so each parks its own window here while the
    // other is on screen. Without that, zooming into a band of symbols and
    // switching to Curve would silently reinterpret those row bounds as a
    // magnitude range. The horizontal axis is shared between the waterfall and
    // curve views, which both span resource elements, but not with the radar,
    // whose delay outputs span transform taps instead - so it is parked too,
    // and only swapped when the radar is on one side of the switch. Indexed by
    // visualizer_view_mode_t.
    float zoom_y_parked_center[VIEW_MODE_COUNT];
    float zoom_y_parked_span[VIEW_MODE_COUNT];
    // The horizontal window is parked by what the axis measures rather than by
    // view mode: the waterfall, curve and radar-CFR views all span resource
    // elements and should share one window, while the radar's delay outputs
    // need their own. Keying on the unit also covers the radar changing its own
    // horizontal axis when its output changes, which a view-mode switch never
    // sees. Slot 0 is resource elements, slot 1 delay taps.
    float zoom_x_parked_center[2];
    float zoom_x_parked_span[2];
    int zoom_x_parked_slot;

    int axes_enabled;      // margins reserved for axes; off makes the view full-bleed
    int axis_valid;
    int axis_rows;         // texture rows spanning the plot height
    int axis_phase_mode;   // rows are phases within the averaging period, not absolute symbols
    int axis_curve_mode;   // rows are magnitude, not time
    int axis_row0_value;   // absolute symbol (or phase) shown by row 0
    int axis_period;       // averaging period, phase mode only
    // Grid comes from the radar core, not the symbol ring, so markers and the
    // cursor readout sample the published radar frame instead of the ring.
    int axis_radar_mode;
    // What the axes measure this frame. The waterfall and curve views are
    // always RE-by-symbol, but the radar's grid changes with its output, so
    // ticks, the cursor readout and the zoom window all read these rather than
    // assuming the resource-element grid. See visualizer_grid_info_t.
    int axis_x_count;
    visualizer_axis_unit_t axis_x_unit;
    visualizer_axis_unit_t axis_y_unit;
    float axis_x_unit_scale;   // metres per delay tap, 0 when not applicable
    float axis_y_unit_scale;   // hertz per Doppler bin, 0 when not applicable
    int axis_y_scrolling;      // vertical axis advances with new symbols
    const char* axis_value_label;
    // Row the trigger line marks and marker offsets count from, or -1 when no
    // triggered window is on screen. Published with the frame, so everything
    // reading it describes the frame actually drawn.
    int axis_trigger_row;

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

// Resets the grid description to the resource-element-by-symbol layout the
// waterfall and curve views share. Each draw path calls this before publishing
// its own axis state, so a field only has to be set where it actually differs
// and nothing carries over from the previous frame's view.
static void axis_publish_defaults(opengl_visualizer_t* vis) {
    vis->axis_phase_mode = 0;
    vis->axis_curve_mode = 0;
    vis->axis_radar_mode = 0;
    vis->axis_period = 0;
    vis->axis_x_count = HEATMAP_TEXTURE_WIDTH;
    vis->axis_x_unit = AXIS_UNIT_RE;
    vis->axis_y_unit = AXIS_UNIT_SYMBOL;
    vis->axis_x_unit_scale = 0.0f;
    vis->axis_y_unit_scale = 0.0f;
    vis->axis_y_scrolling = 1;
    vis->axis_value_label = "magnitude";
    vis->axis_trigger_row = -1;
}

// Advances the display's normalisation toward the level just measured.
// Small changes are smoothed so the colours do not flicker frame to frame;
// a large change means the previous level is simply wrong for this signal,
// so it is taken immediately rather than eased into over many frames.
static float magnitude_scale_step(float current, float measured) {
    const float alpha_rise = 0.35f;
    const float alpha_fall = 0.08f;
    const float snap_ratio = 2.0f;  // a factor-of-two gap is a different signal, not drift

    if (current <= 1e-6f) {
        return measured;
    }
    if (measured > current * snap_ratio || measured < current / snap_ratio) {
        return measured;
    }
    const float alpha = (measured > current) ? alpha_rise : alpha_fall;
    return alpha * measured + (1.0f - alpha) * current;
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
    if (mode == vis->view_mode) {
        return;
    }

    if (mode < 0 || mode >= VIEW_MODE_COUNT) {
        return;
    }

    // Hand the vertical zoom window over to the incoming view's own, since rows
    // and magnitude are not interchangeable; see zoom_y_parked_center.
    const int leaving = (int)vis->view_mode;
    const int arriving = (int)mode;
    vis->zoom_y_parked_center[leaving] = vis->zoom_y_center;
    vis->zoom_y_parked_span[leaving] = vis->zoom_y_span;
    vis->zoom_y_center = vis->zoom_y_parked_center[arriving];
    vis->zoom_y_span = vis->zoom_y_parked_span[arriving];

    vis->view_mode = mode;
    // The horizontal window is handed over by zoom_x_park_on_unit_change once
    // the incoming view has published what its axis measures - which is not
    // known here, since the radar's horizontal axis depends on its output.
}

void visualizer_set_radar(opengl_visualizer_t* vis, struct fh_radar* radar) {
    if (!vis) {
        return;
    }
    vis->radar = radar;
}

// Marks every accumulator cell "never folded" and forgets which symbols went
// in, without touching allocation. Also forgets the last frame folded, so the
// frame currently published is folded afresh rather than skipped as seen.
static void radar_persist_clear_values(opengl_visualizer_t* vis) {
    if (vis->radar_persist_accum) {
        const size_t cells = (size_t)vis->radar_persist_period * (size_t)vis->radar_persist_cols;
        for (size_t i = 0; i < cells; i++) {
            vis->radar_persist_accum[i] = PERSIST_EMPTY_CELL;
        }
    }
    if (vis->radar_persist_tags) {
        for (int i = 0; i < RADAR_PERSIST_TAG_SLOTS; i++) {
            vis->radar_persist_tags[i] = INT_MIN;
        }
    }
    vis->radar_persist_frame_seen = 0;
    vis->radar_persist_has_folded = 0;
}

// Sizes the accumulator for frames cols wide, clearing it whenever it has to
// be (re)allocated. The CFR is always the full resource grid, but keying on
// the frame width means another output can reuse this later without being
// folded into an accumulator of the wrong shape. Returns 0 when there is no
// usable accumulator.
static int radar_persist_ensure(opengl_visualizer_t* vis, int cols) {
    if (vis->radar_persist_accum && vis->radar_persist_tags && vis->radar_persist_cols == cols) {
        return 1;
    }
    free(vis->radar_persist_accum);
    vis->radar_persist_accum = NULL;
    vis->radar_persist_cols = 0;

    if (vis->radar_persist_period <= 0 || cols <= 0) {
        return 0;
    }
    if (!vis->radar_persist_tags) {
        vis->radar_persist_tags = (int*)malloc(RADAR_PERSIST_TAG_SLOTS * sizeof(int));
        if (!vis->radar_persist_tags) {
            return 0;
        }
    }
    vis->radar_persist_accum =
        (float*)malloc((size_t)vis->radar_persist_period * (size_t)cols * sizeof(float));
    if (!vis->radar_persist_accum) {
        return 0;
    }
    vis->radar_persist_cols = cols;
    radar_persist_clear_values(vis);
    return 1;
}

void visualizer_set_radar_persistence_enabled(opengl_visualizer_t* vis, int enabled) {
    if (!vis) {
        return;
    }
    // Allocation waits for the first frame, which is what says how wide the
    // accumulator has to be.
    vis->radar_persist_enabled = enabled ? 1 : 0;
}

int visualizer_get_radar_persistence_enabled(const opengl_visualizer_t* vis) {
    return vis ? vis->radar_persist_enabled : 0;
}

void visualizer_set_radar_persistence_period(opengl_visualizer_t* vis, int period_symbols) {
    if (!vis) {
        return;
    }
    if (period_symbols < 1) {
        period_symbols = 1;
    }
    if (period_symbols == vis->radar_persist_period) {
        return;
    }
    vis->radar_persist_period = period_symbols;
    // Dropped rather than resized: every cell's phase changes with the period,
    // so nothing accumulated so far lands in the right row any more. The next
    // frame reallocates at the new size and refolds its whole window.
    free(vis->radar_persist_accum);
    vis->radar_persist_accum = NULL;
    vis->radar_persist_cols = 0;
    radar_persist_clear_values(vis);
}

int visualizer_get_radar_persistence_period(const opengl_visualizer_t* vis) {
    return vis ? vis->radar_persist_period : 0;
}

void visualizer_set_radar_persistence_n(opengl_visualizer_t* vis, int n) {
    if (!vis) {
        return;
    }
    vis->radar_persist_n = (n < 1) ? 1 : n;
}

int visualizer_get_radar_persistence_n(const opengl_visualizer_t* vis) {
    return vis ? vis->radar_persist_n : 0;
}

void visualizer_reset_radar_persistence(opengl_visualizer_t* vis) {
    if (!vis) {
        return;
    }
    radar_persist_clear_values(vis);
}

int visualizer_get_radar_persistence_periods(const opengl_visualizer_t* vis) {
    if (!vis || !vis->radar_persist_has_folded || vis->radar_persist_period <= 0) {
        return 0;
    }
    const int span = vis->radar_persist_last_symbol - vis->radar_persist_first_symbol;
    if (span < 0) {
        return 0;
    }
    return (span / vis->radar_persist_period) + 1;
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

// Re-seeds only the crossing detector: scan cursor and baseline. Deliberately
// leaves the capture alone. A discontinuity in symbol numbering says the scan
// cursor is lost, which is all this repairs - it says nothing about a capture
// the user asked to freeze, whose rows live in trigger_snapshot_* precisely so
// they outlive whatever the live ring does next.
static void trigger_reseed_detection(opengl_visualizer_t* vis) {
    vis->trigger_armed = 1;
    vis->trigger_needs_seed = 1;
    vis->trigger_has_last_checked = 0;
    vis->trigger_baseline_initialized = 0;
}

// A capture still filling when the numbering jumps can never finish: the rows it
// is waiting for do not exist under the new numbering, so it would sit in
// Capturing for good and, in Single mode, never reach Holding. Give it up and
// let the detector arm again. The fire count stands - it did fire.
static void trigger_abandon_incomplete_capture(opengl_visualizer_t* vis) {
    if (!vis->trigger_capture_active) {
        return;
    }
    vis->trigger_capture_active = 0;
    vis->trigger_captured_symbol = TRIGGER_NO_SYMBOL;
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
    if (!vis) {
        return;
    }
    // Reset even when the mode is unchanged, so re-picking the current mode
    // is always a way out of a stuck state rather than a no-op.
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

// Whether the reset level is in force. Single mode stops at its first capture
// and waits for a manual Re-arm, so there is no run of repeated firing for a
// reset level to steady; the stored choice is kept for Repeat.
static int trigger_hysteresis_active(const opengl_visualizer_t* vis) {
    return vis->trigger_hysteresis_enabled && vis->trigger_mode != TRIGGER_SINGLE;
}

void visualizer_set_trigger_hysteresis_enabled(opengl_visualizer_t* vis, int enabled) {
    if (!vis) {
        return;
    }
    if (enabled && vis->trigger_mode == TRIGGER_SINGLE) {
        return;  // cannot be turned on in Single; see trigger_hysteresis_active
    }
    vis->trigger_hysteresis_enabled = enabled ? 1 : 0;
}

int visualizer_get_trigger_hysteresis_enabled(const opengl_visualizer_t* vis) {
    return vis ? trigger_hysteresis_active(vis) : 0;
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
    
    GLFWwindow* window = glfwCreateWindow(width, height, "ORAN Fronthaul Visualizer", NULL, NULL);
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

    vis->radar_persist_enabled = 0;
    vis->radar_persist_period = RADAR_PERSIST_DEFAULT_PERIOD;
    vis->radar_persist_n = RADAR_PERSIST_DEFAULT_N;
    vis->radar_persist_cols = 0;
    vis->radar_persist_accum = NULL;
    vis->radar_persist_tags = NULL;
    vis->radar_persist_has_epoch = 0;
    vis->radar_persist_frame_seen = 0;
    vis->radar_persist_has_folded = 0;

    vis->curve_frame_traces = NULL;
    vis->curve_frame_trace_capacity = 0;
    vis->curve_frame_trace_count = 0;
    vis->curve_frame_mean = NULL;
    vis->curve_frame_has_mean = 0;

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
    vis->marker_count = 0;
    vis->display_mean_magnitude = 0.0f;
    vis->display_mean_valid = 0;
    visualizer_reset_zoom(vis);
    vis->axes_enabled = 1;
    axis_publish_defaults(vis);
    vis->axis_valid = 0;
    vis->axis_rows = 0;
    vis->axis_row0_value = 0;
    vis->trigger_snapshot_symbol = NULL;
    vis->trigger_snapshot_magnitude = NULL;
    trigger_snapshot_realloc(vis);

    // Allocate texture buffer (tex_width x tex_height)
    vis->tex_width = HEATMAP_TEXTURE_WIDTH;
    vis->texture_data = (uint8_t*)calloc((size_t)vis->tex_width * vis->tex_height * 4, sizeof(uint8_t));
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vis->tex_width, vis->tex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    
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

// --- Zoom ---
// Everything here works in normalized data coordinates: 0..1 across the fully
// zoomed-out extent of an axis, with y running downward so it matches both the
// waterfall's row order and the texture's t coordinate.

// Deep enough to resolve a handful of resource elements or symbol rows across
// the whole plot, which is as far as inspecting individual cells needs.
#define ZOOM_MIN_SPAN 0.002f

// Below this many pixels per texel, linear filtering is the smoother picture;
// above it the texels are big enough that interpolating between them invents
// gradients that aren't in the data, so switch to nearest and let cells read
// as the discrete samples they are.
#define ZOOM_NEAREST_FILTER_PX 2.5f

static float zoom_guard(float value, float fallback) {
    // Rejects NaN and infinities, which would otherwise propagate into the
    // window and blank the view with no way back short of Reset.
    if (!(value > -1e30f && value < 1e30f)) {
        return fallback;
    }
    return value;
}

// Fewest symbol rows the waterfall keeps in view. A window narrower than a
// single row contains no row boundary, so the vertical axis has no tick to hang
// the always-present position readout on - and there is nothing to see inside
// less than one row anyway. This has to be a count rather than a fraction
// because the row extent moves with Memory size, averaging period and capture
// depth: at 14 rows, a fraction that is generous at 1120 rows works out to a
// small part of a single row. The horizontal axis needs no equivalent, its
// extent being a compile-time constant.
#define ZOOM_MIN_VISIBLE_ROWS 2.0f

// Smallest vertical span allowed for the view as it currently stands.
static float zoom_y_min_span(const opengl_visualizer_t* vis) {
    // Curve view plots magnitude vertically, not rows, so there is no row
    // quantisation to respect and a very narrow magnitude window is legitimate.
    if (vis->view_mode == VIEW_MODE_CURVE) {
        return ZOOM_MIN_SPAN;
    }

    const int rows = (vis->axis_valid && vis->axis_rows > 0) ? vis->axis_rows : vis->tex_height;
    if (rows <= 0) {
        return ZOOM_MIN_SPAN;  // extent not established yet; first frame will re-clamp
    }

    float min_span = ZOOM_MIN_VISIBLE_ROWS / (float)rows;
    if (min_span > 1.0f) {
        min_span = 1.0f;  // fewer rows than the minimum: no room to zoom at all
    }
    if (min_span < ZOOM_MIN_SPAN) {
        min_span = ZOOM_MIN_SPAN;
    }
    return min_span;
}

// Forces one axis's window to be a real sub-window of [0,1]: no wider than the
// data, no narrower than min_span, and never hanging off either end.
static void zoom_normalize_axis(float* center, float* span, float min_span) {
    float s = zoom_guard(*span, 1.0f);
    if (s < min_span) {
        s = min_span;
    }
    if (s > 1.0f) {
        s = 1.0f;
    }

    float c = zoom_guard(*center, 0.5f);
    const float half = s * 0.5f;
    if (c < half) {
        c = half;
    }
    if (c > 1.0f - half) {
        c = 1.0f - half;
    }

    *span = s;
    *center = c;
}

// Smallest horizontal span allowed. The fixed fraction is fine across 3276
// resource elements, but the radar's delay axis can be a couple of hundred taps
// wide, where the same fraction is less than a single cell - so the horizontal
// axis gets the same cell-count quantisation the vertical one has.
static float zoom_x_min_span(const opengl_visualizer_t* vis) {
    const int columns = vis->axis_x_count > 0 ? vis->axis_x_count : HEATMAP_TEXTURE_WIDTH;
    float min_span = ZOOM_MIN_VISIBLE_ROWS / (float)columns;
    if (min_span > 1.0f) {
        min_span = 1.0f;
    }
    if (min_span < ZOOM_MIN_SPAN) {
        min_span = ZOOM_MIN_SPAN;
    }
    return min_span;
}

// Hands the horizontal window over whenever the axis changes what it measures,
// so a resource-element range is never silently reinterpreted as a range of
// delay taps. Driven by the published unit rather than by the view mode,
// because the radar swaps its own horizontal axis when its output changes and
// no mode switch occurs at all.
static void zoom_x_park_on_unit_change(opengl_visualizer_t* vis) {
    const int slot = (vis->axis_x_unit == AXIS_UNIT_DELAY_TAP) ? 1 : 0;
    if (slot == vis->zoom_x_parked_slot) {
        return;
    }
    vis->zoom_x_parked_center[vis->zoom_x_parked_slot] = vis->zoom_x_center;
    vis->zoom_x_parked_span[vis->zoom_x_parked_slot] = vis->zoom_x_span;
    vis->zoom_x_center = vis->zoom_x_parked_center[slot];
    vis->zoom_x_span = vis->zoom_x_parked_span[slot];
    vis->zoom_x_parked_slot = slot;
}

static void zoom_normalize(opengl_visualizer_t* vis) {
    zoom_x_park_on_unit_change(vis);
    zoom_normalize_axis(&vis->zoom_x_center, &vis->zoom_x_span, zoom_x_min_span(vis));
    zoom_normalize_axis(&vis->zoom_y_center, &vis->zoom_y_span, zoom_y_min_span(vis));
}

// Leading edge of the window on each axis, i.e. the data coordinate drawn at
// the left (x) or top (y) of the plot rect.
static float zoom_x_origin(const opengl_visualizer_t* vis) {
    return vis->zoom_x_center - vis->zoom_x_span * 0.5f;
}

static float zoom_y_origin(const opengl_visualizer_t* vis) {
    return vis->zoom_y_center - vis->zoom_y_span * 0.5f;
}

// Normalized data coordinate to screen pixels. Results outside the plot rect
// are meaningful and expected - callers either clip or skip on them.
static float zoom_x_to_px(const opengl_visualizer_t* vis, float x_norm, float plot_x, float plot_w) {
    const float span = vis->zoom_x_span > 1e-6f ? vis->zoom_x_span : 1e-6f;
    return plot_x + plot_w * (x_norm - zoom_x_origin(vis)) / span;
}

static float zoom_y_to_px(const opengl_visualizer_t* vis, float y_norm, float plot_y, float plot_h) {
    const float span = vis->zoom_y_span > 1e-6f ? vis->zoom_y_span : 1e-6f;
    return plot_y + plot_h * (y_norm - zoom_y_origin(vis)) / span;
}

// Curve view insets its trace a little at the top and bottom so a full-scale
// peak doesn't sit right on the frame. The inset is applied after the zoom
// transform rather than before it, which keeps it a constant visual margin
// instead of quietly eating into the magnitude range the zoom window asks for -
// so the window really does show the range the UI reports.
#define CURVE_Y_INSET 0.05f
#define CURVE_Y_USABLE 0.90f

// Normalized curve y (0 = full scale, at the top; 1 = zero magnitude, at the
// bottom) to screen pixels.
static float curve_y_to_px(const opengl_visualizer_t* vis, float y_norm, float plot_y, float plot_h) {
    const float span = vis->zoom_y_span > 1e-6f ? vis->zoom_y_span : 1e-6f;
    const float zoomed = (y_norm - zoom_y_origin(vis)) / span;
    return plot_y + (CURVE_Y_INSET + zoomed * CURVE_Y_USABLE) * plot_h;
}

// Restricts the GL scissor box to the plot rect. Needed wherever geometry is
// drawn from data coordinates that the zoom can push outside the plot - a line
// strip would otherwise run straight over the axis margins. GL's scissor
// origin is bottom-left, unlike this code's top-left screen space.
static void zoom_push_plot_scissor(const opengl_visualizer_t* vis) {
    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 1.0f, plot_h = 1.0f;
    visualizer_get_plot_rect(vis, &plot_x, &plot_y, &plot_w, &plot_h);

    const int x = (int)plot_x;
    const int w = (int)plot_w;
    const int h = (int)plot_h;
    const int y = vis->height - (int)plot_y - h;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y > 0 ? y : 0, w > 0 ? w : 1, h > 0 ? h : 1);
}

static void zoom_pop_plot_scissor(void) {
    glDisable(GL_SCISSOR_TEST);
}

void visualizer_set_zoom(opengl_visualizer_t* vis,
                         float x_center, float x_span,
                         float y_center, float y_span) {
    if (!vis) {
        return;
    }
    vis->zoom_x_center = x_center;
    vis->zoom_x_span = x_span;
    vis->zoom_y_center = y_center;
    vis->zoom_y_span = y_span;
    zoom_normalize(vis);
}

void visualizer_get_zoom(const opengl_visualizer_t* vis,
                         float* out_x_center, float* out_x_span,
                         float* out_y_center, float* out_y_span) {
    if (out_x_center) { *out_x_center = vis ? vis->zoom_x_center : 0.5f; }
    if (out_x_span) { *out_x_span = vis ? vis->zoom_x_span : 1.0f; }
    if (out_y_center) { *out_y_center = vis ? vis->zoom_y_center : 0.5f; }
    if (out_y_span) { *out_y_span = vis ? vis->zoom_y_span : 1.0f; }
}

void visualizer_reset_zoom(opengl_visualizer_t* vis) {
    if (!vis) {
        return;
    }
    vis->zoom_x_center = 0.5f;
    vis->zoom_x_span = 1.0f;
    vis->zoom_y_center = 0.5f;
    vis->zoom_y_span = 1.0f;
    // Also clears the other views' parked windows, so Reset means the full view
    // everywhere rather than leaving a surprise behind the next mode switch.
    for (int i = 0; i < VIEW_MODE_COUNT; i++) {
        vis->zoom_y_parked_center[i] = 0.5f;
        vis->zoom_y_parked_span[i] = 1.0f;
    }
    for (int i = 0; i < 2; i++) {
        vis->zoom_x_parked_center[i] = 0.5f;
        vis->zoom_x_parked_span[i] = 1.0f;
    }
}

int visualizer_zoom_is_active(const opengl_visualizer_t* vis) {
    if (!vis) {
        return 0;
    }
    return (vis->zoom_x_span < 0.9999f || vis->zoom_y_span < 0.9999f) ? 1 : 0;
}

void visualizer_zoom_by(opengl_visualizer_t* vis,
                        float x_factor, float y_factor,
                        float focus_x, float focus_y) {
    if (!vis) {
        return;
    }

    x_factor = zoom_guard(x_factor, 1.0f);
    y_factor = zoom_guard(y_factor, 1.0f);
    focus_x = zoom_guard(focus_x, 0.5f);
    focus_y = zoom_guard(focus_y, 0.5f);

    if (x_factor > 0.0f && x_factor != 1.0f) {
        const float span = vis->zoom_x_span;
        const float anchor = zoom_x_origin(vis) + focus_x * span;  // data under the focus point
        const float new_span = span / x_factor;
        vis->zoom_x_span = new_span;
        vis->zoom_x_center = anchor - focus_x * new_span + new_span * 0.5f;
    }
    if (y_factor > 0.0f && y_factor != 1.0f) {
        const float span = vis->zoom_y_span;
        const float anchor = zoom_y_origin(vis) + focus_y * span;
        const float new_span = span / y_factor;
        vis->zoom_y_span = new_span;
        vis->zoom_y_center = anchor - focus_y * new_span + new_span * 0.5f;
    }

    zoom_normalize(vis);
}

void visualizer_pan_by(opengl_visualizer_t* vis, float x_fraction, float y_fraction) {
    if (!vis) {
        return;
    }
    vis->zoom_x_center += zoom_guard(x_fraction, 0.0f) * vis->zoom_x_span;
    vis->zoom_y_center += zoom_guard(y_fraction, 0.0f) * vis->zoom_y_span;
    zoom_normalize(vis);
}

int visualizer_get_zoom_total_rows(const opengl_visualizer_t* vis) {
    if (!vis) {
        return 0;
    }
    if (vis->axis_valid && vis->axis_rows > 0) {
        return vis->axis_rows;
    }
    return vis->tex_height > 0 ? vis->tex_height : 0;
}

// Gathers what the Curve view draws this frame into curve_frame_*: magnitude
// vs. resource element for the newest symbol (or, with trailing traces on, the
// last N symbols read live from the ring), plus the optional rolling-mean
// line, and the scale they are drawn against. The mean is folded
// separately, in curve_advance.
static void curve_build_frame(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    vis->curve_frame_trace_count = 0;
    vis->curve_frame_has_mean = 0;
    if (!memory || !memory->symbols || memory->symbol_highest < memory->symbol_lowest) {
        return;  // no data yet
    }

    int trace_count = vis->curve_trace_enabled ? vis->curve_trace_count : 1;
    if (trace_count < 1) {
        trace_count = 1;
    }
    if (trace_count > CURVE_MAX_TRACES) {
        trace_count = CURVE_MAX_TRACES;
    }
    vis->curve_frame_trace_slots = trace_count;

    if (trace_count > vis->curve_frame_trace_capacity) {
        float* grown = (float*)realloc(vis->curve_frame_traces,
                                       (size_t)trace_count * HEATMAP_TEXTURE_WIDTH * sizeof(float));
        if (grown == NULL) {
            return;
        }
        vis->curve_frame_traces = grown;
        vis->curve_frame_trace_capacity = trace_count;
    }
    if (vis->curve_frame_mean == NULL) {
        vis->curve_frame_mean = (float*)malloc(HEATMAP_TEXTURE_WIDTH * sizeof(float));
        if (vis->curve_frame_mean == NULL) {
            return;
        }
    }

    // When trailing and mean are both on, the trail itself is built from the
    // mean accumulator's recent phase history - "a trail of averages" - in
    // place of raw traces plus one separate static mean line. With trailing
    // off, mean still shows as a single current line as before.
    const int show_mean_trail = vis->curve_mean_enabled && vis->curve_trace_enabled && vis->curve_mean_accum != NULL;
    const int show_single_mean_line = vis->curve_mean_enabled && !vis->curve_trace_enabled && vis->curve_mean_accum != NULL;
    const int mean_period = vis->curve_mean_period > 0 ? vis->curve_mean_period : 1;

    float max_magnitude = 1e-9f;
    int resolved = 0;
    for (int age = 0; age < trace_count; age++) {
        float* dst = vis->curve_frame_traces + (size_t)resolved * HEATMAP_TEXTURE_WIDTH;
        if (show_mean_trail) {
            // Each age steps back through the phase the mean accumulator held at
            // that point - e.g. age 0 is the current phase, age 1 the previous
            // symbol's phase, etc. Not bounded by the raw ring, since the mean
            // accumulator retains state independent of ring depth. In Running
            // Average mode (period forced to 1) every age maps to the same row,
            // so the "trail" degenerates to one visible line - expected, since
            // that mode has no history to show a trail of.
            int phase = (memory->symbol_highest - age) % mean_period;
            if (phase < 0) {
                phase += mean_period;
            }
            const float* row = vis->curve_mean_accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH;
            for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                dst[col] = (row[col] < 0.0f) ? 0.0f : row[col];  // never-folded sentinel
            }
        } else {
            size_t idx;
            if (!ring_row_for_symbol(memory, memory->symbol_highest - age, &idx)) {
                break;
            }
            for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                float i_val, q_val;
                extract_iq(memory->symbols[idx][col], i_val, q_val);
                dst[col] = sqrtf(i_val * i_val + q_val * q_val);
            }
        }
        for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
            if (dst[col] > max_magnitude) {
                max_magnitude = dst[col];
            }
        }
        resolved++;
    }
    vis->curve_frame_trace_count = resolved;

    if (show_single_mean_line) {
        int phase = memory->symbol_highest % mean_period;
        if (phase < 0) {
            phase += mean_period;
        }
        const float* row = vis->curve_mean_accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH;
        for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
            vis->curve_frame_mean[col] = (row[col] < 0.0f) ? 0.0f : row[col];
            if (vis->curve_frame_mean[col] > max_magnitude) {
                max_magnitude = vis->curve_frame_mean[col];
            }
        }
        vis->curve_frame_has_mean = 1;
    }

    if (resolved == 0 && !vis->curve_frame_has_mean) {
        return;
    }

    // Smooth magnitude scaling, same rise/fall EMA convention as the
    // waterfall's magnitude_scale_ema, but on the curve's own independent field.
    if (!vis->curve_magnitude_scale_initialized) {
        vis->curve_magnitude_scale_ema = max_magnitude;
        vis->curve_magnitude_scale_initialized = 1;
    } else {
        vis->curve_magnitude_scale_ema = magnitude_scale_step(vis->curve_magnitude_scale_ema, max_magnitude);
    }
    vis->curve_frame_scale = (vis->curve_magnitude_scale_ema > 1e-6f) ? vis->curve_magnitude_scale_ema : 1e-6f;
}

// Folds new symbols into the Curve view's rolling mean.
static void curve_advance(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!memory || !memory->symbols || memory->symbol_highest < memory->symbol_lowest) {
        return;
    }
    if (vis->curve_mean_enabled) {
        curve_mean_fold_new_symbols(vis, memory);
    }
}

// Draws the frame in curve_frame_* as a 2D plot, replacing the heatmap quad
// when view_mode == VIEW_MODE_CURVE; called from visualizer_update() after
// glClear.
static void curve_draw_frame(opengl_visualizer_t* vis) {
    const int resolved = vis->curve_frame_trace_count;
    const float* single_mean_row = vis->curve_frame_has_mean ? vis->curve_frame_mean : NULL;
    if (resolved == 0 && single_mean_row == NULL) {
        return;
    }
    const int trace_count = vis->curve_frame_trace_slots > 0 ? vis->curve_frame_trace_slots : 1;
    const float render_scale = vis->curve_frame_scale;

    // glLineWidth persists across frames (unlike blend/texture enable state,
    // which ImGui's own glPushAttrib/glPopAttrib restores), so it is set
    // explicitly before every glBegin(GL_LINE_STRIP) call below.
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 1.0f, plot_h = 1.0f;
    visualizer_get_plot_rect(vis, &plot_x, &plot_y, &plot_w, &plot_h);

    // Only the columns in view need drawing, plus one either side so the line
    // enters and leaves the plot at the right slope. Without that bound a deep
    // zoom would push most vertices thousands of screen-widths away.
    const float x_norm_step = 1.0f / (float)(HEATMAP_TEXTURE_WIDTH - 1);
    int col_first = (int)floorf(zoom_x_origin(vis) / x_norm_step) - 1;
    int col_last = (int)ceilf((zoom_x_origin(vis) + vis->zoom_x_span) / x_norm_step) + 1;
    if (col_first < 0) {
        col_first = 0;
    }
    if (col_last > HEATMAP_TEXTURE_WIDTH - 1) {
        col_last = HEATMAP_TEXTURE_WIDTH - 1;
    }

    // Clipped to the plot rect: zoomed vertices routinely fall outside it and
    // must not be drawn over the axis margins.
    zoom_push_plot_scissor(vis);

    axis_publish_defaults(vis);
    vis->axis_valid = 1;
    vis->axis_rows = vis->tex_height > 0 ? vis->tex_height : 1;
    vis->axis_curve_mode = 1;
    vis->axis_y_unit = AXIS_UNIT_MAGNITUDE;
    vis->axis_y_scrolling = 0;

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

        const float* trace = vis->curve_frame_traces + (size_t)t * HEATMAP_TEXTURE_WIDTH;
        glLineWidth(line_width);
        glColor4f(r, g, b, alpha);
        glBegin(GL_LINE_STRIP);
        for (int col = col_first; col <= col_last; col++) {
            float norm = trace[col] / render_scale;
            if (norm > 1.0f) {
                norm = 1.0f;
            }
            glVertex2f(zoom_x_to_px(vis, (float)col * x_norm_step, plot_x, plot_w),
                       curve_y_to_px(vis, 1.0f - norm, plot_y, plot_h));
        }
        glEnd();
    }

    if (single_mean_row != NULL) {
        glLineWidth(2.5f);
        glColor4f(1.00f, 0.85f, 0.25f, 1.0f);
        glBegin(GL_LINE_STRIP);
        for (int col = col_first; col <= col_last; col++) {
            float norm = single_mean_row[col] / render_scale;
            if (norm > 1.0f) {
                norm = 1.0f;
            }
            glVertex2f(zoom_x_to_px(vis, (float)col * x_norm_step, plot_x, plot_w),
                       curve_y_to_px(vis, 1.0f - norm, plot_y, plot_h));
        }
        glEnd();
    }

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glDisable(GL_BLEND);
    zoom_pop_plot_scissor();
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
// Streams number their symbols independently, so state carried over from a
// different one leaves the scan position outside the window that actually
// exists. Treat that as a reason to re-seed rather than trying to scan a
// range that can never match, which would otherwise wedge the trigger.
static int trigger_numbering_is_stale(const opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis->trigger_has_last_checked) {
        return 0;
    }
    if (vis->trigger_last_checked_symbol > memory->symbol_highest) {
        return 1;
    }
    if (vis->trigger_last_checked_symbol < memory->symbol_lowest - TRIGGER_MAX_SCAN_SYMBOLS) {
        return 1;
    }
    // A captured symbol numbered beyond the newest one can only mean the stream
    // restarted its numbering underneath us, which does invalidate the scan
    // position. Deliberately no test for the captured symbol having fallen off
    // the old end: that is just the ring moving on, which happens to every
    // capture within a second or so of firing, and a completed capture does not
    // depend on the ring - its rows are locked into trigger_snapshot_* for
    // exactly this reason. Treating it as stale used to reset the trigger, and
    // with it trigger_single_done, so a Single capture re-armed and fired again
    // roughly every TRIGGER_MAX_SCAN_SYMBOLS + memory-size symbols instead of
    // holding until rearmed.
    if (vis->trigger_captured_symbol != TRIGGER_NO_SYMBOL &&
        vis->trigger_captured_symbol > memory->symbol_highest) {
        return 1;
    }
    return 0;
}

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

    // Numbering discontinuity: repair the scan cursor without throwing away a
    // capture. Wiping everything here is what made a held Single capture vanish
    // and its count restart on every pcap replay loop, where the offset shifts
    // every symbol number at once.
    if (trigger_numbering_is_stale(vis, memory)) {
        trigger_abandon_incomplete_capture(vis);
        trigger_reseed_detection(vis);
    }

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
        const float reset_threshold = trigger_hysteresis_active(vis)
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

    // Numbering discontinuity: repair the scan cursor without throwing away a
    // capture. Wiping everything here is what made a held Single capture vanish
    // and its count restart on every pcap replay loop, where the offset shifts
    // every symbol number at once.
    if (trigger_numbering_is_stale(vis, memory)) {
        trigger_abandon_incomplete_capture(vis);
        trigger_reseed_detection(vis);
    }

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
    // Never let the margin grow past what the ring can hold, or a capture
    // depth close to (or beyond) the memory size would put every symbol out
    // of reach and the trigger could never fire at all.
    int firable_margin = vis->trigger_capture_depth / 2;
    const int window_span = memory->symbol_highest - memory->symbol_lowest;
    if (firable_margin > window_span / 2) {
        firable_margin = window_span / 2;
    }
    const int oldest_firable_symbol = memory->symbol_lowest + firable_margin;

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
        const float reset_threshold = trigger_hysteresis_active(vis)
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

// Distinct hues so adjacent markers stay tellable apart at a glance.
static const unsigned char MARKER_PALETTE[][3] = {
    { 255, 80, 80 },   { 80, 200, 255 },  { 120, 255, 120 }, { 255, 200, 60 },
    { 220, 120, 255 }, { 255, 140, 40 },  { 80, 255, 220 },  { 255, 105, 180 },
    { 160, 160, 255 }, { 200, 255, 80 },  { 255, 255, 255 }, { 150, 220, 180 },
};

int visualizer_add_marker(opengl_visualizer_t* vis) {
    if (!vis || vis->marker_count >= VISUALIZER_MAX_MARKERS) {
        return -1;
    }
    const int index = vis->marker_count++;
    visualizer_marker_info_t* m = &vis->markers[index];
    memset(m, 0, sizeof(*m));
    // Drop a new marker in the middle of the grid at the reference row, so it
    // lands somewhere visible rather than needing to be hunted for.
    m->re = HEATMAP_TEXTURE_WIDTH / 2;
    m->time_offset = 0;
    const size_t palette_size = sizeof(MARKER_PALETTE) / sizeof(MARKER_PALETTE[0]);
    m->color[0] = MARKER_PALETTE[index % palette_size][0];
    m->color[1] = MARKER_PALETTE[index % palette_size][1];
    m->color[2] = MARKER_PALETTE[index % palette_size][2];
    return index;
}

void visualizer_remove_marker(opengl_visualizer_t* vis, int index) {
    if (!vis || index < 0 || index >= vis->marker_count) {
        return;
    }
    for (int i = index; i < vis->marker_count - 1; i++) {
        vis->markers[i] = vis->markers[i + 1];
        // Keep colours tied to list position so numbering and colour agree.
        const size_t palette_size = sizeof(MARKER_PALETTE) / sizeof(MARKER_PALETTE[0]);
        vis->markers[i].color[0] = MARKER_PALETTE[i % palette_size][0];
        vis->markers[i].color[1] = MARKER_PALETTE[i % palette_size][1];
        vis->markers[i].color[2] = MARKER_PALETTE[i % palette_size][2];
    }
    vis->marker_count--;
}

int visualizer_get_marker_count(const opengl_visualizer_t* vis) {
    return vis ? vis->marker_count : 0;
}

int visualizer_get_marker(const opengl_visualizer_t* vis, int index, visualizer_marker_info_t* out_info) {
    if (!vis || !out_info || index < 0 || index >= vis->marker_count) {
        return 0;
    }
    *out_info = vis->markers[index];
    return 1;
}

void visualizer_set_marker(opengl_visualizer_t* vis, int index, int re, int time_offset) {
    if (!vis || index < 0 || index >= vis->marker_count) {
        return;
    }
    // Clamped to the grid actually on screen, which is narrower than the
    // resource grid whenever the radar is showing a truncated delay axis.
    const int columns = vis->axis_x_count > 0 ? vis->axis_x_count : HEATMAP_TEXTURE_WIDTH;
    if (re < 0) {
        re = 0;
    }
    if (re >= columns) {
        re = columns - 1;
    }
    vis->markers[index].re = re;
    vis->markers[index].time_offset = time_offset;
}

const char* visualizer_get_marker_reference_label(const opengl_visualizer_t* vis) {
    if (!vis) {
        return "newest symbol";
    }
    // Follows what the last frame drew rather than the live settings, the same
    // way markers_evaluate picks its reference row.
    if (vis->axis_trigger_row >= 0) {
        return "trigger point";
    }
    return vis->axis_phase_mode ? "last row in period" : "newest symbol";
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
    visualizer_grid_info_t info;
    visualizer_get_grid_info(vis, &info);
    if (out_y_title) {
        *out_y_title = info.y_title;
    }
    if (out_x_title) {
        *out_x_title = info.x_title;
    }
}

void visualizer_get_grid_info(const opengl_visualizer_t* vis, visualizer_grid_info_t* out) {
    if (out == NULL) {
        return;
    }

    out->x_count = HEATMAP_TEXTURE_WIDTH;
    out->y_count = 1;
    out->x_unit = AXIS_UNIT_RE;
    out->y_unit = AXIS_UNIT_SYMBOL;
    out->x_title = "PRB / RE";
    out->y_title = "Frame / slot";
    out->value_label = "magnitude";
    out->x_unit_scale = 0.0f;
    out->y_unit_scale = 0.0f;
    out->y_is_scrolling = 1;

    if (vis == NULL) {
        return;
    }

    out->x_count = vis->axis_x_count > 0 ? vis->axis_x_count : HEATMAP_TEXTURE_WIDTH;
    out->y_count = vis->axis_rows > 0 ? vis->axis_rows : 1;
    out->x_unit = vis->axis_x_unit;
    out->y_unit = vis->axis_y_unit;
    out->x_unit_scale = vis->axis_x_unit_scale;
    out->y_unit_scale = vis->axis_y_unit_scale;
    out->y_is_scrolling = vis->axis_y_scrolling;
    out->value_label = vis->axis_value_label ? vis->axis_value_label : "magnitude";

    switch (vis->axis_x_unit) {
    case AXIS_UNIT_DELAY_TAP:
        out->x_title = (vis->axis_x_unit_scale > 0.0f) ? "Delay tap / range" : "Delay tap";
        break;
    case AXIS_UNIT_RE:
    default:
        out->x_title = "PRB / RE";
        break;
    }

    switch (vis->axis_y_unit) {
    case AXIS_UNIT_MAGNITUDE:
        out->y_title = "Magnitude";
        break;
    case AXIS_UNIT_DOPPLER_BIN:
        out->y_title = (vis->axis_y_unit_scale > 0.0f) ? "Doppler (Hz)" : "Doppler bin";
        break;
    case AXIS_UNIT_SYMBOL:
    default:
        out->y_title = vis->axis_phase_mode ? "Offset in period" : "Frame / slot";
        break;
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
    // Rows per pixel follows the zoom: a narrower window spreads the same rows
    // further apart, which is what lets the density rules below promote finer
    // divisions into view as you zoom in.
    const float px_per_row = plot_h / (vis->zoom_y_span * (float)vis->axis_rows);
    int count = 0;

    // Range-Doppler: rows are frequency bins covering the whole interval at
    // once, so there is no frame/slot structure and nothing scrolls. Zero
    // Doppler sits at the centre row, with approaching targets above it and
    // receding ones below.
    if (vis->axis_y_unit == AXIS_UNIT_DOPPLER_BIN) {
        const int bins = vis->axis_rows;
        const float bin_origin = zoom_y_origin(vis) * (float)bins;
        const float bin_extent = vis->zoom_y_span * (float)bins;
        const float px_per_bin = plot_h / (bin_extent > 1e-6f ? bin_extent : 1e-6f);
        const int step = axis_nice_multiple(px_per_bin, AXIS_MIN_LABEL_PX_Y);

        int first = (int)floorf(bin_origin) - 1;
        int last = (int)ceilf(bin_origin + bin_extent) + 1;
        if (first < 0) {
            first = 0;
        }
        if (last > bins - 1) {
            last = bins - 1;
        }

        const int centre = bins / 2;
        for (int bin = (first / step) * step; bin <= last && count < max_ticks; bin += step) {
            if (bin < first) {
                continue;
            }
            visualizer_axis_tick_t* t = &out_ticks[count++];
            // Cell centre, as on the other discrete axes.
            t->position_px = plot_y + ((float)bin + 0.5f - bin_origin) * px_per_bin;
            t->level = (bin == centre) ? 2 : 1;
            t->has_label = 1;
            t->is_anchor = (bin == centre);
            if (vis->axis_y_unit_scale > 0.0f) {
                const double hz = (double)(centre - bin) * (double)vis->axis_y_unit_scale;
                if (fabs(hz) >= 1000.0) {
                    snprintf(t->label, sizeof(t->label), "%+.1fk", hz / 1000.0);
                } else {
                    snprintf(t->label, sizeof(t->label), "%+.0f", hz);
                }
            } else {
                snprintf(t->label, sizeof(t->label), "%d", centre - bin);
            }
            t->label2[0] = '\0';
            t->label3[0] = '\0';
        }
        return count;
    }

    // Curve view: the vertical axis is normalised magnitude, so it gets
    // percentage gridlines, stepped by however fine the zoom can show.
    if (vis->axis_curve_mode) {
        const float px_per_pct = plot_h * (CURVE_Y_USABLE / 100.0f) / vis->zoom_y_span;
        const int pct_step = axis_nice_multiple(px_per_pct, AXIS_MIN_LABEL_PX_Y);

        for (int pct = 0; pct <= 100 && count < max_ticks; pct += pct_step) {
            const float pos = curve_y_to_px(vis, 1.0f - (float)pct / 100.0f, plot_y, plot_h);
            if (pos < plot_y - 0.5f || pos > plot_y + plot_h + 0.5f) {
                continue;  // outside the zoom window
            }
            visualizer_axis_tick_t* t = &out_ticks[count++];
            t->position_px = pos;
            t->level = (pct == 0 || pct == 100) ? 2 : 1;
            t->has_label = 1;
            t->is_anchor = 0;
            snprintf(t->label, sizeof(t->label), "%d%%", pct);
            t->label2[0] = '\0';
            t->label3[0] = '\0';
        }
        return count;
    }

    // Rows actually inside the zoom window, so the loops below neither emit
    // ticks for rows that aren't drawn nor overflow out_ticks at deep zoom.
    // One row of slack each side keeps a tick that is only partly on screen.
    const float row_origin = zoom_y_origin(vis) * (float)vis->axis_rows;
    const float row_extent = vis->zoom_y_span * (float)vis->axis_rows;
    int visible_row_first = (int)floorf(row_origin) - 1;
    int visible_row_last = (int)ceilf(row_origin + row_extent) + 1;
    if (visible_row_first < 0) {
        visible_row_first = 0;
    }
    if (visible_row_last > vis->axis_rows - 1) {
        visible_row_last = vis->axis_rows - 1;
    }

    // Screen position of a row, given the zoom window. Centre of the row rather
    // than its top edge: a symbol row is a discrete cell, and markers, the
    // cursor readout and the trigger line all address it at its centre, so an
    // edge-aligned tick would sit half a row away from every one of them.
    const float row_to_px_scale = plot_h / (row_extent > 1e-6f ? row_extent : 1e-6f);
    const auto row_to_px = [&](int row) {
        return plot_y + ((float)row + 0.5f - row_origin) * row_to_px_scale;
    };

    // Last row that overlaps the window, which carries the always-present
    // spelled-out readout. Deliberately not visible_row_last: that one includes
    // a row of slack past the window edge so partially-scrolled ticks still get
    // drawn, and a tick placed there lands below the plot rect, where the
    // caller filters it out - which silently dropped this readout on any zoom
    // that wasn't flush against the bottom. Taken from the window bounds
    // instead, so its position is always strictly inside the plot.
    //
    // Rows are addressed at their centre, so this is the last row whose centre
    // still falls inside the window: ceil() here would pick a row centred up to
    // half a row past the bottom edge, which is far enough to be filtered out
    // again once the zoom makes rows tall.
    int anchor_row = (int)floorf(row_origin + row_extent - 0.5f);
    if (anchor_row > vis->axis_rows - 1) {
        anchor_row = vis->axis_rows - 1;
    }
    if (anchor_row < 0) {
        anchor_row = 0;
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

        // The bottom row in view always carries a fully spelled-out label, so
        // the view is never left without a position readout when it spans less
        // than one division. Zoomed in, that is the bottom of the window rather
        // than the bottom of the data.
        if (anchor_row >= 0 && count < max_ticks) {
            int phase = (vis->axis_row0_value + anchor_row) % period;
            if (phase < 0) {
                phase += period;
            }
            visualizer_axis_tick_t* t = &out_ticks[count++];
            t->position_px = row_to_px(anchor_row);
            t->level = 3;
            t->has_label = 1;
            t->is_anchor = 1;
            snprintf(t->label, sizeof(t->label), "phase +%d", phase);
            if (subframe_useful) {
                snprintf(t->label2, sizeof(t->label2), "sf+%d sl+%d",
                         phase / symbols_per_subframe,
                         (phase % symbols_per_subframe) / SYMBOLS_PER_SLOT);
            } else {
                snprintf(t->label2, sizeof(t->label2), "sl+%d", phase / SYMBOLS_PER_SLOT);
            }
            snprintf(t->label3, sizeof(t->label3), "sym%d", phase % SYMBOLS_PER_SLOT);
        }

        for (int row = visible_row_first; row <= visible_row_last && count < max_ticks; row++) {
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
            t->position_px = row_to_px(row);
            t->level = level;
            t->has_label = labelled;
            t->is_anchor = 0;
            t->label2[0] = '\0';
            t->label3[0] = '\0';
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

    // The bottom row in view always gets the complete position spelled out, so
    // a view spanning less than a whole frame still shows which frame,
    // subframe, slot and symbol it is looking at. Zoomed in, that is the bottom
    // of the window rather than the newest symbol.
    const int anchor_symbol = vis->axis_row0_value + anchor_row;
    if (anchor_row >= 0 && anchor_symbol >= 0 && count < max_ticks) {
        visualizer_axis_tick_t* t = &out_ticks[count++];
        t->position_px = row_to_px(anchor_row);
        t->level = 3;
        t->has_label = 1;
        t->is_anchor = 1;
        if (symbols_per_frame > 0) {
            const unsigned long long frame = (unsigned long long)(anchor_symbol / symbols_per_frame);
            const int in_frame = anchor_symbol % symbols_per_frame;
            snprintf(t->label, sizeof(t->label), "F%llu", frame);
            snprintf(t->label2, sizeof(t->label2), "(%llu)", frame % 256ULL);
            snprintf(t->label3, sizeof(t->label3), "sf%d sl%d sym%d",
                     in_frame / symbols_per_subframe,
                     (in_frame % symbols_per_subframe) / SYMBOLS_PER_SLOT,
                     anchor_symbol % SYMBOLS_PER_SLOT);
        } else {
            // Subcarrier spacing unknown, so frame structure can't be resolved.
            snprintf(t->label, sizeof(t->label), "sym %d", anchor_symbol);
            snprintf(t->label2, sizeof(t->label2), "sl%d sym%d",
                     anchor_symbol / SYMBOLS_PER_SLOT, anchor_symbol % SYMBOLS_PER_SLOT);
            t->label3[0] = '\0';
        }
    }

    for (int row = visible_row_first; row <= visible_row_last && count < max_ticks; row++) {
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
        t->position_px = row_to_px(row);
        t->level = level;
        t->has_label = labelled;
        t->is_anchor = 0;
        t->label2[0] = '\0';
        t->label3[0] = '\0';

        if (!labelled) {
            t->label[0] = '\0';
        } else if (level == 3) {
            const unsigned long long frame = (unsigned long long)(symbol / symbols_per_frame);
            // Both numbers matter: the absolute one keeps counting, the
            // parenthesised one is the 8-bit id actually carried on the wire.
            // Stacked rather than side by side so the pair stays narrow.
            snprintf(t->label, sizeof(t->label), "F%llu", frame);
            snprintf(t->label2, sizeof(t->label2), "(%llu)", frame % 256ULL);
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

    // Delay taps have no twelve-to-a-group structure to hang a second level of
    // ticks on, so they get a plain index ladder, with range in metres on the
    // second label line when the subcarrier spacing is known.
    if (vis->axis_x_unit == AXIS_UNIT_DELAY_TAP) {
        const int taps = vis->axis_x_count > 0 ? vis->axis_x_count : 1;
        const float tap_origin = zoom_x_origin(vis) * (float)taps;
        const float tap_extent = vis->zoom_x_span * (float)taps;
        const float px_per_tap = plot_w / (tap_extent > 1e-6f ? tap_extent : 1e-6f);
        const int step = axis_nice_multiple(px_per_tap, AXIS_MIN_LABEL_PX_X);

        int first = (int)floorf(tap_origin) - 1;
        int last = (int)ceilf(tap_origin + tap_extent) + 1;
        if (first < 0) {
            first = 0;
        }
        if (last > taps - 1) {
            last = taps - 1;
        }

        int count = 0;
        const int start = (first / step) * step;
        for (int tap = start; tap <= last && count < max_ticks; tap += step) {
            if (tap < first) {
                continue;
            }
            visualizer_axis_tick_t* t = &out_ticks[count++];
            // Cell centre, as on the resource-element axis above.
            t->position_px = plot_x + ((float)tap + 0.5f - tap_origin) * px_per_tap;
            t->level = 1;
            t->has_label = 1;
            t->is_anchor = 0;
            snprintf(t->label, sizeof(t->label), "%d", tap);
            if (vis->axis_x_unit_scale > 0.0f) {
                snprintf(t->label2, sizeof(t->label2), "%.0fm",
                         (double)((float)tap * vis->axis_x_unit_scale));
            } else {
                t->label2[0] = '\0';
            }
            t->label3[0] = '\0';
        }
        return count;
    }

    // Narrowed by the zoom, which everything below adapts to: wider spacing
    // promotes finer divisions into view and thins out crowded ones.
    // re_origin is fractional, so ticks stay put rather than snapping to whole
    // resource elements as the window slides.
    const float re_origin = zoom_x_origin(vis) * (float)HEATMAP_TEXTURE_WIDTH;
    const float re_extent = vis->zoom_x_span * (float)HEATMAP_TEXTURE_WIDTH;
    int re_first = (int)floorf(re_origin) - 1;
    int re_last = (int)ceilf(re_origin + re_extent) + 1;
    if (re_first < 0) {
        re_first = 0;
    }
    if (re_last > HEATMAP_TEXTURE_WIDTH - 1) {
        re_last = HEATMAP_TEXTURE_WIDTH - 1;
    }
    const float px_per_re = plot_w / (re_extent > 1e-6f ? re_extent : 1e-6f);
    const float px_per_prb = px_per_re * (float)HEATMAP_SAMPLES_PER_PRB;

    int count = 0;

    // Individual resource elements only get their own ticks once a PRB is
    // wide enough to show its twelve subcarriers without them merging.
    int show_re = px_per_re >= AXIS_MIN_TICK_PX;
    const int label_re = px_per_re >= 20.0f;

    int prb_step = axis_nice_multiple(px_per_prb, AXIS_MIN_LABEL_PX_X);
    const int prb_first = re_first / HEATMAP_SAMPLES_PER_PRB;
    const int prb_last = re_last / HEATMAP_SAMPLES_PER_PRB;

    // Thin the ladder down to what the caller's array can hold, rather than
    // letting the loop below stop when it fills up. Running out mid-sweep
    // leaves the right-hand part of the axis with no ticks at all, which is
    // what a wide window hits at high zoom: every visible PRB emits all twelve
    // of its resource elements, so plot_w / AXIS_MIN_TICK_PX ticks are needed
    // and anything past about 1400 px overruns. Dropping the finest level and
    // then coarsening the PRB step degrades the axis evenly instead.
    const int prb_span = prb_last - prb_first + 1;
    if (prb_span > 0) {
        if (show_re && prb_span * HEATMAP_SAMPLES_PER_PRB > max_ticks) {
            show_re = 0;
        }
        while (prb_step > 0 && (prb_span / prb_step) + 1 > max_ticks) {
            prb_step *= 2;
        }
    }

    for (int prb = prb_first; prb <= prb_last && count < max_ticks; prb++) {
        const int on_step = (prb % prb_step) == 0;
        if (!on_step && !show_re) {
            continue;
        }

        const int re = prb * HEATMAP_SAMPLES_PER_PRB;
        visualizer_axis_tick_t* t = &out_ticks[count++];
        // Centre of the cell, not its leading edge. A resource element is a
        // discrete cell rather than a point on a continuous scale, so a tick
        // labelled with its index has to point at the cell it names - which is
        // also where markers, the cursor readout and the trigger line put it.
        // Edge-aligned ticks sit half a cell away from all three, so a marker
        // looks like it is straddling a boundary.
        t->position_px = plot_x + ((float)re + 0.5f - re_origin) * px_per_re;
        t->level = 1;
        t->has_label = on_step;
        t->is_anchor = 0;
        t->label3[0] = '\0';
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
            st->position_px = plot_x + ((float)(re + sub) + 0.5f - re_origin) * px_per_re;
            st->level = 0;
            st->has_label = label_re;
            st->is_anchor = 0;
            st->label2[0] = '\0';
            st->label3[0] = '\0';
            if (label_re) {
                snprintf(st->label, sizeof(st->label), "%d", sub);
            } else {
                st->label[0] = '\0';
            }
        }
    }

    return count;
}

// Re-sample every marker against the view that was just rendered, so each
// one reports whatever that view is showing: the rolling mean in persistence
// mode, the held snapshot in a triggered one, or the live ring otherwise.
// Magnitude of one cell of whatever the waterfall is currently showing: the
// rolling-mean accumulator in persistence mode, the held snapshot in a
// triggered one, or the live ring otherwise. Shared by markers and the
// value-at-cursor readout so the two can never disagree about what a cell
// holds. Returns 1 and sets *out_magnitude when the cell carries data.
static int display_cell_magnitude(const opengl_visualizer_t* vis,
                                  const fh_oran_memory_t* memory,
                                  int row,
                                  int re,
                                  float* out_magnitude) {
    const int columns = vis->axis_x_count > 0 ? vis->axis_x_count : HEATMAP_TEXTURE_WIDTH;
    if (re < 0 || re >= columns || row < 0) {
        return 0;
    }

    // The radar view draws a matrix the symbol ring knows nothing about, so its
    // cells are read back from the published frame instead. Borrowing per
    // lookup is a mutex pair for a handful of cells per frame; the alternative
    // is keeping a second copy of a matrix that can run to tens of megabytes.
    if (vis->axis_radar_mode) {
        // With the rolling mean on screen, a cell reads back from the
        // accumulator, not from the live frame underneath it.
        if (vis->axis_phase_mode) {
            if (vis->radar_persist_accum == NULL || vis->axis_period <= 0 || re >= vis->radar_persist_cols) {
                return 0;
            }
            int phase = (vis->axis_row0_value + row) % vis->axis_period;
            if (phase < 0) {
                phase += vis->axis_period;
            }
            // The period can be edited between the frame that published the
            // axes and this lookup.
            if (phase >= vis->radar_persist_period) {
                return 0;
            }
            const float value = vis->radar_persist_accum[(size_t)phase * (size_t)vis->radar_persist_cols + re];
            if (value < 0.0f) {
                return 0;  // no estimate has reached this cell yet
            }
            *out_magnitude = value;
            return 1;
        }

        if (vis->radar == NULL) {
            return 0;
        }
        int rows = 0;
        int cols = 0;
        const float* frame = fh_radar_acquire_result(vis->radar, &rows, &cols);
        if (frame == NULL) {
            return 0;
        }
        int found = 0;
        if (row < rows && re < cols) {
            *out_magnitude = frame[(size_t)row * (size_t)cols + (size_t)re];
            found = 1;
        }
        fh_radar_release_result(vis->radar);
        return found;
    }

    const int showing_triggered = vis->axis_trigger_row >= 0;

    if (vis->axis_phase_mode && vis->persist_accum && vis->axis_period > 0) {
        int phase = (vis->axis_row0_value + row) % vis->axis_period;
        if (phase < 0) {
            phase += vis->axis_period;
        }
        const float value = vis->persist_accum[(size_t)phase * HEATMAP_TEXTURE_WIDTH + re];
        if (value < 0.0f) {
            return 0;  // negative marks a cell that never received a symbol
        }
        *out_magnitude = value;
        return 1;
    }

    if (showing_triggered && vis->trigger_snapshot_symbol && vis->trigger_snapshot_magnitude &&
        row < vis->trigger_capture_depth) {
        if (vis->trigger_snapshot_symbol[row] == TRIGGER_NO_SYMBOL) {
            return 0;
        }
        *out_magnitude = vis->trigger_snapshot_magnitude[(size_t)row * HEATMAP_TEXTURE_WIDTH + re];
        return 1;
    }

    if (memory && memory->symbols) {
        size_t idx;
        if (ring_row_for_symbol(memory, vis->axis_row0_value + row, &idx)) {
            float i_val, q_val;
            extract_iq(memory->symbols[idx][re], i_val, q_val);
            *out_magnitude = sqrtf(i_val * i_val + q_val * q_val);
            return 1;
        }
    }

    return 0;
}

// Mean magnitude of what is on screen, which marker and cursor percentages are
// measured against. 0 when no meaningful baseline has been computed yet.
static float display_baseline(const opengl_visualizer_t* vis) {
    return (vis->display_mean_valid && vis->display_mean_magnitude > 1e-6f)
               ? vis->display_mean_magnitude
               : 0.0f;
}

static void markers_evaluate(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 1.0f, plot_h = 1.0f;
    visualizer_get_plot_rect(vis, &plot_x, &plot_y, &plot_w, &plot_h);

    const int rows = vis->axis_rows > 0 ? vis->axis_rows : 1;
    const int columns = vis->axis_x_count > 0 ? vis->axis_x_count : HEATMAP_TEXTURE_WIDTH;
    // Offsets are measured from the trigger row when one is on screen,
    // otherwise from the bottom row, which carries the newest symbol.
    const int reference_row = (vis->axis_trigger_row >= 0) ? vis->axis_trigger_row : (rows - 1);

    const float baseline = display_baseline(vis);

    for (int i = 0; i < vis->marker_count; i++) {
        visualizer_marker_info_t* m = &vis->markers[i];
        m->has_value = 0;
        m->on_screen = 0;
        m->magnitude = 0.0f;
        m->percent_of_baseline = 0.0f;

        // Curve view plots magnitude vertically rather than a grid of cells,
        // so there is no cell for a marker to name. Every other view has one.
        if (!vis->axis_valid || vis->axis_curve_mode) {
            continue;
        }

        const int row = reference_row + m->time_offset;
        if (row < 0 || row >= rows) {
            continue;
        }
        if (m->re < 0 || m->re >= columns) {
            continue;
        }

        // Markers stay pinned to their data cell; the zoom only decides where
        // that cell lands on screen, and whether it is on screen at all.
        m->screen_x = zoom_x_to_px(vis, ((float)m->re + 0.5f) / (float)columns,
                                   plot_x, plot_w);
        m->screen_y = zoom_y_to_px(vis, ((float)row + 0.5f) / (float)rows, plot_y, plot_h);
        m->on_screen = (m->screen_x >= plot_x && m->screen_x <= plot_x + plot_w &&
                        m->screen_y >= plot_y && m->screen_y <= plot_y + plot_h) ? 1 : 0;

        float magnitude = 0.0f;
        const int found = display_cell_magnitude(vis, memory, row, m->re, &magnitude);

        if (found) {
            m->has_value = 1;
            m->magnitude = magnitude;
            m->percent_of_baseline = (baseline > 0.0f) ? (100.0f * magnitude / baseline) : 0.0f;
        }
    }
}

// Splits an absolute symbol number into the frame / subframe / slot / symbol
// position the O-RAN header gives it, for the stream's subcarrier spacing -
// the same split the vertical axis labels use, so a readout and the axis
// never disagree. Returns 0 when it cannot: spacing not known yet, or a
// negative symbol, which the ring's not-yet-filled start can produce.
static int symbol_timing(int symbol, int scs_khz, visualizer_cursor_sample_t* out) {
    const int slots_per_subframe = scs_khz / 15;
    if (slots_per_subframe <= 0 || symbol < 0) {
        return 0;
    }
    const int symbols_per_subframe = slots_per_subframe * SYMBOLS_PER_SLOT;
    const int symbols_per_frame = symbols_per_subframe * 10;
    const int in_frame = symbol % symbols_per_frame;
    out->frame = symbol / symbols_per_frame;
    out->frame_id = out->frame % 256;
    out->subframe = in_frame / symbols_per_subframe;
    out->slot = (in_frame % symbols_per_subframe) / SYMBOLS_PER_SLOT;
    out->slot_symbol = symbol % SYMBOLS_PER_SLOT;
    return 1;
}

int visualizer_sample_at_screen(const opengl_visualizer_t* vis,
                                const fh_oran_memory_t* memory,
                                float screen_x,
                                float screen_y,
                                visualizer_cursor_sample_t* out_sample) {
    if (out_sample == NULL) {
        return 0;
    }
    memset(out_sample, 0, sizeof(*out_sample));

    if (vis == NULL || !vis->axis_valid || vis->axis_curve_mode || vis->axis_rows <= 0) {
        return 0;
    }

    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 1.0f, plot_h = 1.0f;
    visualizer_get_plot_rect(vis, &plot_x, &plot_y, &plot_w, &plot_h);
    if (plot_w <= 1.0f || plot_h <= 1.0f) {
        return 0;
    }
    if (screen_x < plot_x || screen_x > plot_x + plot_w ||
        screen_y < plot_y || screen_y > plot_y + plot_h) {
        return 0;
    }

    // Invert the zoom transform: pixels -> fraction of the plot -> normalized
    // data coordinate -> grid cell.
    const float x_norm = zoom_x_origin(vis) + ((screen_x - plot_x) / plot_w) * vis->zoom_x_span;
    const float y_norm = zoom_y_origin(vis) + ((screen_y - plot_y) / plot_h) * vis->zoom_y_span;

    const int rows = vis->axis_rows;
    const int columns = vis->axis_x_count > 0 ? vis->axis_x_count : HEATMAP_TEXTURE_WIDTH;
    int re = (int)floorf(x_norm * (float)columns);
    int row = (int)floorf(y_norm * (float)rows);
    // Rounding at the far edge can land one past the last cell.
    if (re < 0) { re = 0; }
    if (re > columns - 1) { re = columns - 1; }
    if (row < 0) { row = 0; }
    if (row > rows - 1) { row = rows - 1; }

    out_sample->valid = 1;
    out_sample->re = re;
    out_sample->row = row;

    // Same reference row markers measure their offsets from, so a cursor
    // reading can be compared with marker offsets directly.
    const int reference_row = (vis->axis_trigger_row >= 0) ? vis->axis_trigger_row : (rows - 1);
    out_sample->time_offset = row - reference_row;

    if (vis->axis_y_unit == AXIS_UNIT_DOPPLER_BIN) {
        // Rows are frequency bins, not symbols. Reporting a symbol number here
        // would invent a time the row does not have.
    } else if (vis->axis_phase_mode && vis->axis_period > 0) {
        int phase = (vis->axis_row0_value + row) % vis->axis_period;
        if (phase < 0) {
            phase += vis->axis_period;
        }
        out_sample->has_phase = 1;
        out_sample->phase = phase;
    } else {
        out_sample->has_symbol = 1;
        out_sample->symbol = vis->axis_row0_value + row;
        out_sample->has_timing = symbol_timing(out_sample->symbol, memory ? memory->scs_khz : 0, out_sample);
    }

    float magnitude = 0.0f;
    if (display_cell_magnitude(vis, memory, row, re, &magnitude)) {
        const float baseline = display_baseline(vis);
        out_sample->has_value = 1;
        out_sample->magnitude = magnitude;
        out_sample->percent_of_baseline = (baseline > 0.0f) ? (100.0f * magnitude / baseline) : 0.0f;
    }

    return 1;
}

// The waterfall's state that has to advance before a frame is drawn: folding
// new symbols into the rolling mean, and the trigger -
// scanning for crossings, locking rows into a capture, and completing it once
// its post-trigger half has arrived. Runs before the texture is built, so a
// crossing detected this very frame is already reflected in what is drawn.
static void advance_waterfall_state(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!memory || !memory->symbols) {
        return;
    }

    if (vis->persist_enabled) {
        persist_fold_new_symbols(vis, memory);
        if (vis->trigger_mode != TRIGGER_OFF) {
            trigger_scan_mean_new_symbols(vis, memory);
        }
        return;
    }
    if (vis->trigger_mode == TRIGGER_OFF) {
        return;
    }

    trigger_scan_new_symbols(vis, memory);
    trigger_snapshot_capture_new_rows(vis, memory);

    // Completion check: has an active capture's post-trigger half now fully
    // arrived (i.e. is the bottom of the centered window available)?
    int trigger_half_height = vis->trigger_capture_depth / 2;
    if (vis->trigger_capture_active) {
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
}

// Update heatmap texture with IQ data from ring buffer (or, in persistence
// mode, from the rolling-mean accumulator). Only draws: the state it draws
// from is brought up to date by advance_waterfall_state.
static void render_heatmap_texture(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis || !vis->texture_data || !memory || !memory->symbols) {
        return;
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

    // Also catches coming back from the radar view, which sizes the texture to
    // its own transform width rather than the resource grid.
    if (desired_height != vis->tex_height || vis->tex_width != HEATMAP_TEXTURE_WIDTH) {
        // Reallocate CPU-side buffer and resize GPU texture
        free(vis->texture_data);
        vis->tex_height = desired_height;
        vis->tex_width = HEATMAP_TEXTURE_WIDTH;
        vis->texture_data = (uint8_t*)calloc((size_t)vis->tex_width * vis->tex_height * 4, sizeof(uint8_t));
        glBindTexture(GL_TEXTURE_2D, vis->texture_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vis->tex_width, vis->tex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }

    if (vis->texture_data == NULL) {
        return;
    }

    // Clear texture
    memset(vis->texture_data, 0, (size_t)HEATMAP_TEXTURE_WIDTH * vis->tex_height * 4);

    if (memory->symbol_highest < memory->symbol_lowest) {
        return;  // No data yet
    }

    if (vis->persist_enabled) {
        const int period = vis->persist_period;

        // Untriggered, the rows simply run phase 0 downward through the
        // period. Triggered, the same averaged rows are rotated so the phase
        // the crossing landed on sits at the middle row, under the red line -
        // the alignment is what makes the averaged picture hold still.
        axis_publish_defaults(vis);
        vis->axis_valid = 1;
        vis->axis_rows = vis->tex_height;
        vis->axis_phase_mode = 1;
        vis->axis_period = period;
        vis->axis_trigger_row = mean_triggered ? (vis->trigger_capture_depth / 2) : -1;

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
        double mean_sum = 0.0;
        long mean_count = 0;
        for (int row = 0; row < display_height; row++) {
            const float* accum_row = vis->persist_accum + (size_t)PERSIST_ROW_PHASE(row) * HEATMAP_TEXTURE_WIDTH;
            for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                float magnitude = accum_row[col];
                if (magnitude > max_magnitude) {
                    max_magnitude = magnitude;
                }
                if (magnitude >= 0.0f) {  // skip cells that never received a symbol
                    mean_sum += magnitude;
                    mean_count++;
                }
            }
        }
        vis->display_mean_magnitude = (mean_count > 0) ? (float)(mean_sum / (double)mean_count) : 0.0f;
        vis->display_mean_valid = mean_count > 0;

        // Smooth magnitude scaling so rapid amplitude shifts do not cause color flicker.
        if (!vis->magnitude_scale_initialized) {
            vis->magnitude_scale_ema = max_magnitude;
            vis->magnitude_scale_initialized = 1;
        } else {
            vis->magnitude_scale_ema = magnitude_scale_step(vis->magnitude_scale_ema, max_magnitude);
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
        int showing_triggered = (vis->trigger_mode != TRIGGER_OFF && vis->trigger_captured_symbol != TRIGGER_NO_SYMBOL);

        int symbol_count = memory->symbol_highest - memory->symbol_lowest + 1;
        int display_height = (symbol_count < vis->tex_height) ? symbol_count : vis->tex_height;

        axis_publish_defaults(vis);
        vis->axis_valid = 1;
        vis->axis_rows = vis->tex_height;
        vis->axis_trigger_row = showing_triggered ? (vis->trigger_capture_depth / 2) : -1;
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
        double mean_sum = 0.0;
        long mean_count = 0;
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
                        mean_sum += src[col];
                        mean_count++;
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
                    mean_sum += magnitude;
                    mean_count++;
                }
            }
        }
        vis->display_mean_magnitude = (mean_count > 0) ? (float)(mean_sum / (double)mean_count) : 0.0f;
        vis->display_mean_valid = mean_count > 0;

        // Smooth magnitude scaling so rapid amplitude shifts do not cause color flicker.
        if (!vis->magnitude_scale_initialized) {
            vis->magnitude_scale_ema = max_magnitude;
            vis->magnitude_scale_initialized = 1;
        } else {
            vis->magnitude_scale_ema = magnitude_scale_step(vis->magnitude_scale_ema, max_magnitude);
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
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vis->tex_width, vis->tex_height, GL_RGBA, GL_UNSIGNED_BYTE, vis->texture_data);
}

// Folds every symbol row of a published CFR frame that has not been folded
// yet, keyed by phase = symbol % period like the waterfall mean. A row is only
// tagged once it contributed at least one estimate, so a symbol whose uplink
// had not arrived when the pass ran is picked up by a later frame rather than
// lost. A symbol caught half-written - some sections of either direction still
// in flight - folds the resource elements it has; the missing ones read zero
// and are skipped like any unoccupied cell, so a partial row costs a few
// samples but never pulls the mean down.
static void radar_persist_fold_frame(opengl_visualizer_t* vis,
                                     const float* frame,
                                     int rows,
                                     int cols,
                                     const fh_radar_stats_t* stats) {
    if (vis->radar_persist_frame_seen && vis->radar_persist_last_refresh == stats->refresh_count) {
        return;  // already folded; the render loop outpaces the worker
    }

    if (!stats->window_first_valid || rows <= 0 || cols <= 0) {
        return;
    }
    if (!radar_persist_ensure(vis, cols)) {
        return;
    }
    vis->radar_persist_frame_seen = 1;
    vis->radar_persist_last_refresh = stats->refresh_count;

    const int period = vis->radar_persist_period;
    const float alpha = 1.0f / (float)vis->radar_persist_n;
    // Only the newest RADAR_PERSIST_TAG_SLOTS rows can be told apart by their
    // tags. The radar's own window limit keeps every frame inside that; this
    // just makes the assumption safe rather than silent.
    const int first_row = (rows > RADAR_PERSIST_TAG_SLOTS) ? rows - RADAR_PERSIST_TAG_SLOTS : 0;

    for (int row = first_row; row < rows; row++) {
        const int symbol = stats->window_first_symbol + row;
        int slot = symbol % RADAR_PERSIST_TAG_SLOTS;
        if (slot < 0) {
            slot += RADAR_PERSIST_TAG_SLOTS;
        }
        if (vis->radar_persist_tags[slot] == symbol) {
            continue;
        }

        int phase = symbol % period;
        if (phase < 0) {
            phase += period;
        }
        float* accum_row = vis->radar_persist_accum + (size_t)phase * (size_t)cols;
        const float* src = frame + (size_t)row * (size_t)cols;

        int folded = 0;
        for (int col = 0; col < cols; col++) {
            const float value = src[col];
            // Written so a NaN fails it too, not just zero.
            if (!(value > 0.0f)) {
                continue;
            }
            if (accum_row[col] < 0.0f) {
                accum_row[col] = value;  // first estimate ever seen at this phase
            } else {
                accum_row[col] += (value - accum_row[col]) * alpha;
            }
            folded = 1;
        }
        if (!folded) {
            continue;  // unpaired so far; a later frame may still pair it
        }

        vis->radar_persist_tags[slot] = symbol;
        if (!vis->radar_persist_has_folded) {
            vis->radar_persist_first_symbol = symbol;
            vis->radar_persist_last_symbol = symbol;
            vis->radar_persist_has_folded = 1;
        } else {
            if (symbol < vis->radar_persist_first_symbol) {
                vis->radar_persist_first_symbol = symbol;
            }
            if (symbol > vis->radar_persist_last_symbol) {
                vis->radar_persist_last_symbol = symbol;
            }
        }
    }
}

// Brings the radar rolling mean up to date with a borrowed frame; the frame
// and its stats must describe the same pass.
static void radar_persist_take_frame(opengl_visualizer_t* vis,
                                     const float* frame,
                                     int rows,
                                     int cols,
                                     const fh_radar_stats_t* stats) {
    // A different pair, a reset, or a change to what the cells measure: nothing
    // averaged so far describes the frames arriving now. Checked on every
    // frame, not only the ones that get folded, so switching output back to
    // CFR after such a change never shows the old mean for the pass it takes a
    // CFR frame to arrive.
    if (!vis->radar_persist_has_epoch || vis->radar_persist_epoch != stats->epoch) {
        radar_persist_clear_values(vis);
        vis->radar_persist_epoch = stats->epoch;
        vis->radar_persist_has_epoch = 1;
    }

    // Folding goes by the output the frame was computed for, drawing by the one
    // selected now. Right after switching output the two differ for a pass,
    // and a frame of another output must never be folded into the CFR mean.
    if (vis->radar_persist_enabled && stats->output == RADAR_OUT_CFR) {
        radar_persist_fold_frame(vis, frame, rows, cols, stats);
    }
}

// Build the texture from the radar core's most recent finished frame. Unlike
// the waterfall this reads no symbol ring at all: the radar worker has already
// reduced two rings to one magnitude matrix, and the render thread only borrows
// it for as long as the copy takes. With the rolling mean on, the frame is
// folded into the accumulator and the accumulator is drawn instead.
//
// Returns 1 when a frame was drawn, 0 when the radar has nothing yet - the
// caller distinguishes "no data" from "no radar" for the on-screen notice.
static int update_radar_texture(opengl_visualizer_t* vis) {
    axis_publish_defaults(vis);
    vis->axis_valid = 0;
    vis->axis_radar_mode = 1;

    if (vis->radar == NULL) {
        return 0;
    }

    // The three outputs do not share a grid, so the axes are described from the
    // selected one rather than inherited from the waterfall.
    fh_radar_config_t rcfg;
    fh_radar_get_config(vis->radar, &rcfg);

    const int scs_khz = (vis->last_seen_memory != NULL) ? vis->last_seen_memory->scs_khz : 0;
    const double subcarrier_hz = (double)scs_khz * 1000.0;

    switch (rcfg.output) {
    case RADAR_OUT_CFR:
        // Still the resource-element grid, just holding a channel estimate
        // instead of a received magnitude.
        vis->axis_value_label = "|H|";
        break;

    case RADAR_OUT_RANGE_TIME:
        vis->axis_x_unit = AXIS_UNIT_DELAY_TAP;
        vis->axis_x_count = rcfg.ifft_size;
        vis->axis_value_label = "|CIR|";
        if (subcarrier_hz > 0.0) {
            vis->axis_x_unit_scale =
                (float)(299792458.0 / (2.0 * (double)rcfg.ifft_size * subcarrier_hz));
        }
        break;

    case RADAR_OUT_RANGE_DOPPLER:
        vis->axis_x_unit = AXIS_UNIT_DELAY_TAP;
        vis->axis_x_count = rcfg.cir_depth_taps;
        vis->axis_value_label = "|RD|";
        if (subcarrier_hz > 0.0) {
            vis->axis_x_unit_scale =
                (float)(299792458.0 / (2.0 * (double)rcfg.ifft_size * subcarrier_hz));
        }
        // A whole processing interval collapses into one matrix: the vertical
        // axis is a fixed frequency scale, replaced outright each refresh
        // rather than scrolling with arriving symbols.
        vis->axis_y_unit = AXIS_UNIT_DOPPLER_BIN;
        vis->axis_y_scrolling = 0;
        if (scs_khz > 0 && rcfg.time_symbols > 0) {
            // A slot is 14 symbols and 1 ms / (scs / 15 kHz); one Doppler bin
            // is the reciprocal of the whole interval's duration.
            const double symbol_s = 0.001 / ((double)scs_khz / 15.0) / 14.0;
            vis->axis_y_unit_scale = (float)(1.0 / ((double)rcfg.time_symbols * symbol_s));
        }
        break;
    }

    int rows = 0;
    int cols = 0;
    const float* result = fh_radar_acquire_result(vis->radar, &rows, &cols);
    if (result == NULL) {
        return 0;
    }
    if (rows <= 0 || cols <= 0) {
        fh_radar_release_result(vis->radar);
        return 0;
    }

    // Read while the frame is still borrowed: the worker cannot publish in the
    // meantime, so these describe exactly the frame in hand.
    fh_radar_stats_t stats;
    fh_radar_get_stats(vis->radar, &stats);

    radar_persist_take_frame(vis, result, rows, cols, &stats);
    const int show_mean = vis->radar_persist_enabled && rcfg.output == RADAR_OUT_CFR;

    // What goes on screen: the live frame, or the accumulator. The accumulator
    // is the visualizer's own memory, so the frame can go back to the worker
    // straight away instead of being held through the copy below.
    const float* source = result;
    int source_rows = rows;
    int source_cols = cols;
    if (show_mean) {
        fh_radar_release_result(vis->radar);
        if (vis->radar_persist_accum == NULL || vis->radar_persist_cols <= 0) {
            return 0;
        }
        source = vis->radar_persist_accum;
        source_rows = vis->radar_persist_period;
        source_cols = vis->radar_persist_cols;
    }

    // The cell counts come from the published frame, never from the config the
    // axes were described with. The two can briefly disagree - the worker may
    // still be finishing a pass under the previous settings - and letting the
    // axis claim a size the pixels do not have is how a plot starts lying.
    int desired_width = source_cols;
    int desired_height = source_rows;
    const int cap = vis->tex_max_height > 0 ? vis->tex_max_height : 4096;
    if (desired_height > cap) {
        desired_height = cap;
    }
    if (desired_width > cap) {
        desired_width = cap;
    }

    if (desired_height != vis->tex_height || desired_width != vis->tex_width) {
        free(vis->texture_data);
        vis->tex_height = desired_height;
        vis->tex_width = desired_width;
        vis->texture_data = (uint8_t*)calloc((size_t)vis->tex_width * vis->tex_height * 4, sizeof(uint8_t));
        glBindTexture(GL_TEXTURE_2D, vis->texture_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vis->tex_width, vis->tex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }

    if (vis->texture_data == NULL) {
        if (!show_mean) {
            fh_radar_release_result(vis->radar);
        }
        return 0;
    }

    // Same adaptive normalisation as the waterfall. A channel estimate sits
    // near 1.0 when everything is right, but it is the numeric mean in the
    // settings window that says so precisely - the colours only need to show
    // structure, and auto-scaling keeps them legible when it is wrong.
    float max_magnitude = 0.0f;
    double mean_sum = 0.0;
    uint64_t mean_count = 0;
    for (int row = 0; row < vis->tex_height; row++) {
        const float* src = source + (size_t)row * source_cols;
        for (int col = 0; col < vis->tex_width; col++) {
            const float v = src[col];
            if (v > max_magnitude) {
                max_magnitude = v;
            }
            if (v > 0.0f) {
                mean_sum += (double)v;
                mean_count++;
            }
        }
    }

    // Markers and the cursor report a percentage against this, the same way
    // they do over the waterfall.
    vis->display_mean_magnitude = (mean_count > 0) ? (float)(mean_sum / (double)mean_count) : 0.0f;
    vis->display_mean_valid = mean_count > 0;

    if (!vis->magnitude_scale_initialized) {
        vis->magnitude_scale_ema = max_magnitude;
        vis->magnitude_scale_initialized = 1;
    } else {
        vis->magnitude_scale_ema = magnitude_scale_step(vis->magnitude_scale_ema, max_magnitude);
    }
    const float render_scale = (vis->magnitude_scale_ema > 1e-6f) ? vis->magnitude_scale_ema : 1e-6f;

    // Live, row 0 is the oldest symbol in the window, which is the waterfall's
    // top-to-bottom time order, so rows map straight across. The mean runs
    // phase 0 downward through the period, like the untriggered waterfall mean.
    for (int row = 0; row < vis->tex_height; row++) {
        const float* src = source + (size_t)row * source_cols;
        for (int col = 0; col < vis->tex_width; col++) {
            // Only the accumulator holds negatives: cells no estimate ever
            // reached. Black, as in the waterfall mean, so they cannot be
            // mistaken for a measured |H| near zero at the bottom of the scale.
            uint8_t r = 0, g = 0, b = 0;
            if (src[col] >= 0.0f) {
                magnitude_to_rgb(src[col], render_scale, &r, &g, &b);
            }

            const size_t pixel_idx = ((size_t)row * vis->tex_width + col) * 4;
            vis->texture_data[pixel_idx + 0] = r;
            vis->texture_data[pixel_idx + 1] = g;
            vis->texture_data[pixel_idx + 2] = b;
            vis->texture_data[pixel_idx + 3] = 255;
        }
    }

    if (!show_mean) {
        fh_radar_release_result(vis->radar);
    }

    vis->axis_valid = 1;
    vis->axis_rows = vis->tex_height;
    vis->axis_x_count = vis->tex_width;
    if (show_mean) {
        // Rows are phases, so ticks, markers and the cursor readout all switch
        // to the offset-in-period reading the waterfall mean uses.
        vis->axis_phase_mode = 1;
        vis->axis_period = vis->radar_persist_period;
        vis->axis_row0_value = 0;
    } else {
        vis->axis_row0_value = stats.window_first_valid ? stats.window_first_symbol : 0;
    }

    glBindTexture(GL_TEXTURE_2D, vis->texture_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vis->tex_width, vis->tex_height, GL_RGBA, GL_UNSIGNED_BYTE, vis->texture_data);
    return 1;
}

// Draw the current texture across the plot rect. Shared by the waterfall and
// the radar; the two differ only in where the texture came from and whether
// the zoom window narrows the sampled sub-rectangle.
static void draw_plot_texture_quad(opengl_visualizer_t* vis, int apply_zoom) {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, vis->texture_id);

    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 1.0f, plot_h = 1.0f;
    visualizer_get_plot_rect(vis, &plot_x, &plot_y, &plot_w, &plot_h);

    const float s0 = apply_zoom ? zoom_x_origin(vis) : 0.0f;
    const float s1 = apply_zoom ? (s0 + vis->zoom_x_span) : 1.0f;
    const float t0 = apply_zoom ? zoom_y_origin(vis) : 0.0f;
    const float t1 = apply_zoom ? (t0 + vis->zoom_y_span) : 1.0f;

    const int texels_x = vis->tex_width > 0 ? vis->tex_width : 1;
    const float px_per_texel_x = plot_w / ((s1 - s0) * (float)texels_x);
    const int rows = vis->tex_height > 0 ? vis->tex_height : 1;
    const float px_per_texel_y = plot_h / ((t1 - t0) * (float)rows);
    const int want_nearest = (px_per_texel_x >= ZOOM_NEAREST_FILTER_PX ||
                              px_per_texel_y >= ZOOM_NEAREST_FILTER_PX);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, want_nearest ? GL_NEAREST : GL_LINEAR);

    glBegin(GL_QUADS);
    glTexCoord2f(s0, t0); glVertex2f(plot_x, plot_y);
    glTexCoord2f(s1, t0); glVertex2f(plot_x + plot_w, plot_y);
    glTexCoord2f(s1, t1); glVertex2f(plot_x + plot_w, plot_y + plot_h);
    glTexCoord2f(s0, t1); glVertex2f(plot_x, plot_y + plot_h);
    glEnd();

    glDisable(GL_TEXTURE_2D);
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
    // Reset on any change of stream, including the first one seen. A frame
    // where no stream is selected must not swallow the reset, or trigger
    // state from the previous stream survives into the next one.
    if (memory != vis->last_seen_memory) {
        if (memory != NULL) {
            if (vis->persist_accum) {
                persist_clear_values(vis);
            }
            if (vis->curve_mean_accum) {
                curve_mean_clear_values(vis);
            }
            trigger_reset_state(vis);
            // Drop the colour normalisation too. It only ever eases downward
            // (slow fall, fast rise) to keep the picture stable, so carrying a
            // previous stream's level over - including the demo playing behind
            // the startup screen - leaves the view crawling toward the right
            // scale for a second or more. Clearing it makes the first frame of
            // a new stream set the scale outright.
            vis->magnitude_scale_initialized = 0;
            vis->curve_magnitude_scale_initialized = 0;
        }
        vis->last_seen_memory = memory;
    }

    glClear(GL_COLOR_BUFFER_BIT);

    // No default case: -Wswitch then points at every dispatch site whenever a
    // view mode is added, which is how the radar mode was found to need one.
    switch (vis->view_mode) {
    case VIEW_MODE_CURVE:
        curve_advance(vis, memory);
        curve_build_frame(vis, memory);
        curve_draw_frame(vis);
        // Clears any marker readouts left over from the waterfall, which has
        // no counterpart here.
        markers_evaluate(vis, memory);
        break;

    case VIEW_MODE_RADAR:
        // The grid is published before the zoom is re-clamped, because the
        // vertical minimum is a row count and the radar's row count changes
        // with the processing window.
        update_radar_texture(vis);
        zoom_normalize(vis);
        markers_evaluate(vis, memory);
        draw_plot_texture_quad(vis, 1);
        break;

    case VIEW_MODE_WATERFALL:
    case VIEW_MODE_COUNT: {
        advance_waterfall_state(vis, memory);
        render_heatmap_texture(vis, memory);

        // Re-clamp now that the row count for this frame is settled. The
        // vertical minimum is a row count, so a window that was legal at one
        // extent can be too narrow after Memory size, the averaging period or a
        // trigger capture changes it - and nothing else would notice.
        zoom_normalize(vis);
        markers_evaluate(vis, memory);

        // Zooming the waterfall is exactly a narrowing of the texture
        // coordinates: the same quad fills the same plot rect, sampling a
        // sub-rectangle of the same texture. Nothing upstream of the texture
        // upload is involved, which is why zoom cannot perturb the pipeline.
        draw_plot_texture_quad(vis, 1);

        // Trigger line: marks the row carrying the captured symbol once a
        // capture has fired. Drawn in screen space (like the curve view's
        // lines) so it stays crisp regardless of the texture's row height.
        // Goes by the row published with the frame on screen.
        if (vis->axis_trigger_row >= 0) {
            float plot_x = 0.0f, plot_y = 0.0f, plot_w = 1.0f, plot_h = 1.0f;
            visualizer_get_plot_rect(vis, &plot_x, &plot_y, &plot_w, &plot_h);
            const int rows = vis->tex_height > 0 ? vis->tex_height : 1;
            const int centre_row = vis->axis_trigger_row;
            const float row_norm = ((float)centre_row + 0.5f) / (float)rows;
            const float line_y = zoom_y_to_px(vis, row_norm, plot_y, plot_h);
            // Zooming can put the trigger row outside the window; drawing it
            // anyway would pin a red line to the plot edge and misreport where
            // the trigger sits.
            if (line_y >= plot_y && line_y <= plot_y + plot_h) {
                glColor4f(1.0f, 0.0f, 0.0f, 1.0f);
                glLineWidth(1.5f);
                glBegin(GL_LINES);
                glVertex2f(plot_x, line_y);
                glVertex2f(plot_x + plot_w, line_y);
                glEnd();
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            }
        }
        break;
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
    free(vis->radar_persist_accum);
    free(vis->radar_persist_tags);
    free(vis->curve_frame_traces);
    free(vis->curve_frame_mean);
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

