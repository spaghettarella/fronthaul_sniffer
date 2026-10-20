#pragma once

#include "visualization/opengl_visualizer.h"
#include "core/c_radar.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- IQ constellation window ---
// A second, free-standing GLFW window plotting every IQ sample of one stream on
// the complex plane: I to the right, Q up, one shared scale for both. It sits
// beside the main window rather than inside it, so it can be dragged onto a
// monitor of its own and resized to any shape.
//
// Every sample is one dot. The samples are binned into one counter per screen
// pixel and the counts drawn as a texture, which is what keeps millions of
// samples per frame affordable on the fixed-function pipeline this tool uses:
// the cost scales with the samples read, not with how many get drawn.
//
// This module owns the window, its GL objects, the binning and the zoom; the
// panel and the input handling inside the window live in
// gui/imgui_constellation_interface, the same split as opengl_visualizer and
// imgui_visualizer_interface. All calls belong on the thread that owns the
// main window.

struct GLFWwindow;

typedef struct iq_constellation iq_constellation_t;

typedef enum iq_constellation_source {
    IQ_CONSTELLATION_SOURCE_ALL = 0,    // every symbol in the memory, every resource element
    IQ_CONSTELLATION_SOURCE_ZOOMED = 1  // only what the main view's zoom window covers
} iq_constellation_source_t;

// What the constellation plots while the main view is Radar, which reads a DL
// and a UL ring - possibly of two different eAxCs - rather than one stream.
// Outside the Radar view it always plots the IQ samples of the stream on screen.
typedef enum iq_constellation_radar_data {
    IQ_CONSTELLATION_RADAR_CFR = 0, // complex H per resource element, divided as the radar divides it
    IQ_CONSTELLATION_RADAR_IQ = 1   // IQ samples of one direction of the pair
} iq_constellation_radar_data_t;

typedef enum iq_constellation_direction {
    IQ_CONSTELLATION_DL = 0,
    IQ_CONSTELLATION_UL = 1
} iq_constellation_direction_t;

// The Radar view's pair, handed over every frame the main view is Radar.
typedef struct iq_constellation_radar_pair {
    const fh_oran_memory_t* dl;   // NULL when that direction has not been captured
    const fh_oran_memory_t* ul;
    fh_radar_config_t config;     // H is divided with its lag, division and frequency range
    const char* dl_label;         // names the rings, e.g. "eAxC 0x0001"; may be NULL
    const char* ul_label;
} iq_constellation_radar_pair_t;

// What survives the window being closed and opened again: where it was, how big,
// and how it was set up. The caller keeps one of these for the whole run.
typedef struct iq_constellation_prefs {
    int have_geometry;  // x, y, width, height describe a window to come back to
    int x;
    int y;
    int width;
    int height;
    iq_constellation_source_t source;
    int skip_empty;     // leave out samples that are exactly 0+0j (unscheduled resource elements)
    int density;        // colour by hits per pixel; 0 draws every hit pixel the same colour
    int dot_size;       // grow each hit pixel into a dot sized by its hits; 0 keeps single pixels
    iq_constellation_radar_data_t radar_data;     // Radar view only
    iq_constellation_direction_t radar_direction; // Radar view, IQ samples only
} iq_constellation_prefs_t;

void iq_constellation_default_prefs(iq_constellation_prefs_t* out);

// Opens the window, 800x800 unless prefs remember another size and position.
// Must be called with the main window's context current: the new context
// shares its objects, and the caller's context is current again on return.
// Returns NULL when the window cannot be created.
iq_constellation_t* iq_constellation_create(const iq_constellation_prefs_t* prefs);

// Closes the window. When out_prefs is non-NULL it receives the window's last
// geometry and settings, ready for the next create.
void iq_constellation_destroy(iq_constellation_t* c, iq_constellation_prefs_t* out_prefs);

// The window's own close button was pressed.
int iq_constellation_close_requested(const iq_constellation_t* c);

struct GLFWwindow* iq_constellation_window(const iq_constellation_t* c);

// Names the stream on show in the title bar; NULL or "" for none. Only touches
// the window when the text actually changes.
void iq_constellation_set_title(iq_constellation_t* c, const char* stream_label);

// Makes the window's context current and clears it, remembering whose context
// to hand back. Returns 0 when there is nothing to draw into (minimised); the
// frame must still be closed with end_frame either way.
int iq_constellation_begin_frame(iq_constellation_t* c);

// Presents the frame when present is non-zero, then makes the caller's context
// current again.
void iq_constellation_end_frame(iq_constellation_t* c, int present);

// Brings the plot up to date with the memory and draws it. memory may be NULL
// (no stream yet). selection is the main view's zoomed area, applied when the
// source is IQ_CONSTELLATION_SOURCE_ZOOMED; may be NULL. radar is the Radar
// view's pair while that view is on screen, NULL otherwise; with it, the radar
// data and direction settings decide what is plotted instead of memory, and a
// zoomed area - whose rows are DL symbols there - is moved by the lag when the
// UL is plotted, onto the UL symbols those rows were paired with. Rebuilds only when
// something it depends on has changed, and for new data at most ~30 times a
// second - less often when one rebuild is expensive, so a deep memory cannot
// stall the main window - and a frozen memory costs nothing once it is drawn.
void iq_constellation_update(iq_constellation_t* c,
                             const fh_oran_memory_t* memory,
                             const visualizer_zoom_selection_t* selection,
                             const iq_constellation_radar_pair_t* radar);

