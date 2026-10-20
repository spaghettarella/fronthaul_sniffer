#include "visualization/iq_constellation.h"
#include "core/c_oran_memory.h"

#include <GLFW/glfw3.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <limits.h>
#include <algorithm>
#include <new>
#include <string>
#include <vector>

#define IQC_DEFAULT_SIZE 800

// Side of the overview map in the panel, in pixels. Binned in the same pass as
// the plot.
#define IQC_MAP_PX 192

// Samples the overview map is binned from, at most. At 192 pixels a million
// samples already show everything a thumbnail can; binning all of them took
// as long again as the plot itself (measured: 4.4 of 11.3 ms at the default
// Memory size). Past this, the map takes every n-th symbol. The plot always
// takes every sample.
#define IQC_MAP_SAMPLE_TARGET 1000000LL

// Room left around the plot for the tick labels: Q values to the left, I
// values and the axis title below.
#define IQC_MARGIN_LEFT 64.0f
#define IQC_MARGIN_BOTTOM 40.0f
#define IQC_MARGIN_TOP 12.0f
#define IQC_MARGIN_RIGHT 20.0f

// Deepest zoom, 1000x: at the default full view that is a few thousandths of
// full scale across the whole plot, far finer than any quantisation in the
// decoded samples.
#define IQC_MIN_SPAN 0.001f

// Most samples read for one picture. About a dozen milliseconds of binning on
// the render thread; the default Memory size of 1120 symbols is 3.7 M samples,
// but Memory size can reach 573 440 symbols, nearly 1.9 G. Past the budget the
// newest symbols are drawn and the panel says how many were left out.
#define IQC_SAMPLE_BUDGET 8000000LL

// New data alone rebuilds the picture at most this often, and never more often
// than IQC_DATA_REBUILD_COST_FACTOR times the last rebuild took, so a deep
// memory slows the constellation's refresh instead of the main window's.
// Anything that changes how the picture maps to the screen rebuilds it at once.
#define IQC_DATA_REBUILD_INTERVAL_S (1.0 / 30.0)
#define IQC_DATA_REBUILD_COST_FACTOR 3.0

// The full view is the largest |I| or |Q| plotted with this much room around it.
#define IQC_EXTENT_MARGIN 1.1f

// Lowest point on the colour scale a pixel with any hit at all gets, so a lone
// sample still shows against the black of a pixel with none.
#define IQC_DENSITY_FLOOR 0.15f

// Counts below this get their colour from a table built once per picture
// rather than from a log per pixel.
#define IQC_DENSITY_LUT_COUNTS 1024

// Dot size by density: the densest pixel's dot is this many pixels in radius,
// on the plot and on the overview map; sparser pixels shrink toward a single
// pixel on the same log scale as the colour.
#define IQC_DOT_MAX_RADIUS 3
#define IQC_MAP_DOT_MAX_RADIUS 1

// The Curve view's phosphor green, for the uniform-colour mode.
#define IQC_UNIFORM_R 89
#define IQC_UNIFORM_G 255
#define IQC_UNIFORM_B 128

// The selection a picture is built from: the main view's zoom when the source
// asks for it, otherwise everything.
struct iqc_selection {
    int re_first;
    int re_last;
    visualizer_selection_time_t time;
    int symbol_first;
    int symbol_last;
    int phase_first;
    int phase_count;
    int period;
    int from_zoom;
    int main_view_zoomed;
    int main_view_valid;
};

// Where the samples come from: one ring's IQ, or H divided out of the radar's
// pair. Resolved once per update from the settings and the Radar view's pair.
struct iqc_input {
    int cfr;                          // H of memory (DL) against ul
    const fh_oran_memory_t* memory;   // the ring read: the IQ ring, or the DL for H
    const fh_oran_memory_t* ul;       // H only
    fh_radar_config_t config;         // H only
    int radar_view;
    iq_constellation_direction_t direction;
    int symbol_shift;                 // moves the zoomed area's symbols onto this ring's
    iq_constellation_missing_t missing;
};

// What decides how samples map to pixels. Any change here rebuilds at once.
// Zero-filled before use and compared with memcmp, so padding never differs.
struct iqc_view_key {
    const fh_oran_memory_t* memory;
    const fh_oran_memory_t* ul;
    int cfr;
    int lag;
    int division;
    int freq_start_re;
    int freq_end_re;
    int plot_w;
    int plot_h;
    float center_u;
    float center_v;
    float span;
    float extent;
    int refit;
    int source;
    int skip_empty;
    int density;
    int dot_size;
    int re_first;
    int re_last;
    int time;
    int phase_first;
    int phase_count;
    int period;
};

// What only says new data has arrived. Rebuilds are paced.
struct iqc_data_key {
    int symbol_highest;
    int ul_symbol_highest;
    int symbol_first;
    int symbol_last;
};

// What a picture is of, as far as its scale goes: a change here is a new
// signal, and the full view is fitted to it afresh.
struct iqc_identity {
    const fh_oran_memory_t* memory;
    const fh_oran_memory_t* ul;
    int cfr;
    int division;
};

struct iq_constellation {
    GLFWwindow* window = nullptr;
    GLFWwindow* caller_window = nullptr;  // whose context begin_frame took over

    GLuint plot_texture = 0;
    int plot_texture_w = 0;
    int plot_texture_h = 0;
    GLuint map_texture = 0;

    std::vector<uint32_t> plot_counts;
    std::vector<uint8_t> plot_pixels;
    std::vector<uint32_t> map_counts;
    std::vector<uint8_t> map_pixels;
    std::vector<uint32_t> dot_counts;  // counts grown into dots, when dot size follows density
    std::vector<fh_complex_t> cfr_row; // one symbol's H, packed

    int fb_w = 0;
    int fb_h = 0;

    float center_u = 0.5f;
    float center_v = 0.5f;
    float span = 1.0f;

