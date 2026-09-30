#include "imgui_visualizer_interface.h"
#include "capture/c_pcap_demo_stream.h"
#include "core/c_oran_memory.h"
#include "core/c_radar.h"
#include "pipeline/c_packet_queue.h"
#include "visualization/opengl_visualizer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl2.h>
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

struct imgui_visualizer_state {
    bool initialized;
    bool selection_initialized;
    int selected_eaxc_id_index;
    uint8_t selected_data_direction;
    uint8_t prev_data_direction;

    // The radar's DL/UL pair, which may take its two directions from different
    // eAxCs when the receiving antenna is not the transmitting one. Until a
    // pair is picked in radar mode it follows the selected eAxC; once picked it
    // stays, so a look at the waterfall does not undo it. Held as eAxC ids
    // rather than table rows, since rows are rebuilt every frame.
    bool radar_pair_explicit;
    uint16_t radar_dl_eaxc;
    uint16_t radar_ul_eaxc;

    // "Only decode selected": the capture keeps only the channels on screen,
    // on top of its startup filter. See publish_decode_selection.
    bool only_decode_selected;

    // Freeze: the memories stop filling while the capture carries on, so every
    // channel and view shows one moment. Owned here; main applies it to the
    // decode workers - see imgui_visualizer_get_frozen.
    bool frozen;

    int selected_marker;

    // Value at cursor. The two destinations are independent so the reading can
    // go next to the pointer, into the Markers window, or both at once.
    bool cursor_value_enabled;
    bool cursor_value_at_cursor;
    bool cursor_value_in_window;

    // Frame timing. The wire frame_id is only 8 bits, so it wraps every 256
    // frames; the memory pairs it with a wraparound count. Accumulating the
    // advance between samples here keeps a total that survives both that
    // wrap and the counter being reset (which pcap replay does on each loop).
    bool frame_tracking_initialized;
    const void* frame_tracking_memory;
    uint64_t last_absolute_frame;
    uint64_t frames_since_start;

    // Packet rate for the Data Query section, measured from the queue's
    // running total over half-second spans so the number holds still long
    // enough to read. Shows what a pcap speed setting actually achieves.
    bool packet_rate_initialized;
    double packet_rate_last_time;
    size_t packet_rate_last_total;
    double packet_rate_pps;
};

namespace {

// A collapsible section in the style of Live Statistics. open_at_start only
// matters the first time the section is drawn: a section whose feature is in
// use then starts open, the rest start folded, and from there on it stays
// however the user leaves it. The open state is not saved to imgui.ini, so
// every launch starts from this default again.
bool section_header(const char* label, bool open_at_start) {
    return ImGui::CollapsingHeader(label, open_at_start ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

// Tick marks get longer with importance, so frame boundaries read as major
// divisions and individual symbols as fine subdivisions.
float axis_tick_length(int level) {
    switch (level) {
        case 3: return 12.0f;
        case 2: return 9.0f;
        case 1: return 6.0f;
        default: return 3.0f;
    }
}

// Extra outward offset applied to frame labels, keeping them in their own
// column clear of the finer subframe/slot divisions.
const float AXIS_FRAME_LABEL_GAP = 7.0f;

// Right edge shared by everything in the frame-label column, so the running
// axis and the corner readout stay aligned with each other.
float axis_frame_label_right_edge(float plot_x) {
    return plot_x - axis_tick_length(3) - 4.0f - AXIS_FRAME_LABEL_GAP;
}

ImU32 axis_tick_color(int level) {
    return (level >= 2) ? IM_COL32(220, 220, 220, 255) : IM_COL32(150, 150, 150, 255);
}

void draw_plot_axes(opengl_visualizer_t* visualizer, const fh_oran_memory_t* memory) {
    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 0.0f, plot_h = 0.0f;
    visualizer_get_plot_rect(visualizer, &plot_x, &plot_y, &plot_w, &plot_h);
    if (plot_w <= 1.0f || plot_h <= 1.0f) {
        return;
    }

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 axis_color = IM_COL32(220, 220, 220, 255);
    const ImU32 label_color = IM_COL32(230, 230, 230, 255);
    const float plot_bottom = plot_y + plot_h;

    draw->AddLine(ImVec2(plot_x, plot_y), ImVec2(plot_x, plot_bottom), axis_color);
    draw->AddLine(ImVec2(plot_x, plot_bottom), ImVec2(plot_x + plot_w, plot_bottom), axis_color);

    visualizer_axis_tick_t ticks[VISUALIZER_MAX_AXIS_TICKS];
    visualizer_axis_tick_t anchor_tick;
    bool have_anchor = false;

    // Vertical axis: ticks step left out of the plot, labels right-aligned
    // against them so they grow into the margin rather than toward the data.
    const int y_count = visualizer_get_y_ticks(visualizer, memory, ticks, VISUALIZER_MAX_AXIS_TICKS);
    for (int i = 0; i < y_count; i++) {
        const visualizer_axis_tick_t& tick = ticks[i];
        if (tick.position_px < plot_y - 0.5f || tick.position_px > plot_bottom + 0.5f) {
            continue;
        }
        const float len = axis_tick_length(tick.level);
        draw->AddLine(ImVec2(plot_x - len, tick.position_px), ImVec2(plot_x, tick.position_px),
                      axis_tick_color(tick.level));
        if (!tick.has_label || tick.label[0] == '\0') {
            continue;
        }

        // The newest-symbol readout is several lines tall and would crowd the
        // regular labels where it sits, so it is drawn afterwards down in the
        // corner beneath the horizontal axis instead.
        if (tick.is_anchor) {
            anchor_tick = tick;
            have_anchor = true;
            continue;
        }

        // Labels stack downward, each line right-aligned against the tick so
        // the block grows into the margin rather than toward the data.
        const char* lines[3] = { tick.label, tick.label2, tick.label3 };
        int line_count = 0;
        for (int l = 0; l < 3; l++) {
            if (lines[l][0] != '\0') {
                line_count = l + 1;
            }
        }
        if (line_count == 0) {
            continue;
        }

        const float line_h = ImGui::GetTextLineHeight();
        const float block_h = line_h * (float)line_count;
        // Keep a little clearance below the axis title at the very top.
        const float top_limit = plot_y + 6.0f;
        float top_y = tick.position_px - block_h * 0.5f;
        if (top_y < top_limit) {
            top_y = top_limit;
        }
        if (top_y + block_h > plot_bottom) {
            top_y = plot_bottom - block_h;
        }

        // Frame labels sit a little further out than the finer divisions, so
        // the two groups read as separate columns.
        const float group_gap = (tick.level >= 3) ? AXIS_FRAME_LABEL_GAP : 0.0f;
        for (int l = 0; l < line_count; l++) {
            if (lines[l][0] == '\0') {
                continue;
            }
            const ImVec2 size = ImGui::CalcTextSize(lines[l]);
            float text_x = plot_x - len - 4.0f - group_gap - size.x;
            if (text_x < 2.0f) {
                text_x = 2.0f;
            }
            draw->AddText(ImVec2(text_x, top_y + line_h * (float)l), label_color, lines[l]);
        }
    }

    // Anchor readout, tucked into the corner below the horizontal axis and to
    // the left of its origin label, where it has room without competing with
    // either axis.
    if (have_anchor) {
        const char* lines[3] = { anchor_tick.label, anchor_tick.label2, anchor_tick.label3 };
        int line_count = 0;
        for (int l = 0; l < 3; l++) {
            if (lines[l][0] != '\0') {
                line_count = l + 1;
            }
        }
        if (line_count > 0) {
            int fb_width = 0;
            int fb_height = 0;
            visualizer_get_framebuffer_size(visualizer, &fb_width, &fb_height);

            const float line_h = ImGui::GetTextLineHeight();
            float top_y = plot_bottom + 4.0f;
            const float block_h = line_h * (float)line_count;
            if (fb_height > 0 && top_y + block_h > (float)fb_height - 2.0f) {
                top_y = (float)fb_height - 2.0f - block_h;
            }

            // Share the frame-label column so this lines up with the frame
            // numbers running up the axis above it.
            const float right_edge = axis_frame_label_right_edge(plot_x);
            const ImU32 anchor_color = IM_COL32(255, 240, 150, 255);
            for (int l = 0; l < line_count; l++) {
                if (lines[l][0] == '\0') {
                    continue;
                }
                const ImVec2 size = ImGui::CalcTextSize(lines[l]);
                float text_x = right_edge - size.x;
                if (text_x < 2.0f) {
                    text_x = 2.0f;
                }
                draw->AddText(ImVec2(text_x, top_y + line_h * (float)l), anchor_color, lines[l]);
            }
        }
    }

    // Horizontal axis: two stacked label rows, PRB index above absolute RE index.
    const int x_count = visualizer_get_x_ticks(visualizer, ticks, VISUALIZER_MAX_AXIS_TICKS);
    for (int i = 0; i < x_count; i++) {
        const visualizer_axis_tick_t& tick = ticks[i];
        if (tick.position_px < plot_x - 0.5f || tick.position_px > plot_x + plot_w + 0.5f) {
            continue;
        }
        const float len = axis_tick_length(tick.level);
        draw->AddLine(ImVec2(tick.position_px, plot_bottom), ImVec2(tick.position_px, plot_bottom + len),
                      axis_tick_color(tick.level));
        if (!tick.has_label) {
            continue;
        }
        if (tick.label[0] != '\0') {
            const ImVec2 size = ImGui::CalcTextSize(tick.label);
            draw->AddText(ImVec2(tick.position_px - size.x * 0.5f, plot_bottom + len + 2.0f),
                          label_color, tick.label);
        }
        if (tick.label2[0] != '\0') {
            const ImVec2 size = ImGui::CalcTextSize(tick.label2);
            draw->AddText(ImVec2(tick.position_px - size.x * 0.5f, plot_bottom + len + 18.0f),
                          IM_COL32(170, 170, 170, 255), tick.label2);
        }
    }

    const char* y_title = nullptr;
    const char* x_title = nullptr;
    visualizer_get_axis_titles(visualizer, &y_title, &x_title);
    if (y_title != nullptr) {
        draw->AddText(ImVec2(4.0f, plot_y - ImGui::GetTextLineHeight() - 2.0f), label_color, y_title);
    }
    if (x_title != nullptr) {
        const ImVec2 size = ImGui::CalcTextSize(x_title);
        draw->AddText(ImVec2(plot_x + plot_w - size.x, plot_bottom + 36.0f), label_color, x_title);
    }
}

// Five-pointed star, drawn as a dark halo underneath a coloured outline so it
// stays visible against both ends of the heatmap gradient.
void draw_marker_star(ImDrawList* draw, ImVec2 centre, float radius, ImU32 color) {
    ImVec2 points[10];
    for (int i = 0; i < 10; i++) {
        const float pi = 3.14159265358979323846f;
        const float angle = -pi * 0.5f + (float)i * (pi / 5.0f);
        const float r = (i % 2 == 0) ? radius : radius * 0.42f;
        points[i] = ImVec2(centre.x + cosf(angle) * r, centre.y + sinf(angle) * r);
    }
    draw->AddPolyline(points, 10, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 3.5f);
    draw->AddPolyline(points, 10, color, ImDrawFlags_Closed, 1.8f);
}

void draw_markers(opengl_visualizer_t* visualizer) {
    const int count = visualizer_get_marker_count(visualizer);
    if (count <= 0) {
        return;
    }

    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 0.0f, plot_h = 0.0f;
    visualizer_get_plot_rect(visualizer, &plot_x, &plot_y, &plot_w, &plot_h);
    if (plot_w <= 1.0f || plot_h <= 1.0f) {
        return;
    }
    const float plot_right = plot_x + plot_w;
    const float plot_bottom = plot_y + plot_h;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    for (int i = 0; i < count; i++) {
        visualizer_marker_info_t marker;
        if (!visualizer_get_marker(visualizer, i, &marker) || !marker.on_screen) {
            continue;
        }

        const ImU32 color = IM_COL32(marker.color[0], marker.color[1], marker.color[2], 255);
        const ImVec2 centre(marker.screen_x, marker.screen_y);
        draw_marker_star(draw, centre, 7.0f, color);

        char line1[64];
        char line2[64];
        if (marker.has_value) {
            snprintf(line1, sizeof(line1), "#%d  %.1f", i + 1, (double)marker.magnitude);
            snprintf(line2, sizeof(line2), "%.1f%% base", (double)marker.percent_of_baseline);
        } else {
            snprintf(line1, sizeof(line1), "#%d", i + 1);
            snprintf(line2, sizeof(line2), "no data");
        }

        const ImVec2 size1 = ImGui::CalcTextSize(line1);
        const ImVec2 size2 = ImGui::CalcTextSize(line2);
        const float pad = 4.0f;
        const float box_w = (size1.x > size2.x ? size1.x : size2.x) + pad * 2.0f;
        const float box_h = size1.y + size2.y + pad * 2.0f;

        // Prefer the right of the star, flip to the left when that would run
        // past the plot, then clamp so the box can never reach the axes.
        float box_x = centre.x + 12.0f;
        if (box_x + box_w > plot_right) {
            box_x = centre.x - 12.0f - box_w;
        }
        if (box_x < plot_x) {
            box_x = plot_x;
        }
        if (box_x + box_w > plot_right) {
            box_x = plot_right - box_w;
        }

        float box_y = centre.y - box_h * 0.5f;
        if (box_y < plot_y) {
            box_y = plot_y;
        }
        if (box_y + box_h > plot_bottom) {
            box_y = plot_bottom - box_h;
        }

        const ImVec2 box_min(box_x, box_y);
        const ImVec2 box_max(box_x + box_w, box_y + box_h);
        draw->AddRectFilled(box_min, box_max, IM_COL32(255, 255, 255, 240), 3.0f);
        draw->AddRect(box_min, box_max, color, 3.0f, 0, 1.5f);
        draw->AddText(ImVec2(box_x + pad, box_y + pad), IM_COL32(0, 0, 0, 255), line1);
        draw->AddText(ImVec2(box_x + pad, box_y + pad + size1.y), IM_COL32(0, 0, 0, 255), line2);
    }
}

// --- Zoom ---
// The zoom window is held in the visualizer as normalized fractions; this layer
// converts to and from the units an operator actually thinks in (resource
// elements, symbol rows, percent of full scale) and drives it from the mouse
// and keyboard.

// One wheel notch. Compounding keeps each notch the same proportional step
// whatever the current depth.
const float ZOOM_WHEEL_STEP = 1.25f;

// Fraction of a screenful per arrow keypress.
const float ZOOM_PAN_STEP = 0.10f;

struct zoom_window_t {
    float x_first;   // leading edge, in data units
    float x_extent;
    float y_first;
    float y_extent;
};

// Reads the current window out in the units of the given axis extents.
zoom_window_t zoom_window_in_units(opengl_visualizer_t* visualizer, float x_total, float y_total) {
    float xc = 0.5f, xs = 1.0f, yc = 0.5f, ys = 1.0f;
    visualizer_get_zoom(visualizer, &xc, &xs, &yc, &ys);
    zoom_window_t w;
    w.x_extent = xs * x_total;
    w.x_first = (xc - xs * 0.5f) * x_total;
    w.y_extent = ys * y_total;
    w.y_first = (yc - ys * 0.5f) * y_total;
    return w;
}

// Applies a window given in data units, letting the visualizer do the clamping.
void zoom_apply_units(opengl_visualizer_t* visualizer,
                      const zoom_window_t& w,
                      float x_total,
                      float y_total) {
    const float xs = (x_total > 0.0f) ? w.x_extent / x_total : 1.0f;
    const float ys = (y_total > 0.0f) ? w.y_extent / y_total : 1.0f;
    const float xc = (x_total > 0.0f) ? (w.x_first / x_total) + xs * 0.5f : 0.5f;
    const float yc = (y_total > 0.0f) ? (w.y_first / y_total) + ys * 0.5f : 0.5f;
    visualizer_set_zoom(visualizer, xc, xs, yc, ys);
}

// Scroll wheel zooms about the cursor, left-drag and arrow keys pan. All are
// ignored while ImGui owns the input, so scrolling over a settings window or
// typing in one of its fields never moves the plot underneath.
void handle_zoom_input(opengl_visualizer_t* visualizer) {
    if (visualizer == nullptr) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 0.0f, plot_h = 0.0f;
    visualizer_get_plot_rect(visualizer, &plot_x, &plot_y, &plot_w, &plot_h);

    // Left-drag pans, grabbing the data under the cursor and carrying it along.
    // Gated on where the drag *started*, not where the cursor is now, so a drag
    // that runs out over a settings window or off the plot keeps working
    // instead of stopping dead mid-gesture.
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && plot_w > 1.0f && plot_h > 1.0f) {
        const ImVec2 start = io.MouseClickedPos[ImGuiMouseButton_Left];
        const bool started_on_plot = start.x >= plot_x && start.x <= plot_x + plot_w &&
                                     start.y >= plot_y && start.y <= plot_y + plot_h;
        if (started_on_plot && !ImGui::IsAnyItemActive()) {
            // Negated: dragging right shows data further left, so the window
            // moves the opposite way to the cursor.
            visualizer_pan_by(visualizer, -io.MouseDelta.x / plot_w, -io.MouseDelta.y / plot_h);
        }
    }

    if (!io.WantCaptureMouse && io.MouseWheel != 0.0f && plot_w > 1.0f && plot_h > 1.0f) {
        const float focus_x = (io.MousePos.x - plot_x) / plot_w;
        const float focus_y = (io.MousePos.y - plot_y) / plot_h;
        // Only when the cursor is actually over the data, so a wheel event
        // aimed at nothing in particular doesn't move the view.
        if (focus_x >= 0.0f && focus_x <= 1.0f && focus_y >= 0.0f && focus_y <= 1.0f) {
            const float factor = powf(ZOOM_WHEEL_STEP, io.MouseWheel);
            // Shift or Ctrl restricts the zoom to one axis, for stretching a
            // frequency slice or a time slice on its own.
            const bool x_only = io.KeyShift;
            const bool y_only = io.KeyCtrl;
            visualizer_zoom_by(visualizer,
                               y_only ? 1.0f : factor,
                               x_only ? 1.0f : factor,
                               focus_x, focus_y);
        }
    }

    if (!io.WantCaptureKeyboard) {
        float pan_x = 0.0f;
        float pan_y = 0.0f;
        // Repeat enabled so a held arrow key keeps panning.
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) { pan_x -= ZOOM_PAN_STEP; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) { pan_x += ZOOM_PAN_STEP; }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) { pan_y -= ZOOM_PAN_STEP; }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) { pan_y += ZOOM_PAN_STEP; }
        if (pan_x != 0.0f || pan_y != 0.0f) {
            visualizer_pan_by(visualizer, pan_x, pan_y);
        }
    }
}

