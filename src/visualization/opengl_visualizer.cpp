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
    float* persist_accum;           // persist_period * HEATMAP_TEXTURE_WIDTH magnitudes
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
    vis->persist_accum = NULL;

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

// Fold newly-arrived symbols (since the last call) into the persistence
// accumulator, keyed by phase = absolute_symbol_number % persist_period.
// Each symbol is folded exactly once, regardless of partial writes it may
// have received before settling in the ring.
static void persist_fold_new_symbols(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis->persist_accum || vis->persist_period <= 0) {
        return;
    }
    if (memory->symbol_highest < memory->symbol_lowest) {
        return;  // no data yet
    }

    int start_symbol;
    if (!vis->persist_has_last_folded) {
        start_symbol = memory->symbol_lowest;
    } else {
        start_symbol = vis->persist_last_folded_symbol + 1;
        if (start_symbol < memory->symbol_lowest) {
            // Gap since we last folded is bigger than the ring itself;
            // whatever fell out of the window is gone, start from what remains.
            start_symbol = memory->symbol_lowest;
        }
    }

    const float alpha = 1.0f / (float)vis->persist_n;

    for (int symbol_num = start_symbol; symbol_num <= memory->symbol_highest; symbol_num++) {
        int age = memory->symbol_highest - symbol_num;
        size_t idx = (memory->index_symbol_highest + memory->max_symbols - (size_t)age) % memory->max_symbols;

        if (memory->symbols[idx] == NULL) {
            continue;
        }
        if (memory->row_symbol_numbers != NULL && memory->row_symbol_numbers[idx] != symbol_num) {
            continue;
        }

        int phase = symbol_num % vis->persist_period;
        if (phase < 0) {
            phase += vis->persist_period;
        }
        float* accum_row = vis->persist_accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH;

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

    vis->persist_last_folded_symbol = memory->symbol_highest;
    vis->persist_has_last_folded = 1;
}

// Update heatmap texture with IQ data from ring buffer (or, in persistence
// mode, from the rolling-mean accumulator).
static void update_heatmap_texture(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis || !vis->texture_data || !memory || !memory->symbols) {
        return;
    }

    if (vis->persist_enabled) {
        persist_fold_new_symbols(vis, memory);
    }

    // Render only a sliding window of the most recent symbols (raw mode), or
    // the full averaging period (persistence mode). Keeping the texture
    // height bounded prevents the visualizer from dominating CPU time.
    int desired_height;
    if (vis->persist_enabled) {
        desired_height = vis->persist_period;
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
        int display_height = (vis->persist_period < vis->tex_height) ? vis->persist_period : vis->tex_height;

        // Find max magnitude for normalization
        float max_magnitude = 1e-9f;
        for (int phase = 0; phase < display_height; phase++) {
            const float* accum_row = vis->persist_accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH;
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

        // Render phase 0 at the top, ascending downward through the period.
        for (int phase = 0; phase < display_height; phase++) {
            const float* accum_row = vis->persist_accum + (size_t)phase * HEATMAP_TEXTURE_WIDTH;
            int tex_row = phase;

            for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                float magnitude = accum_row[col];
                if (magnitude < 0.0f) {
                    magnitude = 0.0f;  // never folded yet
                }

                uint8_t r, g, b;
                magnitude_to_rgb(magnitude, render_scale, &r, &g, &b);

                size_t pixel_idx = (tex_row * HEATMAP_TEXTURE_WIDTH + col) * 4;
                vis->texture_data[pixel_idx + 0] = r;
                vis->texture_data[pixel_idx + 1] = g;
                vis->texture_data[pixel_idx + 2] = b;
                vis->texture_data[pixel_idx + 3] = 255;
            }
        }
    } else {
        int symbol_count = memory->symbol_highest - memory->symbol_lowest + 1;
        int display_height = (symbol_count < vis->tex_height) ? symbol_count : vis->tex_height;

        // Find max magnitude for normalization
        float max_magnitude = 1e-9f;
        for (int sym_offset = 0; sym_offset < display_height; sym_offset++) {
            int symbol_num = memory->symbol_highest - sym_offset;
            int age = memory->symbol_highest - symbol_num;
            size_t idx = (memory->index_symbol_highest + memory->max_symbols - (size_t)age) % memory->max_symbols;

            if (memory->symbols[idx] == NULL) {
                continue;
            }
            if (memory->row_symbol_numbers != NULL && memory->row_symbol_numbers[idx] != symbol_num) {
                continue;
            }

            for (int i = 0; i < HEATMAP_TEXTURE_WIDTH; i++) {
                // Each IQ sample at position i
                size_t iq_idx = i;
                if (iq_idx < 3276) {
                    float i_val, q_val;
                    extract_iq(memory->symbols[idx][iq_idx], i_val, q_val);
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

        // Render heatmap bottom-up (newest symbols at bottom)
        for (int sym_offset = 0; sym_offset < display_height; sym_offset++) {
            int symbol_num = memory->symbol_highest - sym_offset;
            int age = memory->symbol_highest - symbol_num;
            size_t idx = (memory->index_symbol_highest + memory->max_symbols - (size_t)age) % memory->max_symbols;

            if (memory->symbols[idx] == NULL) {
                continue;
            }
            if (memory->row_symbol_numbers != NULL && memory->row_symbol_numbers[idx] != symbol_num) {
                continue;
            }

            // Texture row is inverted: newest at bottom
            int tex_row = vis->tex_height - 1 - sym_offset;

            for (int col = 0; col < HEATMAP_TEXTURE_WIDTH; col++) {
                size_t iq_idx = col;
                if (iq_idx >= 3276) {
                    iq_idx = 3275;  // Clamp to last sample
                }

                float i_val, q_val;
                extract_iq(memory->symbols[idx][iq_idx], i_val, q_val);
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
    
    // Update texture from memory
    update_heatmap_texture(vis, memory);
    
    // Render with immediate mode - draw fullscreen quad
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, vis->texture_id);
    
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f((float)vis->width, 0);
    glTexCoord2f(1, 1); glVertex2f((float)vis->width, (float)vis->height);
    glTexCoord2f(0, 1); glVertex2f(0, (float)vis->height);
    glEnd();
    
    glDisable(GL_TEXTURE_2D);
    
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
    if (vis->window) {
        glfwDestroyWindow(vis->window);
    }
    
    glfwTerminate();
    free(vis);
}