    // A, the half-width of the full view. +/-1 until data has been seen, the
    // scale every capture source normalises to.
    float extent = 1.0f;
    int extent_fitted = 0;
    int refit = 1;

    iq_constellation_source_t source = IQ_CONSTELLATION_SOURCE_ALL;
    int skip_empty = 1;
    int density = 1;
    int dot_size = 1;
    iq_constellation_radar_data_t radar_data = IQ_CONSTELLATION_RADAR_CFR;
    iq_constellation_direction_t radar_direction = IQ_CONSTELLATION_UL;

    int have_built = 0;
    iqc_view_key last_view;
    iqc_data_key last_data;
    double last_build_time = 0.0;
    double last_build_cost = 0.0;  // seconds the last rebuild took
    iqc_identity last_identity;
    iq_constellation_stats_t stats;

    std::string title;
    int title_set = 0;

    uint8_t palette[256][3];
};

void iq_constellation_default_prefs(iq_constellation_prefs_t* out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->have_geometry = 0;
    out->width = IQC_DEFAULT_SIZE;
    out->height = IQC_DEFAULT_SIZE;
    out->source = IQ_CONSTELLATION_SOURCE_ALL;
    out->skip_empty = 1;
    out->density = 1;
    out->dot_size = 1;
    out->radar_data = IQ_CONSTELLATION_RADAR_CFR;
    out->radar_direction = IQ_CONSTELLATION_UL;
}

static float iqc_guard(float value, float fallback) {
    // Rejects NaN and infinities, which would otherwise stick in the zoom
    // window with no way back short of Reset.
    if (!(value > -1e30f && value < 1e30f)) {
        return fallback;
    }
    return value;
}

// Keeps the zoom window a real part of the full view: no wider than it, no
// narrower than the deepest zoom, and never hanging off an edge.
static void iqc_normalize_zoom(iq_constellation_t* c) {
    float span = iqc_guard(c->span, 1.0f);
    if (span < IQC_MIN_SPAN) {
        span = IQC_MIN_SPAN;
    }
    if (span > 1.0f) {
        span = 1.0f;
    }
    const float half = span * 0.5f;
    float cu = iqc_guard(c->center_u, 0.5f);
    float cv = iqc_guard(c->center_v, 0.5f);
    cu = (cu < half) ? half : (cu > 1.0f - half) ? 1.0f - half : cu;
    cv = (cv < half) ? half : (cv > 1.0f - half) ? 1.0f - half : cv;
    c->span = span;
    c->center_u = cu;
    c->center_v = cv;
}

// The plot rect, in whole framebuffer pixels so one counter maps to exactly one
// pixel of it.
static void iqc_plot_rect(const iq_constellation_t* c, int* x, int* y, int* w, int* h) {
    const int px = (int)IQC_MARGIN_LEFT;
    const int py = (int)IQC_MARGIN_TOP;
    int pw = c->fb_w - (int)IQC_MARGIN_LEFT - (int)IQC_MARGIN_RIGHT;
    int ph = c->fb_h - (int)IQC_MARGIN_TOP - (int)IQC_MARGIN_BOTTOM;
    if (pw < 1) {
        pw = 1;
    }
    if (ph < 1) {
        ph = 1;
    }
    *x = px;
    *y = py;
    *w = pw;
    *h = ph;
}

// Pixels per unit of the normalised full view. The zoom window is square and
// fills the plot's shorter side.
static float iqc_px_per_norm(const iq_constellation_t* c, int plot_w, int plot_h) {
    const float side = (float)(plot_w < plot_h ? plot_w : plot_h);
    return side / (c->span > 1e-6f ? c->span : 1e-6f);
}

iq_constellation_t* iq_constellation_create(const iq_constellation_prefs_t* prefs) {
    iq_constellation_prefs_t p;
    if (prefs != NULL) {
        p = *prefs;
    } else {
        iq_constellation_default_prefs(&p);
    }

    GLFWwindow* caller = glfwGetCurrentContext();

    const int width = (p.have_geometry && p.width > 0) ? p.width : IQC_DEFAULT_SIZE;
    const int height = (p.have_geometry && p.height > 0) ? p.height : IQC_DEFAULT_SIZE;

    // Created hidden so a remembered position applies before it first appears,
    // rather than the window showing up somewhere else and jumping.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(width, height, "IQ Constellation", NULL, caller);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (window == NULL) {
        fprintf(stderr, "Failed to create the IQ Constellation window\n");
        return NULL;
    }
    if (p.have_geometry) {
        glfwSetWindowPos(window, p.x, p.y);
    }
    glfwShowWindow(window);

    iq_constellation_t* c = new (std::nothrow) iq_constellation();
    if (c == NULL) {
        glfwDestroyWindow(window);
        return NULL;
    }
    c->window = window;
    c->source = (p.source == IQ_CONSTELLATION_SOURCE_ZOOMED) ? IQ_CONSTELLATION_SOURCE_ZOOMED
                                                              : IQ_CONSTELLATION_SOURCE_ALL;
    c->skip_empty = p.skip_empty ? 1 : 0;
    c->density = p.density ? 1 : 0;
    c->dot_size = p.dot_size ? 1 : 0;
    c->radar_data = (p.radar_data == IQ_CONSTELLATION_RADAR_IQ) ? IQ_CONSTELLATION_RADAR_IQ
                                                                 : IQ_CONSTELLATION_RADAR_CFR;
    c->radar_direction = (p.radar_direction == IQ_CONSTELLATION_DL) ? IQ_CONSTELLATION_DL : IQ_CONSTELLATION_UL;
    c->cfr_row.resize(VISUALIZER_RE_COUNT);
    memset(&c->last_view, 0, sizeof(c->last_view));
    memset(&c->last_data, 0, sizeof(c->last_data));
    memset(&c->last_identity, 0, sizeof(c->last_identity));
    memset(&c->stats, 0, sizeof(c->stats));
    c->stats.re_last = VISUALIZER_RE_COUNT - 1;

    for (int i = 0; i < 256; i++) {
        visualizer_heatmap_rgb((float)i / 255.0f, &c->palette[i][0], &c->palette[i][1], &c->palette[i][2]);
    }

    glfwMakeContextCurrent(window);
    // The main window already waits for vblank; a second vsynced swap in the
    // same loop would wait for another one and halve both windows' frame rate.
    glfwSwapInterval(0);

    glGenTextures(1, &c->plot_texture);
    glBindTexture(GL_TEXTURE_2D, c->plot_texture);
    // One texel per screen pixel, so nothing to filter.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &c->map_texture);
    glBindTexture(GL_TEXTURE_2D, c->map_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    c->map_counts.assign((size_t)IQC_MAP_PX * IQC_MAP_PX, 0);
    c->map_pixels.assign((size_t)IQC_MAP_PX * IQC_MAP_PX * 4, 0);
    for (size_t i = 3; i < c->map_pixels.size(); i += 4) {
        c->map_pixels[i] = 255;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, IQC_MAP_PX, IQC_MAP_PX, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 c->map_pixels.data());

    glfwGetFramebufferSize(window, &c->fb_w, &c->fb_h);

    glfwMakeContextCurrent(caller);
    return c;
}