// An overview of where the zoom window sits inside the whole display extent:
// outer box is everything the view could show, inner box is what it is showing.
// Quarter gridlines give the eye something to judge the position against.
void draw_zoom_minimap(opengl_visualizer_t* visualizer) {
    const float MINIMAP_W = 260.0f;
    const float MINIMAP_H = 110.0f;
    // A window zoomed in hard would otherwise shrink to nothing; keep it big
    // enough to stay visible and grabbable by eye.
    const float MINIMAP_MIN_PX = 3.0f;

    float xc = 0.5f, xs = 1.0f, yc = 0.5f, ys = 1.0f;
    visualizer_get_zoom(visualizer, &xc, &xs, &yc, &ys);

    ImGui::Dummy(ImVec2(MINIMAP_W, MINIMAP_H));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255));

    for (int q = 1; q < 4; q++) {
        const float fx = p0.x + w * (float)q * 0.25f;
        const float fy = p0.y + h * (float)q * 0.25f;
        const ImU32 grid = IM_COL32(70, 70, 78, 255);
        dl->AddLine(ImVec2(fx, p0.y), ImVec2(fx, p1.y), grid);
        dl->AddLine(ImVec2(p0.x, fy), ImVec2(p1.x, fy), grid);
    }

    float wx0 = p0.x + (xc - xs * 0.5f) * w;
    float wx1 = p0.x + (xc + xs * 0.5f) * w;
    float wy0 = p0.y + (yc - ys * 0.5f) * h;
    float wy1 = p0.y + (yc + ys * 0.5f) * h;
    if (wx1 - wx0 < MINIMAP_MIN_PX) {
        const float mid = (wx0 + wx1) * 0.5f;
        wx0 = mid - MINIMAP_MIN_PX * 0.5f;
        wx1 = mid + MINIMAP_MIN_PX * 0.5f;
    }
    if (wy1 - wy0 < MINIMAP_MIN_PX) {
        const float mid = (wy0 + wy1) * 0.5f;
        wy0 = mid - MINIMAP_MIN_PX * 0.5f;
        wy1 = mid + MINIMAP_MIN_PX * 0.5f;
    }

    const ImU32 window_fill = IM_COL32(255, 170, 40, 60);
    const ImU32 window_edge = IM_COL32(255, 190, 70, 255);
    dl->AddRectFilled(ImVec2(wx0, wy0), ImVec2(wx1, wy1), window_fill);
    dl->AddRect(ImVec2(wx0, wy0), ImVec2(wx1, wy1), window_edge);

    // Outer frame last so it stays crisp over the window rect at full view,
    // where the two coincide.
    dl->AddRect(p0, p1, IM_COL32(150, 150, 158, 255));
}

