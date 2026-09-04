#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declare the memory structure from core
typedef struct fh_oran_memory fh_oran_memory_t;

// Opaque visualizer handle
typedef struct opengl_visualizer opengl_visualizer_t;

// Create and initialize an OpenGL visualizer window
// texture_height controls how many symbol rows are rendered in the waterfall
// Returns NULL on failure (e.g., GLFW/OpenGL init failed)
opengl_visualizer_t* visualizer_create(int width, int height, int texture_height);

// Main render loop—call this in a dedicated thread
// Runs until window closure or error
// memory: pointer to ring buffer to visualize
void visualizer_run(opengl_visualizer_t* vis, const fh_oran_memory_t* memory);

// Non-blocking poll for window events and render one frame
// Returns 0 on success, negative on window close or error
int visualizer_update(opengl_visualizer_t* vis, const fh_oran_memory_t* memory);

// Present the current backbuffer to the screen after overlay drawing is finished
void visualizer_present(opengl_visualizer_t* vis);

// Check if the window is still open
int visualizer_is_running(const opengl_visualizer_t* vis);

// Destroy and free the visualizer
void visualizer_destroy(opengl_visualizer_t* vis);

// --- Rolling-mean persistence mode ---
// Instead of showing raw per-frame magnitude, accumulate an exponential
// moving average of magnitude per (phase-in-period, IQ sample) cell, where
// phase = absolute_symbol_number % persistence_period. This reveals
// periodic structure instead of scrolling past it. Independent of the
// memory ring's own capacity/overwrite behavior.

void visualizer_set_persistence_enabled(opengl_visualizer_t* vis, int enabled);
int visualizer_get_persistence_enabled(const opengl_visualizer_t* vis);

// Sets the averaging period in symbols and reallocates/resets the accumulator.
void visualizer_set_persistence_period(opengl_visualizer_t* vis, int period_symbols);
int visualizer_get_persistence_period(const opengl_visualizer_t* vis);

// Sets the EMA smoothing factor "n" (approximate rolling-mean window in periods).
void visualizer_set_persistence_n(opengl_visualizer_t* vis, int n);
int visualizer_get_persistence_n(const opengl_visualizer_t* vis);

// Clears all accumulated averages without changing settings.
void visualizer_reset_persistence(opengl_visualizer_t* vis);

// --- View mode ---
typedef enum visualizer_view_mode {
    VIEW_MODE_WATERFALL = 0,
    VIEW_MODE_CURVE = 1
} visualizer_view_mode_t;

void visualizer_set_view_mode(opengl_visualizer_t* vis, visualizer_view_mode_t mode);
visualizer_view_mode_t visualizer_get_view_mode(const opengl_visualizer_t* vis);

// --- Curve view: single-symbol magnitude-vs-resource-element plot ---
// Instead of the waterfall's scrolling time axis, Curve mode plots one
// symbol's magnitude across all resource elements. Two independent,
// combinable features:

// Trailing traces: draw the last N symbols (read live from the same memory
// ring, no extra storage), older ones fading out, instead of just the newest.
void visualizer_set_curve_trace_enabled(opengl_visualizer_t* vis, int enabled);
int visualizer_get_curve_trace_enabled(const opengl_visualizer_t* vis);

// Clamped to [1, 256]; default 8.
void visualizer_set_curve_trace_count(opengl_visualizer_t* vis, int count);
int visualizer_get_curve_trace_count(const opengl_visualizer_t* vis);

// Rolling-mean curve: same EMA/phase concept as waterfall persistence, but
// with its own independent accumulator/settings (separate window/settings).
typedef enum visualizer_curve_mean_mode {
    CURVE_MEAN_MODE_PHASE_LOCKED = 0,     // user-set period; phase = symbol % period
    CURVE_MEAN_MODE_RUNNING_AVERAGE = 1   // period forced to 1; every symbol is phase 0
} visualizer_curve_mean_mode_t;