void iq_constellation_destroy(iq_constellation_t* c, iq_constellation_prefs_t* out_prefs) {
    if (c == NULL) {
        return;
    }

    if (out_prefs != NULL) {
        int x = 0, y = 0, w = 0, h = 0;
        glfwGetWindowPos(c->window, &x, &y);
        glfwGetWindowSize(c->window, &w, &h);
        out_prefs->have_geometry = (w > 0 && h > 0) ? 1 : 0;
        out_prefs->x = x;
        out_prefs->y = y;
        out_prefs->width = w;
        out_prefs->height = h;
        out_prefs->source = c->source;
        out_prefs->skip_empty = c->skip_empty;
        out_prefs->density = c->density;
        out_prefs->dot_size = c->dot_size;
        out_prefs->radar_data = c->radar_data;
        out_prefs->radar_direction = c->radar_direction;
    }

    GLFWwindow* caller = glfwGetCurrentContext();
    glfwMakeContextCurrent(c->window);
    if (c->plot_texture) {
        glDeleteTextures(1, &c->plot_texture);
    }
    if (c->map_texture) {
        glDeleteTextures(1, &c->map_texture);
    }
    glfwMakeContextCurrent(caller == c->window ? NULL : caller);
    glfwDestroyWindow(c->window);
    delete c;
}

int iq_constellation_close_requested(const iq_constellation_t* c) {
    return (c != NULL && c->window != NULL) ? glfwWindowShouldClose(c->window) : 0;
}

struct GLFWwindow* iq_constellation_window(const iq_constellation_t* c) {
    return c ? c->window : NULL;
}

void iq_constellation_set_title(iq_constellation_t* c, const char* stream_label) {
    if (c == NULL) {
        return;
    }
    std::string title = "IQ Constellation";
    if (stream_label != NULL && stream_label[0] != '\0') {
        title += " - ";
        title += stream_label;
    }
    if (c->title_set && title == c->title) {
        return;
    }
    glfwSetWindowTitle(c->window, title.c_str());
    c->title = title;
    c->title_set = 1;
}

int iq_constellation_begin_frame(iq_constellation_t* c) {
    if (c == NULL) {
        return 0;
    }
    c->caller_window = glfwGetCurrentContext();
    glfwMakeContextCurrent(c->window);

    int w = 0, h = 0;
    glfwGetFramebufferSize(c->window, &w, &h);
    if (w <= 0 || h <= 0) {
        return 0;
    }
    c->fb_w = w;
    c->fb_h = h;

    // Same top-left pixel space the main window draws in.
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    return 1;
}

void iq_constellation_end_frame(iq_constellation_t* c, int present) {
    if (c == NULL) {
        return;
    }
    if (present) {
        glfwSwapBuffers(c->window);
    }
    glfwMakeContextCurrent(c->caller_window);
    c->caller_window = NULL;
}

// The selection the next picture is built from. The main view's zoom is only
// borrowed when the source asks for it and that view has a grid to describe.
static iqc_selection iqc_effective_selection(const iq_constellation_t* c, const visualizer_zoom_selection_t* zoom) {
    iqc_selection s;
    memset(&s, 0, sizeof(s));
    s.re_first = 0;
    s.re_last = VISUALIZER_RE_COUNT - 1;
    s.time = VISUALIZER_SELECTION_TIME_ALL;
    if (zoom != NULL) {
        s.main_view_zoomed = zoom->zoomed;
        s.main_view_valid = zoom->valid;
    }
    if (c->source != IQ_CONSTELLATION_SOURCE_ZOOMED || zoom == NULL || !zoom->valid) {
        return s;
    }

    s.re_first = zoom->re_first;
    s.re_last = zoom->re_last;
    if (s.re_first < 0) {
        s.re_first = 0;
    }
    if (s.re_last > VISUALIZER_RE_COUNT - 1) {
        s.re_last = VISUALIZER_RE_COUNT - 1;
    }
    if (s.re_last < s.re_first) {
        s.re_last = s.re_first;
    }

    s.time = zoom->time;
    if (s.time == VISUALIZER_SELECTION_TIME_SYMBOLS) {
        s.symbol_first = zoom->symbol_first;
        s.symbol_last = zoom->symbol_last;
    } else if (s.time == VISUALIZER_SELECTION_TIME_PHASES) {
        if (zoom->period > 0 && zoom->phase_count > 0) {
            s.phase_first = zoom->phase_first;
            s.phase_count = zoom->phase_count < zoom->period ? zoom->phase_count : zoom->period;
            s.period = zoom->period;
        } else {
            s.time = VISUALIZER_SELECTION_TIME_ALL;
        }
    }
    s.from_zoom = (s.re_first > 0 || s.re_last < VISUALIZER_RE_COUNT - 1 ||
                   s.time != VISUALIZER_SELECTION_TIME_ALL) ? 1 : 0;
    return s;
}