void draw_zoom_window(opengl_visualizer_t* visualizer) {
    if (visualizer == nullptr) {
        return;
    }

    ImGui::Begin("Zoom", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    // The one action and the one readout stay above the sections, so the
    // window says whether a zoom is active even with everything folded.
    const bool zoom_active = visualizer_zoom_is_active(visualizer) != 0;
    if (ImGui::Button("Reset to full view")) {
        visualizer_reset_zoom(visualizer);
    }
    ImGui::SameLine();
    if (zoom_active) {
        float xs = 1.0f, ys = 1.0f;
        visualizer_get_zoom(visualizer, nullptr, &xs, nullptr, &ys);
        ImGui::Text("x %.1fx   y %.1fx", (double)(1.0f / xs), (double)(1.0f / ys));
    } else {
        ImGui::TextDisabled("full view");
    }

    // The axes are not fixed any more: the radar spans delay taps horizontally
    // and Doppler bins vertically depending on its output, so the extents and
    // the labels are read from the view rather than assumed.
    visualizer_grid_info_t grid;
    visualizer_get_grid_info(visualizer, &grid);

    const bool curve_view = visualizer_get_view_mode(visualizer) == VIEW_MODE_CURVE;
    const bool delay_x = (grid.x_unit == AXIS_UNIT_DELAY_TAP);
    const bool doppler_y = (grid.y_unit == AXIS_UNIT_DOPPLER_BIN);

    const float x_total = (float)(grid.x_count > 0 ? grid.x_count : 1);
    // Curve view plots magnitude vertically, so its window is a percentage of
    // full scale rather than a count of rows.
    const int total_rows = visualizer_get_zoom_total_rows(visualizer);
    const float y_total = curve_view ? 100.0f : (float)(total_rows > 0 ? total_rows : 1);

    if (section_header("Overview", zoom_active)) {
        draw_zoom_minimap(visualizer);
        if (curve_view) {
            ImGui::TextDisabled("full extent: %d RE wide, magnitude tall", (int)x_total);
        } else {
            ImGui::TextDisabled("full extent: %d %s wide, %d %s tall",
                                (int)x_total, delay_x ? "taps" : "RE",
                                total_rows, doppler_y ? "bins" : "rows");
        }
        ImGui::TextDisabled("Display only - memory size, averaging period and");
        ImGui::TextDisabled("capture depth are untouched by anything here.");
    }

    zoom_window_t w = zoom_window_in_units(visualizer, x_total, y_total);
    bool changed = false;

    // The titles name what the axis measures in this view; the part after ###
    // is the ID, fixed so a section stays open or folded when the radar output
    // or the view mode changes what that is.
    const char* x_title = delay_x ? "Delay (horizontal)###zoom_x" : "Frequency (horizontal)###zoom_x";
    const char* y_title = doppler_y  ? "Doppler (vertical)###zoom_y"
                          : curve_view ? "Magnitude (vertical)###zoom_y"
                                       : "Time (vertical)###zoom_y";

    if (section_header(x_title, zoom_active)) {
        if (delay_x) {
            int tap_first = (int)lroundf(w.x_first);
            int tap_count = (int)lroundf(w.x_extent);
            if (ImGui::InputInt("First tap", &tap_first, 1, 16)) {
                w.x_first = (float)tap_first;
                changed = true;
            }
            if (ImGui::InputInt("Tap count", &tap_count, 1, 16)) {
                w.x_extent = (float)tap_count;
                changed = true;
            }
            const int tap_last = tap_first + (tap_count > 0 ? tap_count - 1 : 0);
            if (grid.x_unit_scale > 0.0f) {
                ImGui::TextDisabled("showing taps %d-%d  (%.1f-%.1f m of %.0f m)",
                                    tap_first, tap_last,
                                    (double)((float)tap_first * grid.x_unit_scale),
                                    (double)((float)tap_last * grid.x_unit_scale),
                                    (double)(x_total * grid.x_unit_scale));
            } else {
                ImGui::TextDisabled("showing taps %d-%d of %d", tap_first, tap_last, (int)x_total);
            }
        } else {
            int re_first = (int)lroundf(w.x_first);
            int re_count = (int)lroundf(w.x_extent);
            if (ImGui::InputInt("First RE", &re_first, 12, 120)) {
                w.x_first = (float)re_first;
                changed = true;
            }
            if (ImGui::InputInt("RE count", &re_count, 12, 120)) {
                w.x_extent = (float)re_count;
                changed = true;
            }
            ImGui::TextDisabled("showing RE %d-%d  (PRB %.1f-%.1f of %d)",
                                re_first,
                                re_first + (re_count > 0 ? re_count - 1 : 0),
                                (double)(w.x_first / 12.0f),
                                (double)((w.x_first + w.x_extent) / 12.0f),
                                (int)x_total / 12);
        }
    }

    if (section_header(y_title, zoom_active)) {
        if (doppler_y) {
            // Rows are frequency bins covering one processing interval, not a
            // sliding stretch of time, so they are addressed from the zero-Doppler
            // centre outward rather than as "first row".
            int bin_first = (int)lroundf(w.y_first);
            int bin_count = (int)lroundf(w.y_extent);
            if (ImGui::InputInt("First bin", &bin_first, 1, 10)) {
                w.y_first = (float)bin_first;
                changed = true;
            }
            if (ImGui::InputInt("Bin count", &bin_count, 1, 10)) {
                w.y_extent = (float)bin_count;
                changed = true;
            }
            const int centre = total_rows / 2;
            const int bin_last = bin_first + (bin_count > 0 ? bin_count - 1 : 0);
            if (grid.y_unit_scale > 0.0f) {
                ImGui::TextDisabled("showing %+.0f to %+.0f Hz",
                                    (double)((float)(centre - bin_first) * grid.y_unit_scale),
                                    (double)((float)(centre - bin_last) * grid.y_unit_scale));
            } else {
                ImGui::TextDisabled("showing bins %d-%d of %d", bin_first, bin_last, total_rows);
            }
            ImGui::TextDisabled("Zero Doppler is the centre row. The whole matrix is");
            ImGui::TextDisabled("replaced each interval, so this axis does not scroll.");
        } else if (curve_view) {
            // Normalized y runs downward, so the window's leading edge is the high
            // end of the magnitude scale; present it the way it reads on screen.
            // y_total is 100, so y_first/y_extent are already percentages.
            float pct_high = 100.0f - w.y_first;
            float pct_low = pct_high - w.y_extent;
            const bool high_edited = ImGui::InputFloat("Top of window (%)", &pct_high, 1.0f, 10.0f, "%.1f");
            const bool low_edited = ImGui::InputFloat("Bottom of window (%)", &pct_low, 1.0f, 10.0f, "%.1f");
            if (high_edited || low_edited) {
                // Either field can be dragged past the other; order them so the
                // window stays the right way up instead of inverting.
                if (pct_low > pct_high) {
                    const float swap = pct_low;
                    pct_low = pct_high;
                    pct_high = swap;
                }
                w.y_first = 100.0f - pct_high;
                w.y_extent = pct_high - pct_low;
                changed = true;
            }
            ImGui::TextDisabled("showing %.1f%%-%.1f%% of full scale", (double)pct_low, (double)pct_high);
        } else {
            int row_first = (int)lroundf(w.y_first);
            int row_count = (int)lroundf(w.y_extent);
            if (ImGui::InputInt("First row", &row_first, 1, 10)) {
                w.y_first = (float)row_first;
                changed = true;
            }
            if (ImGui::InputInt("Row count", &row_count, 1, 10)) {
                w.y_extent = (float)row_count;
                changed = true;
            }
            ImGui::TextDisabled("showing rows %d-%d of %d (row 0 = oldest on screen)",
                                row_first,
                                row_first + (row_count > 0 ? row_count - 1 : 0),
                                total_rows);
            ImGui::TextDisabled("Row count follows Memory size / averaging period /");
            ImGui::TextDisabled("capture depth, so the window is kept as a fraction");
            ImGui::TextDisabled("of it and survives those changing.");
        }
    }

    if (changed) {
        zoom_apply_units(visualizer, w, x_total, y_total);
    }

    if (section_header("Mouse and keys", false)) {
        ImGui::TextDisabled("Wheel over the plot zooms both axes; Shift+wheel");
        ImGui::TextDisabled("only horizontally, Ctrl+wheel only vertically.");
        ImGui::TextDisabled("Drag the plot or use the arrow keys to move the window.");
    }

    ImGui::End();
}

// --- Radar settings ---

#define RADAR_SPEED_OF_LIGHT 299792458.0

// Duration of one OFDM symbol including a normal cyclic prefix, in seconds.
// A slot is always 14 symbols and always 1 ms / (scs / 15 kHz), so the symbol
// period follows from the subcarrier spacing alone. Returns 0 when the stream
// has not reported its spacing yet.
static double radar_symbol_seconds(int scs_khz) {
    if (scs_khz <= 0) {
        return 0.0;
    }
    const double slots_per_subframe = (double)scs_khz / 15.0;
    if (slots_per_subframe <= 0.0) {
        return 0.0;
    }
    return 0.001 / slots_per_subframe / 14.0;
}

// Reads the radar config, runs the widgets against a local copy, and commits
// once if anything moved - the same read/edit/write shape the trigger's
// frequency range uses, generalised to the whole struct. Clamping lives in
// fh_radar_set_config, so raw widget values can go straight over.
static void draw_radar_window(fh_radar_t* radar,
                              opengl_visualizer_t* visualizer,
                              const fh_oran_memory_t* dl,
                              const fh_oran_memory_t* ul,
                              bool have_pair,
                              uint16_t dl_eaxc,
                              uint16_t ul_eaxc) {
    ImGui::Begin("Radar Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    const ImVec4 warn_color(1.0f, 0.45f, 0.35f, 1.0f);
    const ImVec4 ok_color(0.3f, 1.0f, 0.3f, 1.0f);

    if (radar == nullptr) {
        ImGui::TextColored(warn_color, "Radar core unavailable.");
        ImGui::End();
        return;
    }

    fh_radar_config_t cfg;
    fh_radar_get_config(radar, &cfg);
    fh_radar_stats_t st;
    fh_radar_get_stats(radar, &st);

    bool changed = false;

    // Used by several sections, so worked out before any of them.
    const int scs_khz = (dl != nullptr && dl->scs_khz > 0)
        ? dl->scs_khz
        : ((ul != nullptr && ul->scs_khz > 0) ? ul->scs_khz : 0);
    const double subcarrier_hz = (double)scs_khz * 1000.0;

    // --- Stream -----------------------------------------------------------
    if (section_header("Stream", true)) {
        if (!have_pair) {
            ImGui::TextColored(warn_color, "No eAxC selected.");
        } else {
            // Missing either direction is the most common reason the plot stays
            // blank, so each is called out rather than left to be inferred.
            ImGui::Text("DL (Tx reference): eAxC 0x%04X", dl_eaxc);
            ImGui::SameLine();
            if (dl != nullptr) {
                ImGui::TextColored(ok_color, "present");
            } else {
                ImGui::TextColored(warn_color, "not captured");
            }
            ImGui::Text("UL (Rx echo):      eAxC 0x%04X", ul_eaxc);
            ImGui::SameLine();
            if (ul != nullptr) {
                ImGui::TextColored(ok_color, "present");
            } else {
                ImGui::TextColored(warn_color, "not captured");
            }
            if (dl_eaxc != ul_eaxc) {
                ImGui::TextDisabled("Split pair: receiving on another eAxC than transmitting.");
            }
        }
        ImGui::TextDisabled("In Stream Selection, click an eAxC to pair its own DL and");
        ImGui::TextDisabled("UL, or a UL / DL cell to take that direction from another eAxC.");
    }

    // --- Processing window ------------------------------------------------
    if (section_header("Processing window", true)) {
        changed |= ImGui::InputInt("Time range (symbols)", &cfg.time_symbols);
        changed |= ImGui::InputInt("Freq. start (RE index)", &cfg.freq_start_re);
        changed |= ImGui::InputInt("Freq. end (RE index)", &cfg.freq_end_re);

        const int re_span = (cfg.freq_end_re >= cfg.freq_start_re)
            ? (cfg.freq_end_re - cfg.freq_start_re + 1) : 0;
        if (subcarrier_hz > 0.0 && re_span > 0) {
            const double bandwidth_hz = (double)re_span * subcarrier_hz;
            // What can actually be resolved in delay, set by occupied bandwidth.
            const double range_res_m = RADAR_SPEED_OF_LIGHT / (2.0 * bandwidth_hz);
            ImGui::TextDisabled("Occupied BW: %.2f MHz over %d RE", bandwidth_hz / 1.0e6, re_span);
            ImGui::TextDisabled("Range resolution: %.2f m", range_res_m);
        } else {
            ImGui::TextDisabled("Occupied BW: waiting for subcarrier spacing");
        }
    }

    // --- Symbol pairing ---------------------------------------------------
    if (section_header("Symbol pairing", true)) {
        changed |= ImGui::InputInt("DL->UL lag (symbols)", &cfg.dl_ul_lag_symbols);
        ImGui::TextDisabled("UL symbol n+lag is divided against DL symbol n. Sweep this");
        ImGui::TextDisabled("if the paired count below stays low or mean |H| is wrong.");

        int division_int = (int)cfg.division;
        ImGui::TextUnformatted("Division");
        ImGui::RadioButton("UL/DL  (H)", &division_int, RADAR_DIV_UL_OVER_DL);
        ImGui::SameLine();
        ImGui::RadioButton("DL/UL  (1/H)", &division_int, RADAR_DIV_DL_OVER_UL);
        if (division_int != (int)cfg.division) {
            cfg.division = (fh_radar_division_t)division_int;
            changed = true;
        }
        ImGui::TextDisabled("UL/DL is the physical channel. DL/UL is its reciprocal and");
        ImGui::TextDisabled("mirrors the delay axis - both read ~1 on occupied REs, so");
        ImGui::TextDisabled("the mean below cannot tell them apart.");

        ImGui::Text("Paired: %d / %d symbols", st.paired_symbols, st.window_symbols);
        ImGui::Text("Skipped: %d (DL empty), %d (UL missing)",
                    st.skipped_dl_empty, st.skipped_ul_missing);

        // The acceptance test: an occupied RE divided by itself through a unit
        // channel is 1, so a healthy setup parks this near 1.0.
        const bool mean_healthy = (st.mean_cfr_magnitude > 0.5f && st.mean_cfr_magnitude < 2.0f);
        ImGui::TextColored(mean_healthy ? ok_color : warn_color,
                           "Mean |H| on occupied REs: %.3f", st.mean_cfr_magnitude);
    }

    // --- Delay / range ----------------------------------------------------
    // This and Cancellation only act on the transform outputs, so with the CFR
    // on screen they have nothing to do and start folded.
    if (section_header("Delay / range", cfg.output != RADAR_OUT_CFR)) {
        static const int ifft_sizes[] = { 512, 1024, 2048, 4096, 8192 };
        static const char* ifft_labels[] = { "512", "1024", "2048", "4096", "8192" };
        int ifft_index = 3;
        for (int i = 0; i < IM_ARRAYSIZE(ifft_sizes); i++) {
            if (ifft_sizes[i] == cfg.ifft_size) {
                ifft_index = i;
                break;
            }
        }
        if (ImGui::Combo("IFFT size", &ifft_index, ifft_labels, IM_ARRAYSIZE(ifft_labels))) {
            cfg.ifft_size = ifft_sizes[ifft_index];
            changed = true;
        }

        // Only the Doppler stage truncates: Range-Time plots the full transform.
        const bool depth_applies = (cfg.output == RADAR_OUT_RANGE_DOPPLER);
        if (!depth_applies) {
            ImGui::BeginDisabled();
        }
        changed |= ImGui::InputInt("CIR depth (taps)", &cfg.cir_depth_taps);
        if (!depth_applies) {
            ImGui::EndDisabled();
        }
        if (depth_applies) {
            // One slow-time transform per kept tap, so this field is what sets the
            // refresh cost.
            ImGui::TextDisabled("%d slow-time FFTs of %d points per refresh",
                                cfg.cir_depth_taps, cfg.time_symbols);
        } else {
            ImGui::TextDisabled("Truncation feeds the Doppler stage only; Range-Time");
            ImGui::TextDisabled("plots all %d taps.", cfg.ifft_size);
        }

        if (subcarrier_hz > 0.0) {
            // Zero padding makes taps finer than the true resolution above; these
            // are deliberately reported as separate numbers.
            const double tap_spacing_m =
                RADAR_SPEED_OF_LIGHT / (2.0 * (double)cfg.ifft_size * subcarrier_hz);
            const int extent_taps = depth_applies ? cfg.cir_depth_taps : cfg.ifft_size;
            ImGui::TextDisabled("Tap spacing: %.2f m   Max range: %.1f m",
                                tap_spacing_m, tap_spacing_m * (double)extent_taps);
        }

        const double symbol_s = radar_symbol_seconds(scs_khz);
        if (symbol_s > 0.0 && cfg.time_symbols > 0) {
            const double doppler_res_hz = 1.0 / ((double)cfg.time_symbols * symbol_s);
            const double doppler_span_hz = 1.0 / (2.0 * symbol_s);
            ImGui::TextDisabled("Doppler: %.1f Hz resolution, +/- %.1f kHz unambiguous",
                                doppler_res_hz, doppler_span_hz / 1000.0);
        }
    }

    // --- Cancellation -----------------------------------------------------
    if (section_header("Cancellation", cfg.output != RADAR_OUT_CFR)) {
        bool mti = cfg.mti_enabled != 0;
        if (ImGui::Checkbox("Static clutter removal (MTI)", &mti)) {
            cfg.mti_enabled = mti ? 1 : 0;
            changed = true;
        }
        ImGui::TextDisabled("Subtracts each tap's mean over the window. Removes the");
        ImGui::TextDisabled("Tx->Rx leakage and anything stationary; moving targets stay.");

        bool blank = cfg.blank_taps_enabled != 0;
        if (ImGui::Checkbox("Blank leading taps", &blank)) {
            cfg.blank_taps_enabled = blank ? 1 : 0;
            changed = true;
        }
        if (!blank) {
            ImGui::BeginDisabled();
        }
        changed |= ImGui::InputInt("Taps to blank (K)", &cfg.blank_taps);
        if (!blank) {
            ImGui::EndDisabled();
        }
        ImGui::TextDisabled("Kills the direct path even when it drifts, which MTI alone");
        ImGui::TextDisabled("leaves as a residual - at the cost of any target that close.");
    }

    // --- Output -----------------------------------------------------------
    if (section_header("Output", true)) {
        int output_int = (int)cfg.output;
        ImGui::RadioButton("CFR magnitude", &output_int, RADAR_OUT_CFR);
        ImGui::RadioButton("Range-Time", &output_int, RADAR_OUT_RANGE_TIME);
        ImGui::RadioButton("Range-Doppler", &output_int, RADAR_OUT_RANGE_DOPPLER);
        if (output_int != (int)cfg.output) {
            cfg.output = (fh_radar_output_t)output_int;
            changed = true;
        }

        if (cfg.output == RADAR_OUT_CFR) {
            ImGui::TextDisabled("Symbol x RE. No transform involved - this is the view");
            ImGui::TextDisabled("that says whether the pairing itself is right.");
        } else {
            ImGui::TextColored(warn_color, "Not implemented yet - the transform stages");
            ImGui::TextColored(warn_color, "are the next step. The plot stays blank.");
        }
    }

    // --- Rolling mean -----------------------------------------------------
    // Lives in the visualizer rather than the radar core, like the waterfall's:
    // it is a way of displaying frames, not a processing stage. Its settings
    // are separate from the waterfall's because the two average different
    // quantities.
    if (visualizer != nullptr &&
        section_header("Rolling mean", visualizer_get_radar_persistence_enabled(visualizer) != 0)) {
        const bool mean_applies = (cfg.output == RADAR_OUT_CFR);
        if (!mean_applies) {
            ImGui::BeginDisabled();
        }

        bool mean_enabled = visualizer_get_radar_persistence_enabled(visualizer) != 0;
        if (ImGui::Checkbox("Rolling mean (persistence)", &mean_enabled)) {
            visualizer_set_radar_persistence_enabled(visualizer, mean_enabled ? 1 : 0);
        }

        int mean_period = visualizer_get_radar_persistence_period(visualizer);
        if (ImGui::InputInt("Averaging period (symbols)", &mean_period)) {
            visualizer_set_radar_persistence_period(visualizer, mean_period);
        }

        int mean_n = visualizer_get_radar_persistence_n(visualizer);
        if (ImGui::InputInt("n (periods to average)", &mean_n)) {
            visualizer_set_radar_persistence_n(visualizer, mean_n);
        }

        // Labelled apart from the radar's own Reset below, which drops the
        // computed frame as well; ImGui also needs the two IDs to differ.
        if (ImGui::Button("Reset mean")) {
            visualizer_reset_radar_persistence(visualizer);
        }
        if (mean_enabled) {
            ImGui::SameLine();
            ImGui::Text("Periods accumulated: %d (n = %d)",
                        visualizer_get_radar_persistence_periods(visualizer),
                        visualizer_get_radar_persistence_n(visualizer));
        }

        if (!mean_applies) {
            ImGui::EndDisabled();
            ImGui::TextDisabled("CFR magnitude only for now.");
        } else {
            ImGui::TextDisabled("Averages |H| per (symbol %% period, RE). REs without an");
            ImGui::TextDisabled("estimate in both directions are skipped and stay black.");
            ImGui::TextDisabled("Starts over on a new pair, a Reset, or a change of");
            ImGui::TextDisabled("frequency range, lag or division.");
            ImGui::TextDisabled("Tip: multiples of 14 align to slot boundaries");
        }
    }

    // --- Compute ----------------------------------------------------------
    if (section_header("Compute", true)) {
        ImGui::Text("FFT threads: %zu", fh_radar_get_fft_threads(radar));
        ImGui::TextDisabled("Fixed at startup, like decode threads.");
        ImGui::Text("Last refresh: %.1f ms   Frames: %llu",
                    st.last_refresh_ms, (unsigned long long)st.refresh_count);

        if (ImGui::Button("Reset")) {
            fh_radar_reset(radar);
        }
    }

    if (changed) {
        fh_radar_set_config(radar, &cfg);
    }

    ImGui::End();
}

// --- Value at cursor ---

// Raw IQ magnitudes run to thousands, while a channel estimate sits around 1,
// so a fixed precision either drowns the reading in digits or quantises away
// the part worth looking at. Picked from the value rather than the view, which
// keeps it right for whatever a future output stage happens to produce.
static const char* value_format(float value) {
    const float magnitude = (value < 0.0f) ? -value : value;
    return (magnitude < 100.0f) ? "%.3f" : "%.1f";
}

// Names the cell a reading came from in whatever the current view's axes
// measure, so one readout serves the resource grid, the delay axis and the
// Doppler axis without any of them being described in another's units.
static void format_cell_column(const visualizer_grid_info_t& grid, int column,
                               char* out, size_t out_size) {
    if (grid.x_unit == AXIS_UNIT_DELAY_TAP) {
        if (grid.x_unit_scale > 0.0f) {
            snprintf(out, out_size, "tap %d (%.1f m)", column,
                     static_cast<double>(static_cast<float>(column) * grid.x_unit_scale));
        } else {
            snprintf(out, out_size, "tap %d", column);
        }
        return;
    }
    snprintf(out, out_size, "RE %d", column);
}

static void format_cell_row(const visualizer_grid_info_t& grid,
                            const visualizer_cursor_sample_t& sample,
                            char* out, size_t out_size) {
    if (grid.y_unit == AXIS_UNIT_DOPPLER_BIN) {
        // Zero Doppler is the centre row; a bin index alone would not say
        // which side of it a reading is on.
        const int centre = grid.y_count / 2;
        if (grid.y_unit_scale > 0.0f) {
            snprintf(out, out_size, "%+.0f Hz",
                     static_cast<double>(static_cast<float>(centre - sample.row) * grid.y_unit_scale));
        } else {
            snprintf(out, out_size, "bin %+d", centre - sample.row);
        }
        return;
    }
    if (sample.has_phase) {
        snprintf(out, out_size, "phase +%d", sample.phase);
        return;
    }
    if (sample.has_symbol) {
        // Spelled out, since the timing line below has a "sym" of its own for
        // the symbol within the slot.
        snprintf(out, out_size, "symbol %d", sample.symbol);
        return;
    }
    snprintf(out, out_size, "row %d", sample.row);
}

// The frame / subframe / slot / symbol position of the cell's symbol, as the
// O-RAN header carries it: the 8-bit frame id first, as on the wire, with the
// frame count that never wraps in brackets. Empty when there is no symbol to
// place, or the stream's subcarrier spacing is not known yet.
static void format_cell_timing(const visualizer_cursor_sample_t& sample, char* out, size_t out_size) {
    if (!sample.has_timing) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "frame %d (%d)  sf %d  slot %d  sym %d",
             sample.frame_id, sample.frame, sample.subframe, sample.slot, sample.slot_symbol);
}

// Fills in the cell under the pointer, or clears valid when there is nothing to
// read (cursor off the plot, over a panel, or the view is Curve).
visualizer_cursor_sample_t sample_under_cursor(opengl_visualizer_t* visualizer,
                                               const fh_oran_memory_t* memory) {
    visualizer_cursor_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    if (visualizer == nullptr) {
        return sample;
    }

    ImGuiIO& io = ImGui::GetIO();
    // A pointer parked over a settings panel isn't pointing at data, and
    // reading the cell hidden behind the panel would be misleading.
    if (io.WantCaptureMouse) {
        return sample;
    }

    visualizer_sample_at_screen(visualizer, memory, io.MousePos.x, io.MousePos.y, &sample);
    return sample;
}

// Crosshair plus a value box pinned to the pointer, in the same style as the
// marker labels so the two read as the same kind of annotation.
void draw_cursor_value_overlay(opengl_visualizer_t* visualizer,
                               const visualizer_cursor_sample_t& sample) {
    if (!sample.valid) {
        return;
    }

    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 0.0f, plot_h = 0.0f;
    visualizer_get_plot_rect(visualizer, &plot_x, &plot_y, &plot_w, &plot_h);
    if (plot_w <= 1.0f || plot_h <= 1.0f) {
        return;
    }
    const float plot_right = plot_x + plot_w;
    const float plot_bottom = plot_y + plot_h;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 at = io.MousePos;

    const ImU32 line_color = IM_COL32(120, 230, 255, 140);
    draw->AddLine(ImVec2(plot_x, at.y), ImVec2(plot_right, at.y), line_color);
    draw->AddLine(ImVec2(at.x, plot_y), ImVec2(at.x, plot_bottom), line_color);

    char line1[64];
    char line2[64];
    // Wide enough for the two cell labels plus their separator; a delay tap
    // carrying its range in metres is the longest either of them gets.
    char line3[112];
    char line4[96];
    if (sample.has_value) {
        snprintf(line1, sizeof(line1), value_format(sample.magnitude),
                 static_cast<double>(sample.magnitude));
        snprintf(line2, sizeof(line2), "%.1f%% base", static_cast<double>(sample.percent_of_baseline));
    } else {
        snprintf(line1, sizeof(line1), "no data");
        line2[0] = '\0';
    }
    visualizer_grid_info_t grid;
    visualizer_get_grid_info(visualizer, &grid);
    char col_text[48];
    char row_text[48];
    format_cell_column(grid, sample.re, col_text, sizeof(col_text));
    format_cell_row(grid, sample, row_text, sizeof(row_text));
    snprintf(line3, sizeof(line3), "%s  %s", col_text, row_text);
    format_cell_timing(sample, line4, sizeof(line4));

    const char* lines[4] = { line1, line2, line3, line4 };
    float text_w = 0.0f;
    float text_h = 0.0f;
    for (int i = 0; i < 4; i++) {
        if (lines[i][0] == '\0') {
            continue;
        }
        const ImVec2 size = ImGui::CalcTextSize(lines[i]);
        if (size.x > text_w) {
            text_w = size.x;
        }
        text_h += size.y;
    }

    const float pad = 4.0f;
    const float box_w = text_w + pad * 2.0f;
    const float box_h = text_h + pad * 2.0f;

    // Prefer below-right of the pointer, flipping and then clamping so the box
    // never leaves the plot or sits under the pointer itself.
    float box_x = at.x + 14.0f;
    float box_y = at.y + 14.0f;
    if (box_x + box_w > plot_right) {
        box_x = at.x - 14.0f - box_w;
    }
    if (box_y + box_h > plot_bottom) {
        box_y = at.y - 14.0f - box_h;
    }
    if (box_x < plot_x) {
        box_x = plot_x;
    }
    if (box_y < plot_y) {
        box_y = plot_y;
    }

    const ImVec2 box_min(box_x, box_y);
    const ImVec2 box_max(box_x + box_w, box_y + box_h);
    draw->AddRectFilled(box_min, box_max, IM_COL32(20, 28, 34, 235), 3.0f);
    draw->AddRect(box_min, box_max, IM_COL32(120, 230, 255, 255), 3.0f, 0, 1.5f);

    float text_y = box_y + pad;
    for (int i = 0; i < 4; i++) {
        if (lines[i][0] == '\0') {
            continue;
        }
        draw->AddText(ImVec2(box_x + pad, text_y), IM_COL32(235, 250, 255, 255), lines[i]);
        text_y += ImGui::CalcTextSize(lines[i]).y;
    }
}

// Hands the capture the channels on screen while "Only decode selected" is
// ticked, and 0 - decode everything - while it is not. For the radar that is
// the DL/UL pair it reads, which need not share an eAxC; for every other view
// the one stream actually displayed, which is not always the selected
// direction (the display falls back to an eAxC's other direction when the
// selected one does not exist). Nothing on screen yet also means 0, so the
// first streams can still be discovered. Stored every frame: a word the
// capture thread reads per packet, cheaper than tracking what changed.
void publish_decode_selection(bool enabled,
                              bool radar_view,
                              bool have_radar_pair,
                              uint16_t radar_dl_eaxc,
                              uint16_t radar_ul_eaxc,
                              const fh_memory_key_t* keys,
                              size_t num_keys,
                              fh_oran_memory_t* const* memory_ptrs,
                              const fh_oran_memory_t* shown,
                              uint64_t* decode_selection) {
    if (decode_selection == nullptr) {
        return;
    }
    fh_channel_whitelist_entry_t on_screen[FH_CHANNEL_SELECTION_MAX];
    int count = 0;
    if (enabled && radar_view) {
        if (have_radar_pair) {
            on_screen[count++] = fh_channel_whitelist_entry_t{radar_dl_eaxc, FH_CHANNEL_DIR_DL};
            on_screen[count++] = fh_channel_whitelist_entry_t{radar_ul_eaxc, FH_CHANNEL_DIR_UL};
        }
    } else if (enabled && shown != nullptr && memory_ptrs != nullptr) {
        for (size_t i = 0; i < num_keys; i++) {
            if (memory_ptrs[i] == shown) {
                const uint8_t bit = (keys[i].data_direction == 1) ? FH_CHANNEL_DIR_DL : FH_CHANNEL_DIR_UL;
                on_screen[count++] = fh_channel_whitelist_entry_t{keys[i].eaxc_id, bit};
                break;
            }
        }
    }
    __atomic_store_n(decode_selection, fh_channel_selection_pack(on_screen, count), __ATOMIC_RELAXED);
}

// A freeze cannot start while the waterfall's trigger is on: the trigger
// already decides what that view holds. Releasing one is always possible, so
// this only ever gates switching it on.
bool freeze_can_start(opengl_visualizer_t* visualizer) {
    return visualizer != nullptr && !(visualizer_get_view_mode(visualizer) == VIEW_MODE_WATERFALL &&
                                      visualizer_get_trigger_mode(visualizer) != TRIGGER_OFF);
}

void toggle_freeze(imgui_visualizer_state_t* state, opengl_visualizer_t* visualizer) {
    if (state->frozen) {
        state->frozen = false;
    } else if (freeze_can_start(visualizer)) {
        state->frozen = true;
    }
}

// Marks a frozen view on the plot itself, top right, in the cursor readout's
// style: frozen data looks exactly like a live stream that happens to be
// quiet, and the button saying so sits in another window.
void draw_frozen_badge(opengl_visualizer_t* visualizer) {
    float plot_x = 0.0f, plot_y = 0.0f, plot_w = 0.0f, plot_h = 0.0f;
    visualizer_get_plot_rect(visualizer, &plot_x, &plot_y, &plot_w, &plot_h);
    if (plot_w <= 1.0f || plot_h <= 1.0f) {
        return;
    }

    const char* text = "FROZEN";
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float pad = 4.0f;
    const float inset = 8.0f;
    const ImVec2 box_max(plot_x + plot_w - inset, plot_y + inset + size.y + pad * 2.0f);
    const ImVec2 box_min(box_max.x - size.x - pad * 2.0f, plot_y + inset);

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(box_min, box_max, IM_COL32(20, 28, 34, 235), 3.0f);
    draw->AddRect(box_min, box_max, IM_COL32(120, 230, 255, 255), 3.0f, 0, 1.5f);
    draw->AddText(ImVec2(box_min.x + pad, box_min.y + pad), IM_COL32(235, 250, 255, 255), text);
}

}  // namespace