void visualizer_set_curve_mean_enabled(opengl_visualizer_t* vis, int enabled);
int visualizer_get_curve_mean_enabled(const opengl_visualizer_t* vis);

void visualizer_set_curve_mean_mode(opengl_visualizer_t* vis, visualizer_curve_mean_mode_t mode);
visualizer_curve_mean_mode_t visualizer_get_curve_mean_mode(const opengl_visualizer_t* vis);

// Only meaningful in CURVE_MEAN_MODE_PHASE_LOCKED; reallocates/resets the accumulator.
void visualizer_set_curve_mean_period(opengl_visualizer_t* vis, int period_symbols);
int visualizer_get_curve_mean_period(const opengl_visualizer_t* vis);

void visualizer_set_curve_mean_n(opengl_visualizer_t* vis, int n);
int visualizer_get_curve_mean_n(const opengl_visualizer_t* vis);

// Clears the accumulated mean curve without changing settings.
void visualizer_reset_curve_mean(opengl_visualizer_t* vis);

// --- Waterfall trigger ---
// Oscilloscope-style triggering for the waterfall's raw (non-persistence)
// view: when a new symbol's peak magnitude rises above the trigger level,
// the display stops continuously scrolling and instead centers that symbol
// at the vertical middle, with older context above and newer context
// filling in below as it arrives, marked by a horizontal red line. Only
// takes effect while Persistence is off (a period-averaged display has no
// coherent chronological "center" to trigger on).
typedef enum visualizer_trigger_mode {
    TRIGGER_OFF = 0,
    TRIGGER_REPEAT = 1,  // re-arms automatically once each capture completes
    TRIGGER_SINGLE = 2   // captures once, then holds until visualizer_trigger_rearm()
} visualizer_trigger_mode_t;

// Resets all trigger state on any mode change, so switching modes always
// starts fresh rather than reusing a stale captured symbol.
void visualizer_set_trigger_mode(opengl_visualizer_t* vis, visualizer_trigger_mode_t mode);
visualizer_trigger_mode_t visualizer_get_trigger_mode(const opengl_visualizer_t* vis);

// Trigger level as a percentage of a slow-moving baseline. Both sides of
// the comparison use the same statistic - each symbol's peak magnitude
// (within the trigger frequency range) against an EMA of that same peak
// over recent symbols carrying data. Comparing like with like is what makes
// the percentage mean something concrete:
//
//   ~100%  = a symbol as strong as the recent typical peak
//   < 100% = fires when the signal returns after a gap or quiet period,
//            which is what lines a rising edge up on the start of a TDD
//            burst (gaps count as zero)
//   > 100% = fires only on symbols notably stronger than typical
//   far above the strongest expected symbol = effectively never fires
//
// The baseline is deliberately NOT the waterfall's display auto-scale: that
// one re-normalizes to the current peak every frame, so its ceiling is
// always ~100% of itself and a threshold against it would always be crossed.
void visualizer_set_trigger_level_pct(opengl_visualizer_t* vis, float level_pct);
float visualizer_get_trigger_level_pct(const opengl_visualizer_t* vis);

// Live measured values behind the last evaluated symbol, for UI diagnostics
// and for picking a level that actually corresponds to the signal. Any
// out-pointer may be NULL.
void visualizer_get_trigger_levels(const opengl_visualizer_t* vis,
                                   float* out_last_peak,
                                   float* out_baseline,
                                   float* out_fire_threshold);

// Restricts trigger crossing detection (and the baseline it's measured
// against) to a sub-range of resource elements, [start, end), instead of the
// full 0..3275 bandwidth - lets triggering focus on a specific frequency
// slice. Does not affect what's displayed once triggered, only what's
// searched to decide when to fire. Clamped/ordered internally.
void visualizer_set_trigger_freq_range(opengl_visualizer_t* vis, int re_start, int re_end);
int visualizer_get_trigger_freq_start(const opengl_visualizer_t* vis);
int visualizer_get_trigger_freq_end(const opengl_visualizer_t* vis);

