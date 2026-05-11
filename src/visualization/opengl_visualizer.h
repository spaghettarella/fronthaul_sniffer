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

#ifdef __cplusplus
}
#endif