// Turns counts into pixels: black where nothing landed, otherwise either the
// waterfall's gradient on a log scale of the count, or one flat colour.
static void iqc_colourize(const iq_constellation_t* c,
                          const std::vector<uint32_t>& counts,
                          std::vector<uint8_t>& pixels) {
    pixels.resize(counts.size() * 4);

    uint32_t max_count = 0;
    for (uint32_t n : counts) {
        if (n > max_count) {
            max_count = n;
        }
    }

    // Colour index per small count, so the common case is a table read.
    uint8_t lut[IQC_DENSITY_LUT_COUNTS];
    const float inv_log_max = (max_count > 0) ? 1.0f / log1pf((float)max_count) : 0.0f;
    const auto density_index = [&](uint32_t n) -> uint8_t {
        float norm = IQC_DENSITY_FLOOR + (1.0f - IQC_DENSITY_FLOOR) * log1pf((float)n) * inv_log_max;
        if (norm > 1.0f) {
            norm = 1.0f;
        }
        return (uint8_t)(norm * 255.0f);
    };
    if (c->density) {
        const uint32_t lut_top = (max_count < IQC_DENSITY_LUT_COUNTS) ? max_count + 1 : IQC_DENSITY_LUT_COUNTS;
        for (uint32_t n = 1; n < lut_top; n++) {
            lut[n] = density_index(n);
        }
        lut[0] = 0;
    }

    uint8_t* dst = pixels.data();
    for (size_t i = 0; i < counts.size(); i++, dst += 4) {
        const uint32_t n = counts[i];
        dst[3] = 255;
        if (n == 0) {
            dst[0] = dst[1] = dst[2] = 0;
        } else if (!c->density) {
            dst[0] = IQC_UNIFORM_R;
            dst[1] = IQC_UNIFORM_G;
            dst[2] = IQC_UNIFORM_B;
        } else {
            const uint8_t index = (n < IQC_DENSITY_LUT_COUNTS) ? lut[n] : density_index(n);
            dst[0] = c->palette[index][0];
            dst[1] = c->palette[index][1];
            dst[2] = c->palette[index][2];
        }
    }
}

// Grows every hit pixel into a round dot whose radius follows its count on the
// colour's log scale: max_radius pixels for the densest pixel, a single pixel
// for counts too small to round up to radius 1. Where dots overlap the larger
// count wins, so a dense dot always draws over a sparse one and the colours
// still say how dense each dot is. Reads counts, writes out; out takes the
// counts' size.
static void iqc_grow_dots(const std::vector<uint32_t>& counts, int w, int h, int max_radius,
                          std::vector<uint32_t>& out) {
    out = counts;

    uint32_t max_count = 0;
    for (uint32_t n : counts) {
        if (n > max_count) {
            max_count = n;
        }
    }
    if (max_count == 0 || max_radius <= 0) {
        return;
    }

    // A count of n gets radius round(max_radius * log1p(n) / log1p(max)), so
    // radius r starts at the count where that reaches r - 0.5.
    float starts_at[IQC_DOT_MAX_RADIUS + 1];
    const int radii = max_radius < IQC_DOT_MAX_RADIUS ? max_radius : IQC_DOT_MAX_RADIUS;
    const float log_max = log1pf((float)max_count);
    for (int r = 1; r <= radii; r++) {
        starts_at[r] = expm1f(((float)r - 0.5f) / (float)radii * log_max);
    }

    // Half-width of each disc's rows. r * r + r rather than r * r rounds the
    // small discs off without leaving them square or spiky.
    int half_width[IQC_DOT_MAX_RADIUS + 1][2 * IQC_DOT_MAX_RADIUS + 1];
    for (int r = 1; r <= radii; r++) {
        for (int dy = -r; dy <= r; dy++) {
            half_width[r][dy + r] = (int)floorf(sqrtf((float)(r * r + r - dy * dy)));
        }
    }

    for (int y = 0; y < h; y++) {
        const uint32_t* src = counts.data() + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            const uint32_t n = src[x];
            if (n == 0 || (float)n < starts_at[1]) {
                continue;
            }
            int r = 1;
            while (r < radii && (float)n >= starts_at[r + 1]) {
                r++;
            }
            for (int dy = -r; dy <= r; dy++) {
                const int yy = y + dy;
                if (yy < 0 || yy >= h) {
                    continue;
                }
                const int hw = half_width[r][dy + r];
                const int x0 = (x - hw < 0) ? 0 : x - hw;
                const int x1 = (x + hw > w - 1) ? w - 1 : x + hw;
                uint32_t* dst = out.data() + (size_t)yy * (size_t)w;
                for (int xx = x0; xx <= x1; xx++) {
                    if (dst[xx] < n) {
                        dst[xx] = n;
                    }
                }
            }
        }
    }
}

// Re-fits the full view after a pass. Only a real change of scale moves it -
// the data outgrowing the full view, shrinking to under half of it, a new
// stream, or a Reset - so the zoom, which is held relative to the full view,
// does not breathe with the frame-to-frame peak.
static void iqc_fit_extent(iq_constellation_t* c, float peak) {
    if (!(peak > 0.0f)) {
        return;
    }
    const float target = peak * IQC_EXTENT_MARGIN;
    if (c->refit || !c->extent_fitted || peak > c->extent || target < c->extent * 0.5f) {
        c->extent = target;
    }
    c->extent_fitted = 1;
    c->refit = 0;
}

