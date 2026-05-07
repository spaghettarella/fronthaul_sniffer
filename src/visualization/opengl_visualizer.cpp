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

typedef struct opengl_visualizer {
    GLFWwindow* window;
    GLuint texture_id;
    int width;
    int height;
    int tex_height; // number of symbol rows in the texture
    int tex_max_height; // maximum texture height supported by GL
    int is_running;
    uint8_t* texture_data;  // RGBA framebuffer
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

opengl_visualizer_t* visualizer_create(int width, int height) {
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
        // Query GL maximum texture size (applies to width/height); use it to bound our texture height
        GLint max_tex_size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);
        if (max_tex_size <= 0) {
            max_tex_size = 4096; // fallback conservative default
        }
        vis->tex_height = HEATMAP_DEFAULT_TEXTURE_HEIGHT;
        if (vis->tex_height > max_tex_size) vis->tex_height = max_tex_size;
        vis->tex_max_height = max_tex_size;
    vis->is_running = 1;
    
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

// Update heatmap texture with IQ data from ring buffer
static void update_heatmap_texture(opengl_visualizer_t* vis, const fh_oran_memory_t* memory) {
    if (!vis || !vis->texture_data || !memory || !memory->symbols) {
        return;
    }
    
    // Ensure texture size matches memory size (dynamic height)
    int desired_height = vis->tex_height;
    if (memory && memory->max_symbols > 0) {
        // Prefer to match memory size, but clamp to GL max texture size
        int mem_h = (int)memory->max_symbols;
        int cap = vis->tex_max_height > 0 ? vis->tex_max_height : 4096;
        desired_height = (mem_h > cap) ? cap : mem_h;
        if (desired_height <= 0) desired_height = HEATMAP_DEFAULT_TEXTURE_HEIGHT;
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
    
    // Calculate which symbols to display (most recent ones)
    int symbol_count = (memory->symbol_highest >= memory->symbol_lowest) ? 
                       (memory->symbol_highest - memory->symbol_lowest + 1) : 0;
    if (symbol_count <= 0) {
        return;  // No data yet
    }
    
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
    
    // Render heatmap bottom-up (newest symbols at bottom)
    for (int sym_offset = 0; sym_offset < display_height; sym_offset++) {
        int symbol_num = memory->symbol_highest - sym_offset;
        int age = memory->symbol_highest - symbol_num;
        size_t idx = (memory->index_symbol_highest + memory->max_symbols - (size_t)age) % memory->max_symbols;
        
        if (memory->symbols[idx] == NULL) {
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
            magnitude_to_rgb(magnitude, max_magnitude, &r, &g, &b);
            
            size_t pixel_idx = (tex_row * HEATMAP_TEXTURE_WIDTH + col) * 4;
            vis->texture_data[pixel_idx + 0] = r;      // R
            vis->texture_data[pixel_idx + 1] = g;      // G
            vis->texture_data[pixel_idx + 2] = b;      // B
            vis->texture_data[pixel_idx + 3] = 255;    // A
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
    if (vis->window) {
        glfwDestroyWindow(vis->window);
    }
    
    glfwTerminate();
    free(vis);
}

