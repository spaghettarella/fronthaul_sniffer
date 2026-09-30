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
// VIEW_MODE_COUNT sizes the per-mode state below; keep it last so adding a
// mode grows those arrays instead of silently sharing another mode's slot.
typedef enum visualizer_view_mode {
    VIEW_MODE_WATERFALL = 0,
    VIEW_MODE_CURVE = 1,
    VIEW_MODE_RADAR = 2,
    VIEW_MODE_COUNT = 3
} visualizer_view_mode_t;

void visualizer_set_view_mode(opengl_visualizer_t* vis, visualizer_view_mode_t mode);
visualizer_view_mode_t visualizer_get_view_mode(const opengl_visualizer_t* vis);

// --- Radar view ---
// The radar's magnitude matrix is computed by the radar core on its own
// thread (see core/c_radar.h); the visualizer only draws whatever frame is
// current. Handing over the handle is what switches the radar view from its
// placeholder to live data.
struct fh_radar;
void visualizer_set_radar(opengl_visualizer_t* vis, struct fh_radar* radar);

// --- Radar rolling mean ---
// The waterfall's phase-locked EMA applied to the radar's CFR magnitude: each
// paired symbol's |H| row is folded once into the cell for
// (symbol % period, RE), and the view shows the accumulator instead of the
// live window. Its own accumulator and settings, since |H| and a received
// magnitude are different quantities and must never share an average.
//
// Only cells that carry a channel estimate are folded - occupied in both
// directions. An unoccupied resource element has no |H| to average, and
// folding it as zero would make the mean follow the scheduler's allocation
// rather than the channel. Cells that never received an estimate stay black.
//
// CFR output only for now; the other outputs keep drawing their live frame.
// The accumulator starts over whenever the radar's frames stop being
// comparable (see fh_radar_stats::epoch): a new DL/UL pair, a radar reset, or
// a change of frequency range, lag or division.
void visualizer_set_radar_persistence_enabled(opengl_visualizer_t* vis, int enabled);
int visualizer_get_radar_persistence_enabled(const opengl_visualizer_t* vis);

// Averaging period in symbols; reallocates and clears the accumulator.
void visualizer_set_radar_persistence_period(opengl_visualizer_t* vis, int period_symbols);
int visualizer_get_radar_persistence_period(const opengl_visualizer_t* vis);

// EMA smoothing factor "n" (approximate rolling-mean window in periods).
void visualizer_set_radar_persistence_n(opengl_visualizer_t* vis, int n);
int visualizer_get_radar_persistence_n(const opengl_visualizer_t* vis);

// Clears the accumulated average without changing settings.
void visualizer_reset_radar_persistence(opengl_visualizer_t* vis);

// Whole periods folded since the accumulator was last cleared, so the UI can
// show whether the average has settled. 0 while nothing has been folded.
int visualizer_get_radar_persistence_periods(const opengl_visualizer_t* vis);

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

// --- Axes ---
// The heatmap is drawn into a plot rect inset from the window edges, leaving
// margins for axis ticks and labels so nothing is ever drawn over the data.
// The visualizer owns the mapping from data to pixels (which differs between
// the live, triggered and rolling-mean views), so it decides which ticks
// exist; the caller only draws them. Positions are in framebuffer pixels -
// use visualizer_get_framebuffer_size() to render in the same space.
void visualizer_get_plot_rect(const opengl_visualizer_t* vis,
                              float* out_x, float* out_y, float* out_w, float* out_h);

void visualizer_get_framebuffer_size(const opengl_visualizer_t* vis, int* out_width, int* out_height);

// Turns the axis margins off so the view fills the whole window edge to
// edge. Intended for backdrops that carry no axes of their own, such as the
// demo capture playing behind the startup screen, where the reserved margins
// would otherwise just show as black bars. Enabled by default.
void visualizer_set_axes_enabled(opengl_visualizer_t* vis, int enabled);
int visualizer_get_axes_enabled(const opengl_visualizer_t* vis);

#define VISUALIZER_MAX_AXIS_TICKS 256

// level orders ticks by importance, so a caller can scale tick length with
// it. Vertical: 0=symbol, 1=slot, 2=subframe, 3=frame (or, in rolling-mean
// and curve views, the equivalent relative divisions). Horizontal:
// 0=resource element, 1=PRB.
typedef struct visualizer_axis_tick {
    float position_px;
    int level;
    int has_label;
    int is_anchor;    // the always-labelled newest-symbol readout, highlighted by the caller
    // Label lines, stacked top to bottom; later ones are "" when unused. The
    // horizontal axis uses line 2 for the absolute RE index. The vertical
    // axis splits the frame number from its wrapped id, and the anchor adds
    // the subframe/slot/symbol detail on line 3.
    char label[48];
    char label2[48];
    char label3[48];
} visualizer_axis_tick_t;