const char* data_direction_to_string(uint8_t data_direction) {
    switch (data_direction) {
        case 0:
            return "UL";  // Uplink
        case 1:
            return "DL";  // Downlink
        default:
            return "UNKNOWN";
    }
}

imgui_visualizer_state_t* imgui_visualizer_init(int window_width, int window_height) {
    imgui_visualizer_state_t* state = (imgui_visualizer_state_t*)malloc(sizeof(*state));
    if (!state) {
        return NULL;
    }

    memset(state, 0, sizeof(*state));
    state->initialized = true;
    state->selection_initialized = false;
    state->selected_eaxc_id_index = 0;
    state->selected_data_direction = 0;  // Default to UL
    state->prev_data_direction = 0;
    state->radar_pair_explicit = false;
    state->radar_dl_eaxc = 0;
    state->radar_ul_eaxc = 0;
    state->only_decode_selected = false;
    state->frozen = false;
    state->selected_marker = -1;
    state->cursor_value_enabled = false;
    state->cursor_value_at_cursor = true;
    state->cursor_value_in_window = true;
    state->frame_tracking_initialized = false;
    state->frame_tracking_memory = nullptr;
    state->last_absolute_frame = 0;
    state->frames_since_start = 0;
    state->packet_rate_initialized = false;
    state->packet_rate_last_time = 0.0;
    state->packet_rate_last_total = 0;
    state->packet_rate_pps = 0.0;

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // IMPORTANT: Set display size to match the window we're rendering to
    io.DisplaySize = ImVec2(static_cast<float>(window_width), static_cast<float>(window_height));
    
    // Setup ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    GLFWwindow* window = glfwGetCurrentContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    return state;
}

