#include "imgui_visualizer_interface.h"
#include "core/c_oran_memory.h"
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
};

namespace {

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

    if (ImGui::Button("Reset to full view")) {
        visualizer_reset_zoom(visualizer);
    }
    ImGui::SameLine();
    if (visualizer_zoom_is_active(visualizer)) {
        float xs = 1.0f, ys = 1.0f;
        visualizer_get_zoom(visualizer, nullptr, &xs, nullptr, &ys);
        ImGui::Text("x %.1fx   y %.1fx", (double)(1.0f / xs), (double)(1.0f / ys));
    } else {
        ImGui::TextDisabled("full view");
    }

    ImGui::TextDisabled("Display only - memory size, averaging period and");
    ImGui::TextDisabled("capture depth are untouched by anything here.");

    const bool curve_view = visualizer_get_view_mode(visualizer) == VIEW_MODE_CURVE;
    const float x_total = (float)VISUALIZER_RE_COUNT;
    // Curve view plots magnitude vertically, so its window is a percentage of
    // full scale rather than a count of rows.
    const int total_rows = visualizer_get_zoom_total_rows(visualizer);
    const float y_total = curve_view ? 100.0f : (float)(total_rows > 0 ? total_rows : 1);

    ImGui::Separator();
    draw_zoom_minimap(visualizer);
    if (curve_view) {
        ImGui::TextDisabled("full extent: %d RE wide, magnitude tall", VISUALIZER_RE_COUNT);
    } else {
        ImGui::TextDisabled("full extent: %d RE wide, %d rows tall", VISUALIZER_RE_COUNT, total_rows);
    }

    zoom_window_t w = zoom_window_in_units(visualizer, x_total, y_total);
    bool changed = false;