// Rising: fires when peak magnitude crosses from below to at/above the
// threshold. Falling: fires on the opposite crossing (above to below).
// Either way, once fired it won't fire again until the signal crosses back
// the other direction first - so a sustained high (rising mode) or
// sustained low (falling mode) does not repeatedly retrigger.
typedef enum visualizer_trigger_edge {
    TRIGGER_EDGE_RISING = 0,
    TRIGGER_EDGE_FALLING = 1
} visualizer_trigger_edge_t;

void visualizer_set_trigger_edge(opengl_visualizer_t* vis, visualizer_trigger_edge_t edge);
visualizer_trigger_edge_t visualizer_get_trigger_edge(const opengl_visualizer_t* vis);

// Optional hysteresis: when enabled, "the signal crossed back the other
// direction" (which re-arms detection for the next fire) is measured
// against this separate, lower-margin reset level instead of the trigger
// level itself. Without this, a signal that briefly dips/spikes right at
// the trigger level (noise, fading) can look like a fresh crossing and fire
// again almost immediately. With it, the signal has to clear a real gap
// past the trigger level before it counts as reset. For rising edge, keep
// this below the trigger level (must drop further to reset); for falling
// edge, keep it above (must rise further). Disabled by default - falls back
// to using the trigger level for both.
void visualizer_set_trigger_hysteresis_enabled(opengl_visualizer_t* vis, int enabled);
int visualizer_get_trigger_hysteresis_enabled(const opengl_visualizer_t* vis);

void visualizer_set_trigger_reset_level_pct(opengl_visualizer_t* vis, float level_pct);
float visualizer_get_trigger_reset_level_pct(const opengl_visualizer_t* vis);

// How many symbol rows a triggered capture uses (half before the trigger,
// half after) - independent of the live view's own display height, which is
// tied to Memory size. This independence matters: waiting for the
// post-trigger half to arrive requires the ring to slide forward by that
// same amount, which would evict the pre-trigger half by the time it's done
// if the capture window were anywhere near the ring's total capacity. Keep
// this comfortably smaller than Memory size (the default is deliberately
// small for that reason).
void visualizer_set_trigger_capture_depth(opengl_visualizer_t* vis, int depth_symbols);
int visualizer_get_trigger_capture_depth(const opengl_visualizer_t* vis);

// Manually re-arms a Single-mode capture (no-op in Off/Repeat).
void visualizer_trigger_rearm(opengl_visualizer_t* vis);

// Current trigger state, for UI diagnostics - tells you at a glance whether
// the trigger is actively searching or waiting on something.
typedef enum visualizer_trigger_status {
    TRIGGER_STATUS_OFF = 0,
    TRIGGER_STATUS_ARMED,      // searching: a qualifying crossing will fire
    TRIGGER_STATUS_WAIT_RESET, // signal hasn't crossed back past the reset level yet
    TRIGGER_STATUS_CAPTURING,  // fired; still collecting the post-trigger half
    TRIGGER_STATUS_HOLDING,    // capture complete and held (Single: needs Re-arm)
    TRIGGER_STATUS_WARMING_UP  // rolling mean is on but hasn't accumulated n periods yet
} visualizer_trigger_status_t;

visualizer_trigger_status_t visualizer_get_trigger_status(const opengl_visualizer_t* vis);

// With the rolling mean enabled the trigger judges the averaged rows rather
// than individual symbols, and holds off until the mean has accumulated the
// configured number of periods - averaging first keeps it steady when
// symbols are noisy or packets are being dropped. Reports that progress;
// either out-pointer may be NULL. Returns 0 when the rolling mean is off,
// in which case there is no warm-up and the values are meaningless.
int visualizer_get_trigger_mean_progress(const opengl_visualizer_t* vis,
                                         int* out_periods_elapsed,
                                         int* out_periods_required);

// Total captures since the trigger was last reset; handy for confirming the
// trigger is firing at all when the display looks static.
unsigned long visualizer_get_trigger_fire_count(const opengl_visualizer_t* vis);

#ifdef __cplusplus
}
#endif