// What the settings and the Radar view's pair say to plot. Outside the Radar
// view that is always the IQ of the stream on screen.
static iqc_input iqc_resolve_input(const iq_constellation_t* c,
                                   const fh_oran_memory_t* memory,
                                   const iq_constellation_radar_pair_t* radar) {
    iqc_input in;
    memset(&in, 0, sizeof(in));
    in.direction = IQ_CONSTELLATION_DL;
    if (radar == NULL) {
        in.memory = memory;
        in.missing = (memory == NULL) ? IQ_CONSTELLATION_MISSING_STREAM : IQ_CONSTELLATION_MISSING_NONE;
        return in;
    }

    in.radar_view = 1;
    in.config = radar->config;
    if (c->radar_data == IQ_CONSTELLATION_RADAR_CFR) {
        in.cfr = 1;
        in.memory = radar->dl;
        in.ul = radar->ul;
        in.missing = (radar->dl == NULL) ? IQ_CONSTELLATION_MISSING_DL
                   : (radar->ul == NULL) ? IQ_CONSTELLATION_MISSING_UL
                                         : IQ_CONSTELLATION_MISSING_NONE;
    } else if (c->radar_direction == IQ_CONSTELLATION_UL) {
        in.direction = IQ_CONSTELLATION_UL;
        in.memory = radar->ul;
        // The Radar view's rows are DL symbols, and DL symbol n is paired with
        // UL symbol n + lag; that is where its UL samples are.
        in.symbol_shift = radar->config.dl_ul_lag_symbols;
        in.missing = (radar->ul == NULL) ? IQ_CONSTELLATION_MISSING_UL : IQ_CONSTELLATION_MISSING_NONE;
    } else {
        in.memory = radar->dl;
        in.missing = (radar->dl == NULL) ? IQ_CONSTELLATION_MISSING_DL : IQ_CONSTELLATION_MISSING_NONE;
    }
    return in;
}

// Moves a zoomed area's symbols onto the ring the input reads, and narrows H
// to the resource elements the radar divides at all.
static void iqc_adapt_selection(iqc_selection* sel, const iqc_input& in) {
    if (in.symbol_shift != 0) {
        if (sel->time == VISUALIZER_SELECTION_TIME_SYMBOLS) {
            sel->symbol_first += in.symbol_shift;
            sel->symbol_last += in.symbol_shift;
        } else if (sel->time == VISUALIZER_SELECTION_TIME_PHASES && sel->period > 0) {
            int phase = (sel->phase_first + in.symbol_shift) % sel->period;
            if (phase < 0) {
                phase += sel->period;
            }
            sel->phase_first = phase;
        }
    }
    if (in.cfr) {
        if (sel->re_first < in.config.freq_start_re) {
            sel->re_first = in.config.freq_start_re;
        }
        if (sel->re_last > in.config.freq_end_re) {
            sel->re_last = in.config.freq_end_re;
        }
        if (sel->re_last < sel->re_first) {
            sel->re_last = sel->re_first;  // disjoint: H then yields nothing
        }
    }
}