// --- Settings ---
void iq_constellation_set_source(iq_constellation_t* c, iq_constellation_source_t source);
iq_constellation_source_t iq_constellation_get_source(const iq_constellation_t* c);
void iq_constellation_set_skip_empty(iq_constellation_t* c, int enabled);
int iq_constellation_get_skip_empty(const iq_constellation_t* c);
void iq_constellation_set_density(iq_constellation_t* c, int enabled);
int iq_constellation_get_density(const iq_constellation_t* c);
// Dot size by density: denser pixels draw as bigger dots, up to a few pixels
// across, sparse ones as single pixels. On by default.
void iq_constellation_set_dot_size(iq_constellation_t* c, int enabled);
int iq_constellation_get_dot_size(const iq_constellation_t* c);
// Radar view only: H, or the IQ of one direction. H by default, UL by default.
void iq_constellation_set_radar_data(iq_constellation_t* c, iq_constellation_radar_data_t data);
iq_constellation_radar_data_t iq_constellation_get_radar_data(const iq_constellation_t* c);
void iq_constellation_set_radar_direction(iq_constellation_t* c, iq_constellation_direction_t direction);
iq_constellation_direction_t iq_constellation_get_radar_direction(const iq_constellation_t* c);

// --- Zoom ---
// Normalised like the waterfall's: 0..1 across the full view on each axis, u
// running right with I and v running down, i.e. against Q. The window is
// square - I and Q always share a scale - so one span covers both axes; 1.0 is
// the full view, 0.25 a 4x zoom. The full view is the square of side 2A
// centred on the origin, A being fitted to the data (see the view below). It
// fills the plot's shorter side, so a window that is not square shows extra
// range along its longer one.
void iq_constellation_get_zoom(const iq_constellation_t* c, float* out_center_u, float* out_center_v, float* out_span);
int iq_constellation_zoom_is_active(const iq_constellation_t* c);

// Back to the full view, refitting the full view to the data on the next update.
void iq_constellation_reset_zoom(iq_constellation_t* c);

// Centres the zoom window on a normalised point, as a click in a minimap does.
void iq_constellation_set_center(iq_constellation_t* c, float center_u, float center_v);

// Scales the zoom by factor (>1 zooms in) about a point in framebuffer pixels,
// keeping the data under that point where it is.
void iq_constellation_zoom_by(iq_constellation_t* c, float factor, float focus_px_x, float focus_px_y);

// Moves the view by a fraction of the zoom window's span, per arrow keypress.
void iq_constellation_pan_by(iq_constellation_t* c, float u_fraction, float v_fraction);

// Moves the view along with a pointer drag of dx, dy framebuffer pixels, so the
// data under the pointer travels with it.
void iq_constellation_pan_px(iq_constellation_t* c, float dx_px, float dy_px);

// --- View ---
// What the plot rect currently shows, in framebuffer pixels and IQ units, for
// axes, readouts and minimaps.
typedef struct iq_constellation_view {
    int framebuffer_width;
    int framebuffer_height;
    float plot_x;
    float plot_y;
    float plot_w;
    float plot_h;
    float i_min;      // left edge
    float i_max;      // right edge
    float q_min;      // bottom edge
    float q_max;      // top edge
    float extent;     // A: the full view spans +/-A on both axes
    int extent_fitted; // A was fitted to data, rather than the +/-1 placeholder
} iq_constellation_view_t;

void iq_constellation_get_view(const iq_constellation_t* c, iq_constellation_view_t* out);

// Inverts the view: a framebuffer position to the I/Q it shows.
void iq_constellation_screen_to_iq(const iq_constellation_t* c, float px, float py, float* out_i, float* out_q);

// --- What was plotted ---
// Why a picture holds nothing at all, for the plot's notice.
typedef enum iq_constellation_missing {
    IQ_CONSTELLATION_MISSING_NONE = 0,
    IQ_CONSTELLATION_MISSING_STREAM = 1, // no stream on screen yet
    IQ_CONSTELLATION_MISSING_DL = 2,     // Radar view: the pair's DL has not been captured
    IQ_CONSTELLATION_MISSING_UL = 3      // Radar view: the pair's UL has not been captured
} iq_constellation_missing_t;

typedef struct iq_constellation_stats {
    int has_memory;
    iq_constellation_missing_t missing;
    int radar_view;               // built from the Radar view's pair
    int showing_cfr;              // the picture is H, not IQ samples
    iq_constellation_direction_t direction; // IQ in the Radar view: which direction
    int symbol_shift;             // added to the zoomed area's symbols (the lag, for UL)
    int symbols_plotted;          // symbols binned into the picture; paired symbols for H
    int symbols_selected;         // symbols the selection asked for that the memory holds
    long long samples_plotted;    // samples binned (after skipping empty ones)
    int budget_hit;               // symbols_plotted < symbols_selected: only the newest were read
    // The selection actually in force for the picture on screen: shifted by
    // symbol_shift, and for H narrowed to the radar's frequency range.
    int re_first;
    int re_last;
    visualizer_selection_time_t time;
    int symbol_first;
    int symbol_last;
    int phase_first;
    int phase_count;
    int period;
    int selection_from_zoom;      // the main view's zoom narrowed it
    int main_view_zoomed;
    int main_view_valid;
} iq_constellation_stats_t;

void iq_constellation_get_stats(const iq_constellation_t* c, iq_constellation_stats_t* out);

// Overview of the full view at a fixed resolution, for the panel's minimap:
// a GL texture of iq_constellation_map_size() square pixels, binned in the same
// pass as the plot.
unsigned int iq_constellation_map_texture(const iq_constellation_t* c);
int iq_constellation_map_size(void);

#ifdef __cplusplus
}
#endif