// Both return the number of ticks written. Tick density adapts to how much
// is in view, so levels that would be too crowded to read are dropped.
// The vertical axis needs the displayed stream to resolve symbol numbering
// and subcarrier spacing; pass the same memory being rendered.
int visualizer_get_y_ticks(const opengl_visualizer_t* vis,
                           const fh_oran_memory_t* memory,
                           visualizer_axis_tick_t* out_ticks,
                           int max_ticks);

int visualizer_get_x_ticks(const opengl_visualizer_t* vis,
                           visualizer_axis_tick_t* out_ticks,
                           int max_ticks);

// Axis titles for the current view, e.g. "Symbol" / "Slot offset" vertically
// and "PRB" / "RE" horizontally. Either out-pointer may be NULL.
void visualizer_get_axis_titles(const opengl_visualizer_t* vis,
                                const char** out_y_title,
                                const char** out_x_title);

// --- Grid description ---
// What the two axes currently measure. The waterfall and curve views always
// plot resource elements horizontally, but the radar's axes change with its
// selected output - delay taps horizontally, Doppler bins vertically - so
// anything that labels an axis, converts a cell index to a physical quantity,
// or sizes the zoom window has to ask rather than assume.
typedef enum visualizer_axis_unit {
    AXIS_UNIT_RE = 0,          // resource element index (horizontal)
    AXIS_UNIT_DELAY_TAP = 1,   // channel-impulse-response tap (horizontal)
    AXIS_UNIT_SYMBOL = 2,      // OFDM symbol, i.e. time (vertical)
    AXIS_UNIT_DOPPLER_BIN = 3, // Doppler frequency bin (vertical)
    AXIS_UNIT_MAGNITUDE = 4    // normalised magnitude (vertical, curve view)
} visualizer_axis_unit_t;

typedef struct visualizer_grid_info {
    int x_count;                  // cells across; 0 when nothing is drawn
    int y_count;                  // cells down
    visualizer_axis_unit_t x_unit;
    visualizer_axis_unit_t y_unit;
    const char* x_title;
    const char* y_title;
    const char* value_label;      // what one cell holds, e.g. "magnitude", "|H|"
    // Physical size of one cell, 0 when unknown or not meaningful: metres per
    // delay tap horizontally, hertz per bin vertically. Lets callers show a
    // real quantity next to the raw index.
    float x_unit_scale;
    float y_unit_scale;
    // Whether the vertical axis scrolls with newly arriving symbols. False for
    // Range-Doppler, where the whole matrix is replaced per processing
    // interval and the axis is a fixed frequency scale.
    int y_is_scrolling;
} visualizer_grid_info_t;

void visualizer_get_grid_info(const opengl_visualizer_t* vis, visualizer_grid_info_t* out);

// --- Zoom ---
// A view window over the data, expressed in normalized [0,1] coordinates per
// axis, where 0,0 is the top-left of the fully zoomed-out view. Horizontally
// the axis spans the resource-element grid; vertically it spans whatever the
// view currently plots - symbol rows in the waterfall, magnitude in curve
// view. Because the window is normalized rather than absolute, it survives
// the vertical extent changing underneath it (Memory size, averaging period
// and trigger capture depth all resize the row count).
//
// This is a rendering transform and nothing more: no capture, decode, memory
// ring, rolling-mean or trigger parameter is affected, so zooming never
// disturbs what is being measured or averaged. Axis ticks re-derive their
// density from the zoomed window, so zooming in exposes finer divisions.
#define VISUALIZER_RE_COUNT 3276

// span is the fraction of the axis in view: 1.0 is everything, 0.25 is 4x
// zoom. Both are clamped so the window stays inside the data and no narrower
// than an internal minimum; read them back to see what was applied.
void visualizer_set_zoom(opengl_visualizer_t* vis,
                         float x_center, float x_span,
                         float y_center, float y_span);
void visualizer_get_zoom(const opengl_visualizer_t* vis,
                         float* out_x_center, float* out_x_span,
                         float* out_y_center, float* out_y_span);

// Back to the full view on both axes.
void visualizer_reset_zoom(opengl_visualizer_t* vis);

// Non-zero when either axis is showing less than the whole extent.
int visualizer_zoom_is_active(const opengl_visualizer_t* vis);