// One pass over the selection, newest symbol first, binning every sample into
// the plot and into the overview map.
static void iqc_build(iq_constellation_t* c, const iqc_input& in, const iqc_selection& sel,
                      int plot_w, int plot_h) {
    c->plot_counts.assign((size_t)plot_w * (size_t)plot_h, 0);
    std::fill(c->map_counts.begin(), c->map_counts.end(), 0u);

    iq_constellation_stats_t& st = c->stats;
    memset(&st, 0, sizeof(st));
    st.missing = in.missing;
    st.radar_view = in.radar_view;
    st.showing_cfr = in.cfr;
    st.direction = in.direction;
    st.symbol_shift = in.symbol_shift;
    st.re_first = sel.re_first;
    st.re_last = sel.re_last;
    st.time = sel.time;
    st.symbol_first = sel.symbol_first;
    st.symbol_last = sel.symbol_last;
    st.phase_first = sel.phase_first;
    st.phase_count = sel.phase_count;
    st.period = sel.period;
    st.selection_from_zoom = sel.from_zoom;
    st.main_view_zoomed = sel.main_view_zoomed;
    st.main_view_valid = sel.main_view_valid;

    const fh_oran_memory_t* memory = in.memory;
    if (in.missing != IQ_CONSTELLATION_MISSING_NONE || memory == NULL || memory->symbols == NULL ||
        memory->symbol_highest < memory->symbol_lowest) {
        return;
    }
    st.has_memory = 1;

    // Sample to pixel, relative to the plot's top-left corner:
    //   u = 0.5 + I / 2A,  x = plot_w / 2 + (u - center_u) * px_per_norm
    //   v = 0.5 - Q / 2A,  y = plot_h / 2 + (v - center_v) * px_per_norm
    // folded into one multiply-add per axis.
    const float extent = c->extent > 1e-12f ? c->extent : 1e-12f;
    const float half_inv_extent = 0.5f / extent;
    const float px_per_norm = iqc_px_per_norm(c, plot_w, plot_h);
    const float ax = px_per_norm * half_inv_extent;
    const float bx = (float)plot_w * 0.5f + (0.5f - c->center_u) * px_per_norm;
    const float ay = -px_per_norm * half_inv_extent;
    const float by = (float)plot_h * 0.5f + (0.5f - c->center_v) * px_per_norm;
    const float fw = (float)plot_w;
    const float fh = (float)plot_h;
    const float map_scale = (float)IQC_MAP_PX * half_inv_extent;
    const float map_offset = (float)IQC_MAP_PX * 0.5f;
    const float map_px = (float)IQC_MAP_PX;
    uint32_t* const plot = c->plot_counts.data();
    uint32_t* const map = c->map_counts.data();

    int symbol_hi = memory->symbol_highest;
    int symbol_lo = memory->symbol_lowest;
    if (sel.time == VISUALIZER_SELECTION_TIME_SYMBOLS) {
        if (sel.symbol_last < symbol_hi) {
            symbol_hi = sel.symbol_last;
        }
        if (sel.symbol_first > symbol_lo) {
            symbol_lo = sel.symbol_first;
        }
    }
    if (symbol_lo == INT_MIN) {
        symbol_lo = INT_MIN + 1;  // the countdown below must be able to step past it
    }
    const int row_len = sel.re_last - sel.re_first + 1;
    const int skip_empty = c->skip_empty;

    // Thins the overview map's symbols to about IQC_MAP_SAMPLE_TARGET samples,
    // estimated from the rows the pass can visit: the ring's span narrowed by
    // the selection, then by the sample budget.
    long long candidate_rows = (symbol_hi >= symbol_lo) ? (long long)symbol_hi - (long long)symbol_lo + 1 : 0;
    if (sel.time == VISUALIZER_SELECTION_TIME_PHASES && sel.period > 0) {
        candidate_rows = candidate_rows * sel.phase_count / sel.period + 1;
    }
    if (candidate_rows > IQC_SAMPLE_BUDGET / row_len + 1) {
        candidate_rows = IQC_SAMPLE_BUDGET / row_len + 1;
    }
    const long long map_estimate = candidate_rows * row_len;
    const int map_stride = (map_estimate > IQC_MAP_SAMPLE_TARGET)
        ? (int)((map_estimate + IQC_MAP_SAMPLE_TARGET - 1) / IQC_MAP_SAMPLE_TARGET)
        : 1;

    long long scanned = 0;
    float peak = 0.0f;
    for (int symbol = symbol_hi; symbol >= symbol_lo; symbol--) {
        if (sel.time == VISUALIZER_SELECTION_TIME_PHASES) {
            int phase = symbol % sel.period;
            if (phase < 0) {
                phase += sel.period;
            }
            int offset = phase - sel.phase_first;
            if (offset < 0) {
                offset += sel.period;
            }
            if (offset >= sel.phase_count) {
                continue;
            }
        }

        // A ring row as it stands, or for H the symbol divided out against its
        // UL partner. Either way a run of values to bin.
        const fh_complex_t* values = NULL;
        int value_count = 0;
        if (in.cfr) {
            const int written = fh_radar_cfr_symbol(&in.config, memory, in.ul, symbol, sel.re_first, sel.re_last,
                                                    c->cfr_row.data());
            if (written < 0) {
                continue;  // did not pair, for the same reasons the radar skips it
            }
            values = c->cfr_row.data();
            value_count = written;
        } else {
            size_t idx;
            if (!fh_oran_memory_row_for_symbol(memory, symbol, &idx)) {
                continue;  // evicted, or a gap the stream never filled (TDD's other direction)
            }
            values = memory->symbols[idx] + sel.re_first;
            value_count = row_len;
        }
        st.symbols_selected++;
        // Keeps counting past the budget, so the panel can say how much was left out.
        if (st.budget_hit) {
            continue;
        }
        if (st.symbols_plotted > 0 && scanned + row_len > IQC_SAMPLE_BUDGET) {
            st.budget_hit = 1;
            continue;
        }
        scanned += row_len;
        const bool to_map = (st.symbols_plotted % map_stride) == 0;
        st.symbols_plotted++;

        for (int k = 0; k < value_count; k++) {
            const float i_val = values[k].real();
            const float q_val = values[k].imag();
            if (skip_empty && i_val == 0.0f && q_val == 0.0f) {
                continue;
            }
            st.samples_plotted++;

            const float ai = fabsf(i_val);
            const float aq = fabsf(q_val);
            const float m = ai > aq ? ai : aq;
            if (m > peak && m < 1e30f) {
                peak = m;
            }

            // Tested in float before the cast, so NaN, infinities and samples
            // far off screen are dropped rather than wrapped into the array.
            const float fx = ax * i_val + bx;
            const float fy = ay * q_val + by;
            if (fx >= 0.0f && fx < fw && fy >= 0.0f && fy < fh) {
                plot[(size_t)(int)fy * (size_t)plot_w + (size_t)(int)fx]++;
            }
            if (to_map) {
                const float mx = map_scale * i_val + map_offset;
                const float my = map_offset - map_scale * q_val;
                if (mx >= 0.0f && mx < map_px && my >= 0.0f && my < map_px) {
                    map[(size_t)(int)my * IQC_MAP_PX + (size_t)(int)mx]++;
                }
            }
        }
    }

    iqc_fit_extent(c, peak);
}