fh_oran_memory_t* imgui_visualizer_render(
    imgui_visualizer_state_t* state,
    const fh_memory_key_t* keys,
    size_t num_keys,
    fh_oran_memory_t** memory_ptrs,
    fh_oran_memory_t* current_display_memory,
    int window_width,
    int window_height,
    fh_queue_stats_t stats,
    opengl_visualizer_t* visualizer,
    const char* source_name,
    int* pcap_speed_pps,
    uint64_t* decode_selection,
    fh_radar_t* radar,
    fh_oran_memory_t** out_radar_dl,
    fh_oran_memory_t** out_radar_ul) {

    if (out_radar_dl != nullptr) { *out_radar_dl = nullptr; }
    if (out_radar_ul != nullptr) { *out_radar_ul = nullptr; }

    if (!state || !state->initialized || num_keys == 0) {
        return current_display_memory;
    }

    // Start ImGui frame
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Update display size in case window was resized, and grab IO reference
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(window_width), static_cast<float>(window_height));

    // Wheel, drag and arrow keys drive the zoom. Done here, right after
    // NewFrame, so the WantCapture flags for this frame are already resolved and
    // input aimed at a settings window never reaches the plot.
    handle_zoom_input(visualizer);

    // Space toggles the freeze - the natural reflex when something interesting
    // scrolls past mid-zoom. Same guard as the arrow keys: not while a text
    // field or other widget has the keyboard.
    if (visualizer != nullptr && !io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        toggle_freeze(state, visualizer);
    }

    // Sampled once per frame and shared by both destinations, so the box at the
    // pointer and the readout in the Markers window can never show different
    // numbers for the same cell. Resolved against the axis state the last
    // visualizer_update() left behind, the same one-frame-old view the axes and
    // markers are drawn from.
    const bool curve_view_active = visualizer != nullptr &&
                                   visualizer_get_view_mode(visualizer) == VIEW_MODE_CURVE;
    visualizer_cursor_sample_t cursor_sample;
    memset(&cursor_sample, 0, sizeof(cursor_sample));
    if (state->cursor_value_enabled) {
        cursor_sample = sample_under_cursor(visualizer, current_display_memory);
    }

    // Create stream selection window
    // Center on first appearance so the selector doesn't open partially off-screen.
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing,
        ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);

    // Apply startup-specific theme: purple background, green accents
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.4f, 0.2f, 0.5f, 0.95f));       // Purple
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.9f, 0.2f, 1.0f));             // Green text
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 0.8f));           // Green button
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.9f, 0.3f, 1.0f));    // Bright green hover
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.6f, 0.1f, 1.0f));     // Dark green active
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.6f, 0.2f, 0.8f));           // Green header
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.3f, 0.8f, 0.3f, 0.9f));    // Bright green header hover
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.2f, 0.8f));        // Dark frame
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.3f, 0.9f, 0.3f, 1.0f));        // Green checkmark

    ImGui::Begin("Stream Selection", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("eAxC_ID");
    ImGui::SameLine();
    ImGui::Text("Direction");

    // Build list of unique eAxC_IDs with their available directions
    struct StreamInfo {
        uint16_t eaxc_id;
        bool has_ul;
        bool has_dl;
    };

    // Collect unique eAxC_IDs
    StreamInfo streams[256] = {};  // Max 256 unique eAxC_IDs (arbitrary limit)
    int num_unique_streams = 0;

    for (size_t i = 0; i < num_keys; i++) {
        uint16_t eaxc_id = keys[i].eaxc_id;
        uint8_t direction = keys[i].data_direction;

        // Find or create stream entry
        int stream_idx = -1;
        for (int j = 0; j < num_unique_streams; j++) {
            if (streams[j].eaxc_id == eaxc_id) {
                stream_idx = j;
                break;
            }
        }

        if (stream_idx == -1) {
            if (num_unique_streams < 256) {
                stream_idx = num_unique_streams++;
                streams[stream_idx].eaxc_id = eaxc_id;
            } else {
                continue;  // Too many streams, skip
            }
        }

        // Mark which directions are available
        if (direction == 0) {
            streams[stream_idx].has_ul = true;
        } else if (direction == 1) {
            streams[stream_idx].has_dl = true;
        }
    }

    // Ensure selected index is valid
    if (state->selected_eaxc_id_index >= num_unique_streams) {
        state->selected_eaxc_id_index = 0;
    }

    // On first display, sync the highlighted selection to the memory that is
    // already being shown in the main heatmap so the active UL/DL state matches.
    if (!state->selection_initialized && current_display_memory != nullptr) {
        bool synced = false;

        for (size_t i = 0; i < num_keys && !synced; i++) {
            if (memory_ptrs == nullptr || memory_ptrs[i] != current_display_memory) {
                continue;
            }

            const uint16_t target_eaxc = keys[i].eaxc_id;
            const uint8_t target_direction = keys[i].data_direction;

            for (int j = 0; j < num_unique_streams; j++) {
                if (streams[j].eaxc_id == target_eaxc) {
                    state->selected_eaxc_id_index = j;
                    state->selected_data_direction = target_direction;
                    synced = true;
                    break;
                }
            }
        }

        // If we couldn't map the current memory pointer, choose a stable first
        // visible direction instead of leaving the stale default UL selection.
        if (!synced && num_unique_streams > 0) {
            state->selected_eaxc_id_index = 0;
            if (streams[0].has_dl && !streams[0].has_ul) {
                state->selected_data_direction = 1;
            } else if (streams[0].has_ul && !streams[0].has_dl) {
                state->selected_data_direction = 0;
            }
        }

        state->selection_initialized = true;
    }

    // Display stream list
    ImGui::BeginChild("StreamList", ImVec2(280, 120), true, ImGuiWindowFlags_AlwaysAutoResize);

    // The radar reads a DL and a UL ring at once. Clicking an eAxC pairs that
    // eAxC's own two directions; clicking a UL or DL cell takes just that
    // direction from that row and leaves the other where it was, which is how
    // a receive antenna other than the transmitting one gets paired.
    const bool radar_selection = visualizer != nullptr &&
                                 visualizer_get_view_mode(visualizer) == VIEW_MODE_RADAR;

    // Resolves the pair currently in force; see radar_pair_explicit.
    auto resolve_radar_pair = [&](uint16_t* dl_eaxc, uint16_t* ul_eaxc) -> bool {
        if (state->radar_pair_explicit) {
            *dl_eaxc = state->radar_dl_eaxc;
            *ul_eaxc = state->radar_ul_eaxc;
            return true;
        }
        if (state->selected_eaxc_id_index < num_unique_streams) {
            *dl_eaxc = streams[state->selected_eaxc_id_index].eaxc_id;
            *ul_eaxc = *dl_eaxc;
            return true;
        }
        return false;
    };

    // Resolved before the table so the highlights show the pair as it stood
    // coming into this frame, the same way the other buttons do.
    uint16_t shown_radar_dl_eaxc = 0;
    uint16_t shown_radar_ul_eaxc = 0;
    const bool shown_radar_pair = resolve_radar_pair(&shown_radar_dl_eaxc, &shown_radar_ul_eaxc);

    ImGui::TextUnformatted("Columns: eAxC_ID | UL | DL");
    ImGui::Spacing();

    // Frozen with only the selected channels being decoded, a switch has
    // nothing from the frozen moment to show: the other channels' memories
    // stopped filling when they were deselected, long before. So the table is
    // locked until the freeze is released. With every channel decoded, a
    // switch simply shows the other channel's memory, frozen at the same
    // moment - which is the point of freezing.
    const bool switch_locked = state->only_decode_selected && state->frozen;
    if (switch_locked) {
        ImGui::BeginDisabled();
    }

    if (ImGui::BeginTable("stream_table", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("eAxC_ID", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("UL", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("DL", ImGuiTableColumnFlags_WidthFixed, 48.0f);

        for (int i = 0; i < num_unique_streams; i++) {
            uint16_t eaxc_id = streams[i].eaxc_id;
            ImGui::PushID(i);

            const bool row_selected = (state->selected_eaxc_id_index == i);
            // In radar mode each direction is highlighted on whichever row the
            // pair takes it from, which need not be the same row.
            const bool ul_selected = radar_selection
                                         ? (shown_radar_pair && shown_radar_ul_eaxc == eaxc_id)
                                         : (row_selected && state->selected_data_direction == 0);
            const bool dl_selected = radar_selection
                                         ? (shown_radar_pair && shown_radar_dl_eaxc == eaxc_id)
                                         : (row_selected && state->selected_data_direction == 1);

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);

            // Format eAxC_ID for display
            char eaxc_label[16];
            snprintf(eaxc_label, sizeof(eaxc_label), "0x%04X", eaxc_id);

            // Lit in radar mode when this eAxC supplies both directions, i.e.
            // when clicking it again would change nothing. A split pair lights
            // only its two direction cells.
            const bool highlight_eaxc = radar_selection && ul_selected && dl_selected;
            if (highlight_eaxc) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.95f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.64f, 1.00f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.40f, 0.80f, 1.00f));
            }
            bool eaxc_clicked = ImGui::Button(eaxc_label, ImVec2(80.0f, 0.0f));
            if (highlight_eaxc) {
                ImGui::PopStyleColor(3);
            }

            if (eaxc_clicked) {
                state->selected_eaxc_id_index = i;
                if (radar_selection) {
                    // One antenna both sending and receiving: the usual case,
                    // and the whole of what the radar could pair before.
                    state->radar_pair_explicit = true;
                    state->radar_dl_eaxc = eaxc_id;
                    state->radar_ul_eaxc = eaxc_id;
                }
                // A direction is still resolved underneath, even in radar mode:
                // it decides which ring the rest of the app treats as the
                // displayed stream, and that is where the subcarrier spacing
                // the radar's derived figures depend on comes from. Radar
                // prefers downlink, the reference the channel estimate divides
                // by; elsewhere a direction that is still available is kept, so
                // clicking an eAxC does not silently switch what is on screen.
                if (radar_selection && streams[i].has_dl) {
                    state->selected_data_direction = 1;
                } else if (!streams[i].has_ul && streams[i].has_dl) {
                    state->selected_data_direction = 1;
                } else if (!streams[i].has_dl && streams[i].has_ul) {
                    state->selected_data_direction = 0;
                }
            }

            auto draw_direction_button = [&](const char* label, uint8_t direction, bool available, bool active) {
                ImGui::TableSetColumnIndex(direction == 0 ? 1 : 2);

                if (!available) {
                    ImGui::BeginDisabled();
                }

                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.95f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.64f, 1.00f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.40f, 0.80f, 1.00f));
                }

                bool button_clicked = ImGui::Button(label, ImVec2(40.0f, 0.0f));
                
                if (active) {
                    ImGui::PopStyleColor(3);
                }

                if (!available) {
                    ImGui::EndDisabled();
                }
                
                if (!button_clicked) {
                    return;
                }
                if (!radar_selection) {
                    state->selected_eaxc_id_index = i;
                    state->selected_data_direction = direction;
                    return;
                }

                // Radar: take only this direction from this row. The other
                // keeps whatever the pair had, including a pair that was still
                // just following the selected eAxC.
                uint16_t dl_eaxc = eaxc_id;
                uint16_t ul_eaxc = eaxc_id;
                resolve_radar_pair(&dl_eaxc, &ul_eaxc);
                if (direction == 1) {
                    dl_eaxc = eaxc_id;
                    // The displayed stream follows the radar's DL, as it does
                    // for an eAxC click: the window is indexed by DL symbols,
                    // and the axes number it from the displayed stream.
                    state->selected_eaxc_id_index = i;
                    state->selected_data_direction = 1;
                } else {
                    ul_eaxc = eaxc_id;
                }
                state->radar_pair_explicit = true;
                state->radar_dl_eaxc = dl_eaxc;
                state->radar_ul_eaxc = ul_eaxc;
            };

            draw_direction_button("UL", 0, streams[i].has_ul, ul_selected);
            draw_direction_button("DL", 1, streams[i].has_dl, dl_selected);

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (switch_locked) {
        ImGui::EndDisabled();
    }

    ImGui::EndChild();

    // The radar reads one DL and one UL ring, not the single stream the other
    // views draw. Resolved after the table so this frame's clicks already
    // count, from the same arrays the selector was built from. Looked up by
    // key rather than by the table's presence flags, so a direction that has
    // not been captured comes back NULL and is reported as such.
    uint16_t radar_dl_eaxc = 0;
    uint16_t radar_ul_eaxc = 0;
    const bool have_radar_pair = resolve_radar_pair(&radar_dl_eaxc, &radar_ul_eaxc);
    fh_oran_memory_t* radar_dl = nullptr;
    fh_oran_memory_t* radar_ul = nullptr;
    if (have_radar_pair && memory_ptrs != nullptr) {
        for (size_t i = 0; i < num_keys; i++) {
            if (keys[i].data_direction == 1 && keys[i].eaxc_id == radar_dl_eaxc) {
                radar_dl = memory_ptrs[i];
            } else if (keys[i].data_direction == 0 && keys[i].eaxc_id == radar_ul_eaxc) {
                radar_ul = memory_ptrs[i];
            }
        }
    }
    if (out_radar_dl != nullptr) { *out_radar_dl = radar_dl; }
    if (out_radar_ul != nullptr) { *out_radar_ul = radar_ul; }

    // Status display
    const ImVec4 status_ok(0.3f, 1.0f, 0.3f, 1.0f);
    const ImVec4 status_warn(1.0f, 0.45f, 0.35f, 1.0f);
    if (radar_selection) {
        // Both directions are named, since they need not share an eAxC.
        if (have_radar_pair) {
            ImGui::Text("Radar: DL 0x%04X / UL 0x%04X", radar_dl_eaxc, radar_ul_eaxc);
            if (radar_dl != nullptr && radar_ul != nullptr) {
                ImGui::TextColored(status_ok, "DL and UL present - radar can run");
            } else if (radar_dl == nullptr && radar_ul == nullptr) {
                ImGui::TextColored(status_warn, "Neither direction captured yet");
            } else if (radar_dl == nullptr) {
                ImGui::TextColored(status_warn, "No DL on 0x%04X - the radar needs both", radar_dl_eaxc);
            } else {
                ImGui::TextColored(status_warn, "No UL on 0x%04X - the radar needs both", radar_ul_eaxc);
            }
        }
        // Outside the list's scroll region, where it costs no table rows.
        ImGui::TextDisabled("eAxC: its own UL+DL. UL/DL cell: that direction only.");
    } else if (state->selected_eaxc_id_index < num_unique_streams) {
        const char* direction_str = data_direction_to_string(state->selected_data_direction);
        ImGui::Text("Selected: 0x%04X / %s",
                    streams[state->selected_eaxc_id_index].eaxc_id, direction_str);
    }

    ImGui::Separator();
    ImGui::Checkbox("Only decode selected", &state->only_decode_selected);
    if (state->only_decode_selected) {
        ImGui::TextDisabled(radar_selection ? "Decoding the radar's DL and UL only, on top of"
                                            : "Decoding the stream on screen only, on top of");
        ImGui::TextDisabled("the startup filter. The rest hold still until selected.");
    }
    if (switch_locked) {
        ImGui::TextColored(ImVec4(0.47f, 0.90f, 1.00f, 1.0f), "Channels locked while frozen - unfreeze to switch.");
    }

    ImGui::End();

    // Statistics window: live packet drop rate (inside the same frame)
    double dropped_pct = (stats.total_packets > 0)
        ? (100.0 * static_cast<double>(stats.dropped_packets) / static_cast<double>(stats.total_packets))
        : 0.0;

    // Sample the newest frame decoded for the stream on screen, and fold its
    // advance into a running total. Reading these without the data lock
    // matches how the renderer already samples the same memory each frame.
    uint8_t current_frame_id = 0;
    bool have_frame_info = false;
    if (current_display_memory != nullptr) {
        const fh_oran_memory_t* mem = current_display_memory;
        current_frame_id = mem->last_frame_id;
        have_frame_info = true;

        const uint64_t absolute_frame = mem->frame_counter * 256ULL + static_cast<uint64_t>(mem->last_frame_id);
        if (!state->frame_tracking_initialized || state->frame_tracking_memory != mem) {
            // First sample, or the selection changed: re-baseline against the
            // new stream's own numbering rather than counting the difference
            // between two unrelated counters as elapsed frames.
            state->frame_tracking_memory = mem;
            state->frame_tracking_initialized = true;
        } else if (absolute_frame > state->last_absolute_frame) {
            state->frames_since_start += absolute_frame - state->last_absolute_frame;
        }
        // A decrease means the counter restarted (pcap replay loops reset it);
        // adopt the new value without letting the total go backwards.
        state->last_absolute_frame = absolute_frame;
    }

    // Arrival rate, refreshed every half second rather than per frame: a
    // per-frame figure jitters with the render loop and reads as noise. The
    // total counts every packet offered to the queue, dropped or not, and
    // never goes backwards, so a decrease can only mean a new queue.
    const double rate_now = ImGui::GetTime();
    if (!state->packet_rate_initialized || stats.total_packets < state->packet_rate_last_total) {
        state->packet_rate_initialized = true;
        state->packet_rate_last_time = rate_now;
        state->packet_rate_last_total = stats.total_packets;
    } else if (rate_now - state->packet_rate_last_time >= 0.5) {
        state->packet_rate_pps =
            static_cast<double>(stats.total_packets - state->packet_rate_last_total) /
            (rate_now - state->packet_rate_last_time);
        state->packet_rate_last_time = rate_now;
        state->packet_rate_last_total = stats.total_packets;
    }

    ImGui::Begin("Live Statistics", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    if (section_header("Data Query", true)) {
        // Which capture this run is reading. Worth having on screen: the
        // counters below mean quite different things for a looped pcap than
        // for a live interface, and nothing else in the running UI says which
        // one is in force.
        ImGui::Text("Source: %s", (source_name != nullptr && source_name[0] != '\0')
                                      ? source_name : "unknown");

        if (pcap_speed_pps != nullptr) {
            // The replay scheduler re-reads this before every packet and while
            // waiting, so a change lands at once even in the middle of a long
            // gap at a very low speed. The range matches the startup screen's.
            int speed = __atomic_load_n(pcap_speed_pps, __ATOMIC_RELAXED);
            bool speed_changed = false;
            ImGui::SetNextItemWidth(200.0f);
            // Logarithmic, since useful speeds span several decades; Ctrl+click
            // types an exact value.
            speed_changed |= ImGui::SliderInt("PCAP speed (packets/sec)", &speed, 1, FH_PCAP_MAX_SPEED_PPS, "%d",
                                              ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::Button("Half")) {
                speed = (speed > 1) ? speed / 2 : 1;
                speed_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Double")) {
                speed = (speed <= FH_PCAP_MAX_SPEED_PPS / 2) ? speed * 2 : FH_PCAP_MAX_SPEED_PPS;
                speed_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Ctrl+click the slider to type a value");
            if (speed_changed) {
                __atomic_store_n(pcap_speed_pps, speed, __ATOMIC_RELAXED);
            }
        }

        // Next to the speed setting because it is the check on it. The replay
        // schedule holds its average exactly, so a pcap rate short of the
        // setting means the pipeline behind it has hit its limit - read it
        // together with the drop rate below.
        ImGui::Text("Packet rate: %.0f packets/sec", state->packet_rate_pps);
        ImGui::Text("Packet drop rate: %.2f%%", dropped_pct);
        ImGui::Text("Total packets: %llu", static_cast<unsigned long long>(stats.total_packets));
        ImGui::Text("Dropped packets: %llu", static_cast<unsigned long long>(stats.dropped_packets));
        ImGui::Text("Queued packets: %llu", static_cast<unsigned long long>(stats.queued_packets));
    }

    if (section_header("Timing Info", true)) {
        if (have_frame_info) {
            ImGui::Text("Frame number: %u", static_cast<unsigned>(current_frame_id));
            ImGui::Text("Frames since start: %llu",
                        static_cast<unsigned long long>(state->frames_since_start));
            ImGui::TextDisabled("Frame number wraps at 256; the total does not.");
        } else {
            ImGui::TextDisabled("No stream selected");
        }
    }

    ImGui::End();

    // View mode selector, and the persistence window matching whichever view is active
    if (visualizer != nullptr) {
        ImGui::Begin("View Mode", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        visualizer_view_mode_t view_mode = visualizer_get_view_mode(visualizer);
        int mode_int = (int)view_mode;
        ImGui::RadioButton("Waterfall", &mode_int, VIEW_MODE_WATERFALL);
        ImGui::SameLine();
        ImGui::RadioButton("Curve", &mode_int, VIEW_MODE_CURVE);
        ImGui::SameLine();
        ImGui::RadioButton("Radar", &mode_int, VIEW_MODE_RADAR);
        if (mode_int != (int)view_mode) {
            // A freeze outlasts a view switch, with one exception: arriving at
            // the radar while only the selected stream is decoded. The radar
            // reads a DL/UL pair, and the pair's other channel was not being
            // decoded, so its frozen memory would not be from the same moment.
            if (mode_int == VIEW_MODE_RADAR && state->only_decode_selected) {
                state->frozen = false;
            }
            visualizer_set_view_mode(visualizer, (visualizer_view_mode_t)mode_int);
            view_mode = (visualizer_view_mode_t)mode_int;
        }

        // Freezes the data, not just the picture: see imgui_visualizer_get_frozen.
        // Lit while frozen, the same way a selected stream button is. Only
        // starting a freeze can be unavailable; releasing one never is.
        const bool frozen = state->frozen;
        const bool freeze_available = frozen || freeze_can_start(visualizer);
        if (!freeze_available) {
            ImGui::BeginDisabled();
        }
        if (frozen) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.95f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.64f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.40f, 0.80f, 1.00f));
        }
        // Fixed ID after ###, so the label can change without it becoming a
        // different widget mid-click.
        const bool freeze_clicked = ImGui::Button(frozen ? "Unfreeze###freeze" : "Freeze###freeze", ImVec2(90.0f, 0.0f));
        if (frozen) {
            ImGui::PopStyleColor(3);
        }
        if (!freeze_available) {
            ImGui::EndDisabled();
        }
        if (freeze_clicked) {
            toggle_freeze(state, visualizer);
        }
        ImGui::SameLine();
        if (!freeze_available) {
            ImGui::TextDisabled("off while the trigger holds the view");
        } else if (state->frozen) {
            ImGui::TextColored(ImVec4(0.47f, 0.90f, 1.00f, 1.0f), "Memory frozen - capture carries on");
        } else {
            ImGui::TextDisabled("or press Space");
        }

        ImGui::End();

        draw_zoom_window(visualizer);

        if (view_mode == VIEW_MODE_RADAR) {
            draw_radar_window(radar, visualizer, radar_dl, radar_ul,
                              have_radar_pair, radar_dl_eaxc, radar_ul_eaxc);
        } else if (view_mode == VIEW_MODE_WATERFALL) {
            ImGui::Begin("Waterfall Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            // Read outside both sections: Trigger notes when it is judging
            // averaged rows, which has to hold even with this section folded.
            bool persist_enabled = visualizer_get_persistence_enabled(visualizer) != 0;

            if (section_header("Rolling mean", persist_enabled)) {
                if (ImGui::Checkbox("Rolling mean (persistence)", &persist_enabled)) {
                    visualizer_set_persistence_enabled(visualizer, persist_enabled ? 1 : 0);
                }

                int period = visualizer_get_persistence_period(visualizer);
                if (ImGui::InputInt("Averaging period (symbols)", &period)) {
                    visualizer_set_persistence_period(visualizer, period);
                }

                int n = visualizer_get_persistence_n(visualizer);
                if (ImGui::InputInt("n (periods to average)", &n)) {
                    visualizer_set_persistence_n(visualizer, n);
                }

                if (ImGui::Button("Reset")) {
                    visualizer_reset_persistence(visualizer);
                }

                ImGui::TextDisabled("Tip: multiples of 14 align to slot boundaries");
                ImGui::TextDisabled("(also multiples of slots/subframe*14, or *10 for frames)");
            }

            if (section_header("Trigger", visualizer_get_trigger_mode(visualizer) != TRIGGER_OFF)) {
                if (persist_enabled) {
                    ImGui::TextDisabled("Rolling mean is on: triggering on averaged rows");
                }

                visualizer_trigger_mode_t trigger_mode = visualizer_get_trigger_mode(visualizer);
                int trigger_mode_int = (int)trigger_mode;
                ImGui::RadioButton("Off", &trigger_mode_int, TRIGGER_OFF);
                ImGui::SameLine();
                ImGui::RadioButton("Repeat", &trigger_mode_int, TRIGGER_REPEAT);
                ImGui::SameLine();
                ImGui::RadioButton("Single", &trigger_mode_int, TRIGGER_SINGLE);
                if (trigger_mode_int != (int)trigger_mode) {
                    visualizer_set_trigger_mode(visualizer, (visualizer_trigger_mode_t)trigger_mode_int);
                    trigger_mode = (visualizer_trigger_mode_t)trigger_mode_int;
                    // A trigger and a freeze would both be deciding what the
                    // waterfall holds; the trigger, just asked for, wins.
                    if (trigger_mode != TRIGGER_OFF) {
                        state->frozen = false;
                    }
                }

                visualizer_trigger_edge_t trigger_edge = visualizer_get_trigger_edge(visualizer);
                int trigger_edge_int = (int)trigger_edge;
                ImGui::RadioButton("Rising edge", &trigger_edge_int, TRIGGER_EDGE_RISING);
                ImGui::SameLine();
                ImGui::RadioButton("Falling edge", &trigger_edge_int, TRIGGER_EDGE_FALLING);
                if (trigger_edge_int != (int)trigger_edge) {
                    visualizer_set_trigger_edge(visualizer, (visualizer_trigger_edge_t)trigger_edge_int);
                }

                float trigger_level_pct = visualizer_get_trigger_level_pct(visualizer);
                if (ImGui::InputFloat("Trigger level (% of baseline)", &trigger_level_pct, 5.0f, 25.0f, "%.1f")) {
                    visualizer_set_trigger_level_pct(visualizer, trigger_level_pct);
                }
                ImGui::TextDisabled("100%% = as strong as a typical recent symbol. Below 100%% fires when");
                ImGui::TextDisabled("the signal returns after a gap (start of a TDD burst); above 100%%");
                ImGui::TextDisabled("only on unusually strong ones. Raise it far past the live peak");
                ImGui::TextDisabled("below to stop firing entirely.");

                // Reads back off in Single, which has no use for it, and cannot be
                // ticked there; the Repeat choice returns with Repeat.
                const bool single_mode = (trigger_mode == TRIGGER_SINGLE);
                bool hysteresis_enabled = visualizer_get_trigger_hysteresis_enabled(visualizer) != 0;
                if (single_mode) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Checkbox("Hysteresis", &hysteresis_enabled)) {
                    visualizer_set_trigger_hysteresis_enabled(visualizer, hysteresis_enabled ? 1 : 0);
                }
                if (single_mode) {
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("not used in Single mode");
                }
                if (!hysteresis_enabled) {
                    ImGui::BeginDisabled();
                }
                float trigger_reset_level_pct = visualizer_get_trigger_reset_level_pct(visualizer);
                if (ImGui::InputFloat("Reset level (% of baseline)", &trigger_reset_level_pct, 5.0f, 25.0f, "%.1f")) {
                    visualizer_set_trigger_reset_level_pct(visualizer, trigger_reset_level_pct);
                }
                if (!hysteresis_enabled) {
                    ImGui::EndDisabled();
                }
                ImGui::TextDisabled("Must clear this level before a new crossing counts as fresh -");
                ImGui::TextDisabled("keep it below Trigger level for rising edge, above for falling.");

                int trigger_freq_start = visualizer_get_trigger_freq_start(visualizer);
                int trigger_freq_end = visualizer_get_trigger_freq_end(visualizer);
                bool freq_changed = false;
                freq_changed |= ImGui::InputInt("Trigger freq. start (RE index)", &trigger_freq_start);
                freq_changed |= ImGui::InputInt("Trigger freq. end (RE index)", &trigger_freq_end);
                if (freq_changed) {
                    visualizer_set_trigger_freq_range(visualizer, trigger_freq_start, trigger_freq_end);
                }
                ImGui::TextDisabled("Restricts where triggering searches (0-3275); does not");
                ImGui::TextDisabled("affect what's displayed once triggered.");

                int trigger_capture_depth = visualizer_get_trigger_capture_depth(visualizer);
                if (ImGui::InputInt("Trigger capture depth (symbols)", &trigger_capture_depth)) {
                    visualizer_set_trigger_capture_depth(visualizer, trigger_capture_depth);
                }
                ImGui::TextDisabled("Rows used by a capture (half before/half after the trigger) -");
                ImGui::TextDisabled("keep well under Memory size, or the post-trigger wait will");
                ImGui::TextDisabled("evict the pre-trigger half before it completes.");

                if (trigger_mode == TRIGGER_OFF) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Re-arm")) {
                    visualizer_trigger_rearm(visualizer);
                }
                if (trigger_mode == TRIGGER_OFF) {
                    ImGui::EndDisabled();
                }

                const char* status_text = "Off";
                ImVec4 status_color(0.7f, 0.7f, 0.7f, 1.0f);
                switch (visualizer_get_trigger_status(visualizer)) {
                    case TRIGGER_STATUS_ARMED:
                        status_text = "Searching (armed)";
                        status_color = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
                        break;
                    case TRIGGER_STATUS_WAIT_RESET:
                        status_text = "Not searching - waiting for signal to cross back";
                        status_color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
                        break;
                    case TRIGGER_STATUS_CAPTURING:
                        status_text = "Capturing (filling post-trigger half)";
                        status_color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
                        break;
                    case TRIGGER_STATUS_HOLDING:
                        status_text = "Holding - press Re-arm to search again";
                        status_color = ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
                        break;
                    case TRIGGER_STATUS_WARMING_UP:
                        status_text = "Warming up the mean before searching";
                        status_color = ImVec4(0.6f, 0.7f, 1.0f, 1.0f);
                        break;
                    case TRIGGER_STATUS_OFF:
                    default:
                        break;
                }
                ImGui::TextColored(status_color, "Status: %s", status_text);
                ImGui::Text("Captures: %lu", visualizer_get_trigger_fire_count(visualizer));

                float live_peak = 0.0f, live_baseline = 0.0f, live_threshold = 0.0f;
                visualizer_get_trigger_levels(visualizer, &live_peak, &live_baseline, &live_threshold);
                ImGui::Text("Live peak: %.1f  (%.0f%% of baseline)", live_peak,
                            (live_baseline > 1e-6f) ? (100.0f * live_peak / live_baseline) : 0.0f);
                ImGui::Text("Baseline: %.1f   Fire threshold: %.1f", live_baseline, live_threshold);

                int mean_periods = 0, mean_required = 0;
                if (visualizer_get_trigger_mean_progress(visualizer, &mean_periods, &mean_required)) {
                    ImGui::Text("Mean periods: %d / %d", mean_periods, mean_required);
                }
            }

            ImGui::End();
        } else {
            ImGui::Begin("Curve Persistence", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            if (section_header("Trailing traces", visualizer_get_curve_trace_enabled(visualizer) != 0)) {
                bool trace_enabled = visualizer_get_curve_trace_enabled(visualizer) != 0;
                if (ImGui::Checkbox("Persistence (trailing traces)", &trace_enabled)) {
                    visualizer_set_curve_trace_enabled(visualizer, trace_enabled ? 1 : 0);
                }

                if (!trace_enabled) {
                    ImGui::BeginDisabled();
                }
                int trace_count = visualizer_get_curve_trace_count(visualizer);
                if (ImGui::InputInt("Trace count", &trace_count)) {
                    visualizer_set_curve_trace_count(visualizer, trace_count);
                }
                if (!trace_enabled) {
                    ImGui::EndDisabled();
                }
            }

            if (section_header("Mean (rolling average)", visualizer_get_curve_mean_enabled(visualizer) != 0)) {
                bool mean_enabled = visualizer_get_curve_mean_enabled(visualizer) != 0;
                if (ImGui::Checkbox("Show mean curve", &mean_enabled)) {
                    visualizer_set_curve_mean_enabled(visualizer, mean_enabled ? 1 : 0);
                }

                if (!mean_enabled) {
                    ImGui::BeginDisabled();
                }

                visualizer_curve_mean_mode_t mean_mode = visualizer_get_curve_mean_mode(visualizer);
                int mean_mode_int = (int)mean_mode;
                const char* mean_mode_labels[] = { "Phase-locked", "Running average" };
                if (ImGui::Combo("Mode", &mean_mode_int, mean_mode_labels, IM_ARRAYSIZE(mean_mode_labels))) {
                    visualizer_set_curve_mean_mode(visualizer, (visualizer_curve_mean_mode_t)mean_mode_int);
                    mean_mode = (visualizer_curve_mean_mode_t)mean_mode_int;
                }

                if (mean_mode == CURVE_MEAN_MODE_PHASE_LOCKED) {
                    int mean_period = visualizer_get_curve_mean_period(visualizer);
                    if (ImGui::InputInt("Averaging period (symbols)", &mean_period)) {
                        visualizer_set_curve_mean_period(visualizer, mean_period);
                    }
                }

                int mean_n = visualizer_get_curve_mean_n(visualizer);
                if (ImGui::InputInt("n (periods to average)", &mean_n)) {
                    visualizer_set_curve_mean_n(visualizer, mean_n);
                }

                if (ImGui::Button("Reset")) {
                    visualizer_reset_curve_mean(visualizer);
                }

                if (!mean_enabled) {
                    ImGui::EndDisabled();
                }

                ImGui::TextDisabled("Tip: multiples of 14 align to slot boundaries");
            }

            ImGui::End();
        }
    }

    // Marker list. Values are re-sampled by the visualizer every frame, so
    // this only edits positions and reads back what it measured.
    if (visualizer != nullptr) {
        ImGui::Begin("Markers", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        if (section_header("Marker list", visualizer_get_marker_count(visualizer) > 0)) {
            ImGui::Text("Offset from: %s", visualizer_get_marker_reference_label(visualizer));
            ImGui::TextDisabled("Positive offset = newer (further down)");

            const int marker_count = visualizer_get_marker_count(visualizer);
            if (ImGui::Button("Add marker")) {
                const int added = visualizer_add_marker(visualizer);
                if (added >= 0) {
                    state->selected_marker = added;
                }
            }
            ImGui::SameLine();

            const bool can_remove = state->selected_marker >= 0 && state->selected_marker < marker_count;
            if (!can_remove) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Remove selected")) {
                visualizer_remove_marker(visualizer, state->selected_marker);
                const int remaining = visualizer_get_marker_count(visualizer);
                if (state->selected_marker >= remaining) {
                    state->selected_marker = remaining - 1;
                }
            }
            if (!can_remove) {
                ImGui::EndDisabled();
            }

            if (marker_count == 0) {
                ImGui::TextDisabled("No markers yet");
            } else if (ImGui::BeginTable("marker_table", 5,
                                         ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_BordersInnerV)) {
                // Column and value headings follow whatever the view's axes
                // measure, so a marker over a delay grid is never labelled as if it
                // sat on the resource grid.
                visualizer_grid_info_t mgrid;
                visualizer_get_grid_info(visualizer, &mgrid);
                const bool marker_delay_x = (mgrid.x_unit == AXIS_UNIT_DELAY_TAP);
                const bool marker_doppler_y = (mgrid.y_unit == AXIS_UNIT_DOPPLER_BIN);

                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn(marker_delay_x ? "Tap" : "RE", ImGuiTableColumnFlags_WidthFixed, 76.0f);
                ImGui::TableSetupColumn(marker_doppler_y ? "Bin" : "Offset", ImGuiTableColumnFlags_WidthFixed, 76.0f);
                ImGui::TableSetupColumn(mgrid.value_label, ImGuiTableColumnFlags_WidthFixed, 76.0f);
                ImGui::TableSetupColumn("% base", ImGuiTableColumnFlags_WidthFixed, 76.0f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < marker_count; i++) {
                    visualizer_marker_info_t marker;
                    if (!visualizer_get_marker(visualizer, i, &marker)) {
                        continue;
                    }

                    ImGui::TableNextRow();
                    ImGui::PushID(i);

                    ImGui::TableSetColumnIndex(0);
                    char id_label[16];
                    snprintf(id_label, sizeof(id_label), "#%d", i + 1);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(marker.color[0] / 255.0f, marker.color[1] / 255.0f,
                                                 marker.color[2] / 255.0f, 1.0f));
                    if (ImGui::Selectable(id_label, state->selected_marker == i)) {
                        state->selected_marker = i;
                    }
                    ImGui::PopStyleColor();

                    ImGui::TableSetColumnIndex(1);
                    int re = marker.re;
                    ImGui::SetNextItemWidth(70.0f);
                    if (ImGui::InputInt("##re", &re, 0, 0)) {
                        visualizer_set_marker(visualizer, i, re, marker.time_offset);
                    }

                    ImGui::TableSetColumnIndex(2);
                    int offset = marker.time_offset;
                    ImGui::SetNextItemWidth(70.0f);
                    if (ImGui::InputInt("##offset", &offset, 0, 0)) {
                        visualizer_set_marker(visualizer, i, marker.re, offset);
                    }

                    ImGui::TableSetColumnIndex(3);
                    if (marker.has_value) {
                        ImGui::Text(value_format(marker.magnitude),
                                    static_cast<double>(marker.magnitude));
                    } else {
                        ImGui::TextDisabled("-");
                    }

                    ImGui::TableSetColumnIndex(4);
                    if (marker.has_value) {
                        ImGui::Text("%.1f%%", static_cast<double>(marker.percent_of_baseline));
                    } else {
                        ImGui::TextDisabled("-");
                    }

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            ImGui::TextDisabled("Baseline = mean magnitude currently on screen");
        }

        // Value at cursor: the same readout a marker gives, but following the
        // pointer instead of being pinned, for sweeping across a region without
        // placing anything. The checkbox cannot share the header's label, since
        // ImGui would give the two the same ID.
        if (section_header("Value at cursor", state->cursor_value_enabled)) {
            ImGui::Checkbox("Enabled", &state->cursor_value_enabled);
            if (!state->cursor_value_enabled) {
                ImGui::BeginDisabled();
            }
            ImGui::Indent();
            ImGui::Checkbox("Show next to cursor", &state->cursor_value_at_cursor);
            ImGui::Checkbox("Show here", &state->cursor_value_in_window);

            if (state->cursor_value_in_window) {
                // Fixed-size region: the readout appears and disappears as the
                // pointer moves on and off the plot, and this window auto-resizes,
                // so a free-flowing block would make it jump around underneath the
                // cursor. Four lines is the tallest the readout ever gets.
                const float readout_h = ImGui::GetTextLineHeightWithSpacing() * 4.0f;
                ImGui::BeginChild("cursor_readout", ImVec2(320.0f, readout_h), false);
                if (curve_view_active) {
                    ImGui::TextDisabled("Not available in Curve view: the");
                    ImGui::TextDisabled("vertical axis is magnitude, not time.");
                } else if (!cursor_sample.valid) {
                    ImGui::TextDisabled("Hover the plot to read a cell");
                } else {
                    visualizer_grid_info_t grid;
                    visualizer_get_grid_info(visualizer, &grid);
                    char col_text[48];
                    char row_text[48];
                    format_cell_column(grid, cursor_sample.re, col_text, sizeof(col_text));
                    format_cell_row(grid, cursor_sample, row_text, sizeof(row_text));

                    // A Doppler row is not an offset in time, so the offset column
                    // only appears where the vertical axis actually measures time.
                    if (grid.y_is_scrolling) {
                        ImGui::Text("%s, %s (offset %+d)", col_text, row_text, cursor_sample.time_offset);
                    } else {
                        ImGui::Text("%s, %s", col_text, row_text);
                    }
                    char timing_text[96];
                    format_cell_timing(cursor_sample, timing_text, sizeof(timing_text));
                    if (timing_text[0] != '\0') {
                        ImGui::TextUnformatted(timing_text);
                    }
                    if (cursor_sample.has_value) {
                        char value_text[64];
                        snprintf(value_text, sizeof(value_text), value_format(cursor_sample.magnitude),
                                 static_cast<double>(cursor_sample.magnitude));
                        ImGui::Text("%s %s", grid.value_label, value_text);
                        ImGui::Text("%.1f%% of baseline",
                                    static_cast<double>(cursor_sample.percent_of_baseline));
                    } else {
                        ImGui::TextDisabled("cell holds no data");
                    }
                }
                ImGui::EndChild();
            }
            ImGui::Unindent();
            if (!state->cursor_value_enabled) {
                ImGui::EndDisabled();
            }
        }

        ImGui::End();
    }

    // Axes and markers. Drawn on the background list so they sit over the
    // heatmap but under every panel, and entirely within the margins the
    // visualizer reserves, so they never cover data.
    if (visualizer != nullptr) {
        draw_plot_axes(visualizer, current_display_memory);
        draw_markers(visualizer);
        if (state->frozen) {
            draw_frozen_badge(visualizer);
        }
        if (state->cursor_value_enabled && state->cursor_value_at_cursor) {
            draw_cursor_value_overlay(visualizer, cursor_sample);
        }
    }

    // Pop the theme colors
    ImGui::PopStyleColor(9);

    // Finalize the frame so the caller can submit draw data after the heatmap pass.
    ImGui::Render();

    // Find the selected memory
    fh_oran_memory_t* shown = current_display_memory;
    if (num_unique_streams > 0 && state->selected_eaxc_id_index < num_unique_streams) {
        uint16_t selected_eaxc_id = streams[state->selected_eaxc_id_index].eaxc_id;
        uint8_t selected_direction = state->selected_data_direction;

        // Search for matching key
        fh_oran_memory_t* exact = nullptr;
        fh_oran_memory_t* same_eaxc_fallback = nullptr;
        for (size_t i = 0; i < num_keys; i++) {
            if (keys[i].eaxc_id != selected_eaxc_id) {
                continue;
            }
            if (memory_ptrs == nullptr || memory_ptrs[i] == nullptr) {
                continue;
            }
            if (keys[i].data_direction == selected_direction) {
                exact = memory_ptrs[i];
                break;
            }
            same_eaxc_fallback = memory_ptrs[i];
        }

        // The carried direction may not exist for this eAxC - easy to reach in
        // radar mode, where the eAxC is chosen by row and the direction is
        // implicit. Falling back to the other direction of the same eAxC keeps
        // the displayed stream related to what is selected; returning the
        // previous memory would hand back an unrelated eAxC, and the radar
        // reads its subcarrier spacing from whatever this resolves to.
        if (exact != nullptr) {
            shown = exact;
        } else if (same_eaxc_fallback != nullptr) {
            shown = same_eaxc_fallback;
        }
    }

    // After the view mode radio above, so a switch this frame already counts.
    const bool radar_view_now = visualizer != nullptr &&
                                visualizer_get_view_mode(visualizer) == VIEW_MODE_RADAR;

    publish_decode_selection(state->only_decode_selected, radar_view_now, have_radar_pair, radar_dl_eaxc,
                             radar_ul_eaxc, keys, num_keys, memory_ptrs, shown, decode_selection);

    return shown;
}

bool imgui_visualizer_get_frozen(const imgui_visualizer_state_t* state) {
    return state != nullptr && state->frozen;
}

void imgui_visualizer_present(void) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr) {
        return;
    }

    ImGui_ImplOpenGL2_RenderDrawData(draw_data);
}

void imgui_visualizer_shutdown(imgui_visualizer_state_t* state) {
    if (!state || !state->initialized) {
        return;
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    free(state);
}
