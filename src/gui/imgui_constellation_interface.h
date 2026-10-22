#pragma once

#include "visualization/iq_constellation.h"

#ifdef __cplusplus
extern "C" {
#endif

// The panel and input handling inside the IQ constellation window: its own
// Dear ImGui context, bound to that window alone. The main window keeps the
// context imgui_visualizer_init made; each frame here switches to this one and
// back, and this window's input events are routed to it by callbacks of its
// own, so neither window ever sees the other's mouse or keyboard.
typedef struct imgui_constellation_state imgui_constellation_state_t;

// Sets up the context on an open constellation window. Returns NULL on
// failure. Leaves the caller's GL and ImGui contexts current.
imgui_constellation_state_t* imgui_constellation_init(iq_constellation_t* constellation);

// Tears the context down. Call before iq_constellation_destroy closes the window.
void imgui_constellation_shutdown(imgui_constellation_state_t* state);

// Draws and presents one whole frame of the constellation window: input, the
// plot, axes, overlays and the panel. memory is the stream on screen in the
// main window (may be NULL), selection that window's zoomed area (may be NULL),
// radar the Radar view's pair while that view is on screen and NULL otherwise
// - it adds the panel's H / IQ and DL / UL choices - mean the complex twin of
// the rolling mean on screen (may be NULL), which the Rolling mean data box
// plots, stream_label names the stream for the title and panel, and frozen
// marks the plot while the memories are held. Restores the caller's GL and
// ImGui contexts before returning.
void imgui_constellation_frame(imgui_constellation_state_t* state,
                               iq_constellation_t* constellation,
                               const fh_oran_memory_t* memory,
                               const visualizer_zoom_selection_t* selection,
                               const iq_constellation_radar_pair_t* radar,
                               const visualizer_complex_mean_t* mean,
                               const char* stream_label,
                               bool frozen);

#ifdef __cplusplus
}
#endif