void iq_constellation_update(iq_constellation_t* c,
                             const fh_oran_memory_t* memory,
                             const visualizer_zoom_selection_t* selection,
                             const iq_constellation_radar_pair_t* radar) {
    if (c == NULL) {
        return;
    }

    const iqc_input in = iqc_resolve_input(c, memory, radar);

    // A different stream, or H in place of IQ, is a different scale, so the
    // full view is fitted again; the zoom is kept, being relative to it.
    iqc_identity identity;
    memset(&identity, 0, sizeof(identity));
    identity.memory = in.memory;
    identity.ul = in.ul;
    identity.cfr = in.cfr;
    identity.division = in.cfr ? (int)in.config.division : 0;
    if (memcmp(&identity, &c->last_identity, sizeof(identity)) != 0) {
        c->refit = 1;
        c->last_identity = identity;
    }

    int plot_x = 0, plot_y = 0, plot_w = 1, plot_h = 1;
    iqc_plot_rect(c, &plot_x, &plot_y, &plot_w, &plot_h);
    iqc_selection sel = iqc_effective_selection(c, selection);
    iqc_adapt_selection(&sel, in);

    iqc_view_key view;
    memset(&view, 0, sizeof(view));
    view.memory = in.memory;
    view.ul = in.ul;
    view.cfr = in.cfr;
    if (in.cfr) {
        view.lag = in.config.dl_ul_lag_symbols;
        view.division = (int)in.config.division;
        view.freq_start_re = in.config.freq_start_re;
        view.freq_end_re = in.config.freq_end_re;
    }
    view.plot_w = plot_w;
    view.plot_h = plot_h;
    view.center_u = c->center_u;
    view.center_v = c->center_v;
    view.span = c->span;
    view.extent = c->extent;
    view.refit = c->refit;
    view.source = (int)c->source;
    view.skip_empty = c->skip_empty;
    view.density = c->density;
    view.dot_size = c->dot_size;
    view.re_first = sel.re_first;
    view.re_last = sel.re_last;
    view.time = (int)sel.time;
    view.phase_first = sel.phase_first;
    view.phase_count = sel.phase_count;
    view.period = sel.period;

    iqc_data_key data;
    memset(&data, 0, sizeof(data));
    data.symbol_highest = in.memory != NULL ? in.memory->symbol_highest : INT_MIN;
    data.ul_symbol_highest = in.ul != NULL ? in.ul->symbol_highest : INT_MIN;
    data.symbol_first = sel.symbol_first;
    data.symbol_last = sel.symbol_last;

    const double now = glfwGetTime();
    const int view_changed = !c->have_built || memcmp(&view, &c->last_view, sizeof(view)) != 0;
    const int data_changed = memcmp(&data, &c->last_data, sizeof(data)) != 0;
    double data_interval = c->last_build_cost * IQC_DATA_REBUILD_COST_FACTOR;
    if (data_interval < IQC_DATA_REBUILD_INTERVAL_S) {
        data_interval = IQC_DATA_REBUILD_INTERVAL_S;
    }
    const int data_due = data_changed && (now - c->last_build_time) >= data_interval;

    if (view_changed || data_due) {
        iqc_build(c, in, sel, plot_w, plot_h);
        if (c->dot_size) {
            iqc_grow_dots(c->plot_counts, plot_w, plot_h, IQC_DOT_MAX_RADIUS, c->dot_counts);
            iqc_colourize(c, c->dot_counts, c->plot_pixels);
            iqc_grow_dots(c->map_counts, IQC_MAP_PX, IQC_MAP_PX, IQC_MAP_DOT_MAX_RADIUS, c->dot_counts);
            iqc_colourize(c, c->dot_counts, c->map_pixels);
        } else {
            iqc_colourize(c, c->plot_counts, c->plot_pixels);
            iqc_colourize(c, c->map_counts, c->map_pixels);
        }

        glBindTexture(GL_TEXTURE_2D, c->plot_texture);
        if (plot_w != c->plot_texture_w || plot_h != c->plot_texture_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, plot_w, plot_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         c->plot_pixels.data());
            c->plot_texture_w = plot_w;
            c->plot_texture_h = plot_h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, plot_w, plot_h, GL_RGBA, GL_UNSIGNED_BYTE,
                            c->plot_pixels.data());
        }
        glBindTexture(GL_TEXTURE_2D, c->map_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, IQC_MAP_PX, IQC_MAP_PX, GL_RGBA, GL_UNSIGNED_BYTE,
                        c->map_pixels.data());

        // Keyed on what the picture was built with, so a refit made by this
        // very pass reads as a change and the next update redraws to it.
        c->last_view = view;
        c->last_data = data;
        c->last_build_time = now;
        c->last_build_cost = glfwGetTime() - now;
        c->have_built = 1;
    }

    if (c->plot_texture_w <= 0 || c->plot_texture_h <= 0) {
        return;
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, c->plot_texture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    const float x0 = (float)plot_x;
    const float y0 = (float)plot_y;
    const float x1 = x0 + (float)c->plot_texture_w;
    const float y1 = y0 + (float)c->plot_texture_h;
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x0, y0);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x1, y0);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x1, y1);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x0, y1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

void iq_constellation_set_source(iq_constellation_t* c, iq_constellation_source_t source) {
    if (c != NULL) {
        c->source = (source == IQ_CONSTELLATION_SOURCE_ZOOMED) ? IQ_CONSTELLATION_SOURCE_ZOOMED
                                                                : IQ_CONSTELLATION_SOURCE_ALL;
    }
}

iq_constellation_source_t iq_constellation_get_source(const iq_constellation_t* c) {
    return c ? c->source : IQ_CONSTELLATION_SOURCE_ALL;
}

void iq_constellation_set_skip_empty(iq_constellation_t* c, int enabled) {
    if (c != NULL) {
        c->skip_empty = enabled ? 1 : 0;
    }
}

int iq_constellation_get_skip_empty(const iq_constellation_t* c) {
    return c ? c->skip_empty : 0;
}

void iq_constellation_set_density(iq_constellation_t* c, int enabled) {
    if (c != NULL) {
        c->density = enabled ? 1 : 0;
    }
}

int iq_constellation_get_density(const iq_constellation_t* c) {
    return c ? c->density : 0;
}

void iq_constellation_set_dot_size(iq_constellation_t* c, int enabled) {
    if (c != NULL) {
        c->dot_size = enabled ? 1 : 0;
    }
}

int iq_constellation_get_dot_size(const iq_constellation_t* c) {
    return c ? c->dot_size : 0;
}

void iq_constellation_set_radar_data(iq_constellation_t* c, iq_constellation_radar_data_t data) {
    if (c != NULL) {
        c->radar_data = (data == IQ_CONSTELLATION_RADAR_IQ) ? IQ_CONSTELLATION_RADAR_IQ : IQ_CONSTELLATION_RADAR_CFR;
    }
}

iq_constellation_radar_data_t iq_constellation_get_radar_data(const iq_constellation_t* c) {
    return c ? c->radar_data : IQ_CONSTELLATION_RADAR_CFR;
}

void iq_constellation_set_radar_direction(iq_constellation_t* c, iq_constellation_direction_t direction) {
    if (c != NULL) {
        c->radar_direction = (direction == IQ_CONSTELLATION_UL) ? IQ_CONSTELLATION_UL : IQ_CONSTELLATION_DL;
    }
}

iq_constellation_direction_t iq_constellation_get_radar_direction(const iq_constellation_t* c) {
    return c ? c->radar_direction : IQ_CONSTELLATION_UL;
}

void iq_constellation_get_zoom(const iq_constellation_t* c, float* out_center_u, float* out_center_v, float* out_span) {
    if (out_center_u) { *out_center_u = c ? c->center_u : 0.5f; }
    if (out_center_v) { *out_center_v = c ? c->center_v : 0.5f; }
    if (out_span) { *out_span = c ? c->span : 1.0f; }
}