// Scales the zoom on each axis by the given factor (>1 zooms in, 1 leaves the
// axis alone) about a focus point given as a fraction of the plot rect. The
// data under the focus point stays put, which is what makes scroll-wheel zoom
// track the cursor instead of drifting away from it.
void visualizer_zoom_by(opengl_visualizer_t* vis,
                        float x_factor, float y_factor,
                        float focus_x, float focus_y);

// Moves the window by a fraction of its own current span, so a keypress pans
// by the same visual distance no matter how deep the zoom is.
void visualizer_pan_by(opengl_visualizer_t* vis, float x_fraction, float y_fraction);

// Vertical extent of the fully zoomed-out waterfall, in texture rows - the
// denominator the normalized y window is a fraction of. Follows whichever of
// Memory size, averaging period or capture depth is driving the view, so the
// zoom UI can show and set the window in rows. Returns 0 before the first
// frame has been rendered.
int visualizer_get_zoom_total_rows(const opengl_visualizer_t* vis);

// --- Markers ---
// Each marker pins a point in the view by frequency (resource element) and
// time (an offset in symbols from a reference row). The reference is the
// trigger point when a capture is showing, otherwise the newest symbol -
// visualizer_get_marker_reference_label() names whichever is in force. A
// positive offset moves toward newer data, i.e. downward on screen.
//
// Values are re-sampled every frame from whatever the view is currently
// showing, so a marker reads the rolling mean in persistence mode and the
// held snapshot in a triggered one, without the caller needing to know.
#define VISUALIZER_MAX_MARKERS 12

typedef struct visualizer_marker_info {
    int re;                     // 0..3275
    int time_offset;            // symbols from the reference row
    int has_value;              // 0 when the marker falls outside the data
    float magnitude;            // absolute IQ magnitude at the marker
    float percent_of_baseline;  // relative to the mean magnitude on screen
    int on_screen;              // whether screen_x/screen_y are usable
    float screen_x;
    float screen_y;
    unsigned char color[3];
} visualizer_marker_info_t;

// Returns the new marker's index, or -1 when the list is full.
int visualizer_add_marker(opengl_visualizer_t* vis);
void visualizer_remove_marker(opengl_visualizer_t* vis, int index);
int visualizer_get_marker_count(const opengl_visualizer_t* vis);

// Returns 1 and fills out_info on success, 0 for an invalid index.
int visualizer_get_marker(const opengl_visualizer_t* vis, int index, visualizer_marker_info_t* out_info);
void visualizer_set_marker(opengl_visualizer_t* vis, int index, int re, int time_offset);

// Names what marker offsets are measured from, for labelling the UI.
const char* visualizer_get_marker_reference_label(const opengl_visualizer_t* vis);

// --- Value at cursor ---
// Reads out the cell under an arbitrary screen position, so hovering the plot
// can report a value without placing a marker. Resolves through the same path
// markers use, so the two always agree about what a cell holds, and respects
// the zoom: the position is inverted through the current view window.
//
// Like markers, this describes the waterfall grid only. In curve view the
// vertical axis is magnitude rather than time, so there is no cell under the
// cursor to read; valid comes back 0.
typedef struct visualizer_cursor_sample {
    int valid;                  // cursor is over the plot and inside the grid
    int re;                     // resource element under the cursor
    int row;                    // texture row under the cursor, 0 = top of full view
    int time_offset;            // row as a marker-style offset from the reference row
    int has_symbol;             // whether symbol is meaningful (not rolling-mean)
    int symbol;                 // absolute symbol number
    // Where that symbol sits in the radio frame structure, split the same way
    // the vertical axis labels it. Only when has_symbol and the stream's
    // subcarrier spacing is known.
    int has_timing;
    int frame;                  // absolute frame number, never wraps
    int frame_id;               // frame number as the 8-bit header field carries it
    int subframe;               // 0..9
    int slot;                   // within the subframe
    int slot_symbol;            // 0..13
    int has_phase;              // whether phase is meaningful (rolling-mean only)
    int phase;                  // position within the averaging period
    int has_value;              // 0 when the cell holds no data
    float magnitude;            // absolute IQ magnitude
    float percent_of_baseline;  // relative to the mean magnitude on screen
} visualizer_cursor_sample_t;

// Returns 1 when out_sample->valid is set, 0 otherwise. Coordinates are in
// framebuffer pixels, matching visualizer_get_plot_rect.
int visualizer_sample_at_screen(const opengl_visualizer_t* vis,
                                const fh_oran_memory_t* memory,
                                float screen_x,
                                float screen_y,
                                visualizer_cursor_sample_t* out_sample);

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
//
// Off in Single mode, where it reads back as off and cannot be turned on. The
// choice made for Repeat is kept and applies again once Repeat is selected.
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
