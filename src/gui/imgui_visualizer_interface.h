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
// radar: radar core the Radar Settings window edits; may be NULL
// out_radar_dl, out_radar_ul: both directions of the selected eAxC, for the
//   radar, which needs the pair rather than the single displayed stream. Either
//   is set to NULL when that direction has not been seen. Optional.
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
    struct fh_radar* radar,
    struct fh_oran_memory** out_radar_dl,
    struct fh_oran_memory** out_radar_ul);

// Shutdown ImGui and release resources
void imgui_visualizer_shutdown(imgui_visualizer_state_t* state);

// Helper to get the string representation of a data_direction
const char* data_direction_to_string(uint8_t data_direction);

#ifdef __cplusplus
}
#endif
