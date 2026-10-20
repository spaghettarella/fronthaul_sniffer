#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "pipeline/c_packet_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
struct fh_oran_memory;
struct fh_radar;
typedef struct opengl_visualizer opengl_visualizer_t;

typedef struct fh_memory_key {
    uint16_t eaxc_id;
    uint8_t data_direction;

    bool operator==(const fh_memory_key& other) const {
        return eaxc_id == other.eaxc_id && data_direction == other.data_direction;
    }
} fh_memory_key_t;

#ifdef __cplusplus
}
#endif

// Hash function for unordered_map (must be in global namespace for C++)
namespace std {
template <>
struct hash<fh_memory_key_t> {
    std::size_t operator()(const fh_memory_key_t& key) const {
        return (static_cast<std::size_t>(key.eaxc_id) << 8) ^ static_cast<std::size_t>(key.data_direction);
    }
};
}  // namespace std

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for ImGui interface state
typedef struct imgui_visualizer_state imgui_visualizer_state_t;

// Initialize ImGui for GLFW window integration
// Call once after the OpenGL context is created but before any ImGui rendering
// window_width, window_height: dimensions of the GLFW window
// Returns a state handle on success, NULL on failure
imgui_visualizer_state_t* imgui_visualizer_init(int window_width, int window_height);

// Render the finalized ImGui draw data into the current OpenGL backbuffer
// Call this after the scene has been drawn and before the buffer swap
void imgui_visualizer_present(void);

// Render the stream selection interface
// Should be called once per frame, after OpenGL setup but before buffer swap
// state: the state handle from imgui_visualizer_init
// keys: array of discovered memory keys
// num_keys: number of keys in the array
// current_display_memory: pointer to the currently displayed memory (can be NULL)
// source_name: human-readable capture source (interface, pcap file, tap queue)
//   shown in the statistics window; may be NULL
// pcap_speed_pps: replay speed of a pcap source, in packets per second, which
//   the statistics window edits in place with __atomic builtins while the
//   capture thread reads it. NULL when the source is not a pcap file.
// decode_selection: the capture's running selection word (see
//   fh_channel_selection_pack), which Stream Selection's "Only decode
//   selected" sets to the channels on screen and to 0 when unticked. Written
//   with __atomic_store_n. May be NULL.
// radar: radar core the Radar Settings window edits; may be NULL
// out_radar_dl, out_radar_ul: the DL and UL rings the radar should pair, which
//   may come from different eAxCs (picked in the Stream Selection table in
//   radar mode). Either is set to NULL when that direction has not been seen
//   on its eAxC. Optional.
// window_width, window_height: current window dimensions
// Returns the display_memory pointer (either unchanged or updated based on user selection)
struct fh_oran_memory* imgui_visualizer_render(
    imgui_visualizer_state_t* state,
    const fh_memory_key_t* keys,
    size_t num_keys,
    struct fh_oran_memory** memory_ptrs,
    struct fh_oran_memory* current_display_memory,
    int window_width,
    int window_height,
    fh_queue_stats_t stats,
    opengl_visualizer_t* visualizer,
    const char* source_name,
    int* pcap_speed_pps,
    uint64_t* decode_selection,
    struct fh_radar* radar,
    struct fh_oran_memory** out_radar_dl,
    struct fh_oran_memory** out_radar_ul);

// Whether the view's Freeze is on. Frozen, the memories must not be written
// to any more while the capture itself carries on, so that every channel and
// view shows the same moment; the caller applies that to its decode workers.
bool imgui_visualizer_get_frozen(const imgui_visualizer_state_t* state);

// Whether the IQ Constellation box in the View Mode window is ticked. The
// caller opens and closes the constellation window to match, and unticks the
// box with the setter when that window is closed from its own title bar or
// could not be opened at all.
bool imgui_visualizer_get_constellation_enabled(const imgui_visualizer_state_t* state);
void imgui_visualizer_set_constellation_enabled(imgui_visualizer_state_t* state, bool enabled);

// Shutdown ImGui and release resources
void imgui_visualizer_shutdown(imgui_visualizer_state_t* state);

// Helper to get the string representation of a data_direction
const char* data_direction_to_string(uint8_t data_direction);

#ifdef __cplusplus
}
#endif