    ImGui::Separator();
    ImGui::TextUnformatted("Frequency (horizontal)");

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
                        VISUALIZER_RE_COUNT / 12);

    ImGui::Separator();
    if (curve_view) {
        ImGui::TextUnformatted("Magnitude (vertical)");
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
        ImGui::TextUnformatted("Time (vertical)");
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

    if (changed) {
        zoom_apply_units(visualizer, w, x_total, y_total);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Wheel over the plot zooms both axes; Shift+wheel");
    ImGui::TextDisabled("only horizontally, Ctrl+wheel only vertically.");
    ImGui::TextDisabled("Drag the plot or use the arrow keys to move the window.");

    ImGui::End();
}

// --- Value at cursor ---

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
    char line3[64];
    if (sample.has_value) {
        snprintf(line1, sizeof(line1), "%.1f", static_cast<double>(sample.magnitude));
        snprintf(line2, sizeof(line2), "%.1f%% base", static_cast<double>(sample.percent_of_baseline));
    } else {
        snprintf(line1, sizeof(line1), "no data");
        line2[0] = '\0';
    }
    if (sample.has_phase) {
        snprintf(line3, sizeof(line3), "RE %d  phase +%d", sample.re, sample.phase);
    } else {
        snprintf(line3, sizeof(line3), "RE %d  sym %d", sample.re, sample.symbol);
    }

    const char* lines[3] = { line1, line2, line3 };
    float text_w = 0.0f;
    float text_h = 0.0f;
    for (int i = 0; i < 3; i++) {
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
    for (int i = 0; i < 3; i++) {
        if (lines[i][0] == '\0') {
            continue;
        }
        draw->AddText(ImVec2(box_x + pad, text_y), IM_COL32(235, 250, 255, 255), lines[i]);
        text_y += ImGui::CalcTextSize(lines[i]).y;
    }
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
    state->selected_marker = -1;
    state->cursor_value_enabled = false;
    state->cursor_value_at_cursor = true;
    state->cursor_value_in_window = true;
    state->frame_tracking_initialized = false;
    state->frame_tracking_memory = nullptr;
    state->last_absolute_frame = 0;
    state->frames_since_start = 0;

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
    opengl_visualizer_t* visualizer) {

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

    ImGui::TextUnformatted("Columns: eAxC_ID | UL | DL");
    ImGui::Spacing();

    if (ImGui::BeginTable("stream_table", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("eAxC_ID", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("UL", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("DL", ImGuiTableColumnFlags_WidthFixed, 48.0f);

        for (int i = 0; i < num_unique_streams; i++) {
            uint16_t eaxc_id = streams[i].eaxc_id;
            ImGui::PushID(i);

            const bool row_selected = (state->selected_eaxc_id_index == i);
            const bool ul_selected = row_selected && state->selected_data_direction == 0;
            const bool dl_selected = row_selected && state->selected_data_direction == 1;

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);

            // Format eAxC_ID for display
            char eaxc_label[16];
            snprintf(eaxc_label, sizeof(eaxc_label), "0x%04X", eaxc_id);

            bool eaxc_clicked = ImGui::Button(eaxc_label, ImVec2(80.0f, 0.0f));

            if (eaxc_clicked) {
                state->selected_eaxc_id_index = i;
                if (!streams[i].has_ul && streams[i].has_dl) {
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
                
                if (button_clicked) {
                    state->selected_eaxc_id_index = i;
                    state->selected_data_direction = direction;
                }
            };

            draw_direction_button("UL", 0, streams[i].has_ul, ul_selected);
            draw_direction_button("DL", 1, streams[i].has_dl, dl_selected);

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();

    // Status display
    if (state->selected_eaxc_id_index < num_unique_streams) {
        uint16_t selected_eaxc_id = streams[state->selected_eaxc_id_index].eaxc_id;
        const char* direction_str = data_direction_to_string(state->selected_data_direction);
        ImGui::Text("Selected: 0x%04X / %s", selected_eaxc_id, direction_str);
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

    ImGui::Begin("Live Statistics", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    if (ImGui::CollapsingHeader("Data Query", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Packet drop rate: %.2f%%", dropped_pct);
        ImGui::Text("Total packets: %llu", static_cast<unsigned long long>(stats.total_packets));
        ImGui::Text("Dropped packets: %llu", static_cast<unsigned long long>(stats.dropped_packets));
        ImGui::Text("Queued packets: %llu", static_cast<unsigned long long>(stats.queued_packets));
    }

    if (ImGui::CollapsingHeader("Timing Info", ImGuiTreeNodeFlags_DefaultOpen)) {
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
        if (mode_int != (int)view_mode) {
            visualizer_set_view_mode(visualizer, (visualizer_view_mode_t)mode_int);
            view_mode = (visualizer_view_mode_t)mode_int;
        }

        ImGui::End();

        draw_zoom_window(visualizer);

        if (view_mode == VIEW_MODE_WATERFALL) {
            ImGui::Begin("Waterfall Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            bool persist_enabled = visualizer_get_persistence_enabled(visualizer) != 0;
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

            ImGui::Separator();
            ImGui::TextUnformatted("Trigger");
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

            bool hysteresis_enabled = visualizer_get_trigger_hysteresis_enabled(visualizer) != 0;
            if (ImGui::Checkbox("Hysteresis", &hysteresis_enabled)) {
                visualizer_set_trigger_hysteresis_enabled(visualizer, hysteresis_enabled ? 1 : 0);
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

            ImGui::End();
        } else {
            ImGui::Begin("Curve Persistence", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            ImGui::TextUnformatted("Trailing traces");
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

            ImGui::Separator();
            ImGui::TextUnformatted("Mean (rolling average)");
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

            ImGui::End();
        }
    }

    // Marker list. Values are re-sampled by the visualizer every frame, so
    // this only edits positions and reads back what it measured.
    if (visualizer != nullptr) {
        ImGui::Begin("Markers", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

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
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("RE", ImGuiTableColumnFlags_WidthFixed, 76.0f);
            ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 76.0f);
            ImGui::TableSetupColumn("IQ", ImGuiTableColumnFlags_WidthFixed, 76.0f);
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
                    ImGui::Text("%.1f", static_cast<double>(marker.magnitude));
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

        // Value at cursor: the same readout a marker gives, but following the
        // pointer instead of being pinned, for sweeping across a region without
        // placing anything.
        ImGui::Separator();
        ImGui::Checkbox("Value at cursor", &state->cursor_value_enabled);
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
            // cursor. Three lines is the tallest the readout ever gets.
            const float readout_h = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
            ImGui::BeginChild("cursor_readout", ImVec2(320.0f, readout_h), false);
            if (curve_view_active) {
                ImGui::TextDisabled("Not available in Curve view: the");
                ImGui::TextDisabled("vertical axis is magnitude, not time.");
            } else if (!cursor_sample.valid) {
                ImGui::TextDisabled("Hover the plot to read a cell");
            } else {
                if (cursor_sample.has_phase) {
                    ImGui::Text("RE %d, phase +%d (offset %+d)",
                                cursor_sample.re, cursor_sample.phase, cursor_sample.time_offset);
                } else {
                    ImGui::Text("RE %d, symbol %d (offset %+d)",
                                cursor_sample.re, cursor_sample.symbol, cursor_sample.time_offset);
                }
                if (cursor_sample.has_value) {
                    ImGui::Text("IQ %.1f", static_cast<double>(cursor_sample.magnitude));
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

        ImGui::End();
    }

    // Axes and markers. Drawn on the background list so they sit over the
    // heatmap but under every panel, and entirely within the margins the
    // visualizer reserves, so they never cover data.
    if (visualizer != nullptr) {
        draw_plot_axes(visualizer, current_display_memory);
        draw_markers(visualizer);
        if (state->cursor_value_enabled && state->cursor_value_at_cursor) {
            draw_cursor_value_overlay(visualizer, cursor_sample);
        }
    }

    // Pop the theme colors
    ImGui::PopStyleColor(9);

    // Finalize the frame so the caller can submit draw data after the heatmap pass.
    ImGui::Render();

    // Find and return the selected memory
    if (num_unique_streams > 0 && state->selected_eaxc_id_index < num_unique_streams) {
        uint16_t selected_eaxc_id = streams[state->selected_eaxc_id_index].eaxc_id;
        uint8_t selected_direction = state->selected_data_direction;

        // Search for matching key
        for (size_t i = 0; i < num_keys; i++) {
            if (keys[i].eaxc_id == selected_eaxc_id && keys[i].data_direction == selected_direction) {
                if (memory_ptrs && memory_ptrs[i]) {
                    return memory_ptrs[i];
                }
            }
        }
    }
    
    return current_display_memory;
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