int iq_constellation_zoom_is_active(const iq_constellation_t* c) {
    return (c != NULL && c->span < 0.9999f) ? 1 : 0;
}

void iq_constellation_reset_zoom(iq_constellation_t* c) {
    if (c == NULL) {
        return;
    }
    c->center_u = 0.5f;
    c->center_v = 0.5f;
    c->span = 1.0f;
    c->refit = 1;
}

void iq_constellation_set_center(iq_constellation_t* c, float center_u, float center_v) {
    if (c == NULL) {
        return;
    }
    c->center_u = center_u;
    c->center_v = center_v;
    iqc_normalize_zoom(c);
}

void iq_constellation_zoom_by(iq_constellation_t* c, float factor, float focus_px_x, float focus_px_y) {
    if (c == NULL) {
        return;
    }
    factor = iqc_guard(factor, 1.0f);
    if (!(factor > 0.0f) || factor == 1.0f) {
        return;
    }
    int plot_x = 0, plot_y = 0, plot_w = 1, plot_h = 1;
    iqc_plot_rect(c, &plot_x, &plot_y, &plot_w, &plot_h);

    // Offset of the focus from the plot centre, which is where the window's
    // centre is drawn; the data under it stays put across the change.
    const float rel_x = iqc_guard(focus_px_x, 0.0f) - ((float)plot_x + (float)plot_w * 0.5f);
    const float rel_y = iqc_guard(focus_px_y, 0.0f) - ((float)plot_y + (float)plot_h * 0.5f);
    const float before = iqc_px_per_norm(c, plot_w, plot_h);
    const float anchor_u = c->center_u + rel_x / before;
    const float anchor_v = c->center_v + rel_y / before;

    c->span /= factor;
    if (c->span < IQC_MIN_SPAN) {
        c->span = IQC_MIN_SPAN;
    }
    if (c->span > 1.0f) {
        c->span = 1.0f;
    }
    const float after = iqc_px_per_norm(c, plot_w, plot_h);
    c->center_u = anchor_u - rel_x / after;
    c->center_v = anchor_v - rel_y / after;
    iqc_normalize_zoom(c);
}

void iq_constellation_pan_by(iq_constellation_t* c, float u_fraction, float v_fraction) {
    if (c == NULL) {
        return;
    }
    c->center_u += iqc_guard(u_fraction, 0.0f) * c->span;
    c->center_v += iqc_guard(v_fraction, 0.0f) * c->span;
    iqc_normalize_zoom(c);
}

void iq_constellation_pan_px(iq_constellation_t* c, float dx_px, float dy_px) {
    if (c == NULL) {
        return;
    }
    int plot_x = 0, plot_y = 0, plot_w = 1, plot_h = 1;
    iqc_plot_rect(c, &plot_x, &plot_y, &plot_w, &plot_h);
    const float px_per_norm = iqc_px_per_norm(c, plot_w, plot_h);
    // Negated: dragging right brings data from the left into view.
    c->center_u -= iqc_guard(dx_px, 0.0f) / px_per_norm;
    c->center_v -= iqc_guard(dy_px, 0.0f) / px_per_norm;
    iqc_normalize_zoom(c);
}

void iq_constellation_get_view(const iq_constellation_t* c, iq_constellation_view_t* out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (c == NULL) {
        return;
    }
    int plot_x = 0, plot_y = 0, plot_w = 1, plot_h = 1;
    iqc_plot_rect(c, &plot_x, &plot_y, &plot_w, &plot_h);
    const float px_per_norm = iqc_px_per_norm(c, plot_w, plot_h);
    const float two_a = 2.0f * c->extent;

    out->framebuffer_width = c->fb_w;
    out->framebuffer_height = c->fb_h;
    out->plot_x = (float)plot_x;
    out->plot_y = (float)plot_y;
    out->plot_w = (float)plot_w;
    out->plot_h = (float)plot_h;
    const float u_left = c->center_u - (float)plot_w * 0.5f / px_per_norm;
    const float u_right = c->center_u + (float)plot_w * 0.5f / px_per_norm;
    const float v_top = c->center_v - (float)plot_h * 0.5f / px_per_norm;
    const float v_bottom = c->center_v + (float)plot_h * 0.5f / px_per_norm;
    out->i_min = (u_left - 0.5f) * two_a;
    out->i_max = (u_right - 0.5f) * two_a;
    out->q_max = (0.5f - v_top) * two_a;
    out->q_min = (0.5f - v_bottom) * two_a;
    out->extent = c->extent;
    out->extent_fitted = c->extent_fitted;
}

void iq_constellation_screen_to_iq(const iq_constellation_t* c, float px, float py, float* out_i, float* out_q) {
    float i_val = 0.0f;
    float q_val = 0.0f;
    if (c != NULL) {
        int plot_x = 0, plot_y = 0, plot_w = 1, plot_h = 1;
        iqc_plot_rect(c, &plot_x, &plot_y, &plot_w, &plot_h);
        const float px_per_norm = iqc_px_per_norm(c, plot_w, plot_h);
        const float u = c->center_u + (px - ((float)plot_x + (float)plot_w * 0.5f)) / px_per_norm;
        const float v = c->center_v + (py - ((float)plot_y + (float)plot_h * 0.5f)) / px_per_norm;
        i_val = (u - 0.5f) * 2.0f * c->extent;
        q_val = (0.5f - v) * 2.0f * c->extent;
    }
    if (out_i) { *out_i = i_val; }
    if (out_q) { *out_q = q_val; }
}

void iq_constellation_get_stats(const iq_constellation_t* c, iq_constellation_stats_t* out) {
    if (out == NULL) {
        return;
    }
    if (c == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = c->stats;
}

unsigned int iq_constellation_map_texture(const iq_constellation_t* c) {
    return c ? (unsigned int)c->map_texture : 0u;
}

int iq_constellation_map_size(void) {
    return IQC_MAP_PX;
}
