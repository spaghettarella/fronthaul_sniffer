#include "gui/imgui_constellation_interface.h"
#include "gui/imgui_hint.h"
#include "gui/imgui_theme.h"
#include "core/c_oran_memory.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl2.h>
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

struct imgui_constellation_state {
    ImGuiContext* context;
    GLFWwindow* window;
    // Whether the view was zoomed when the Overview section last looked. The
    // section opens when a zoom takes the view in from full and closes when one
    // brings it back out; in between it stays however it was left, so it can
    // be opened by hand at full view.
    bool overview_zoomed;
};

namespace {

// Skip empty's tolerance: Double starts from one LSB of 16-bit IQ, full scale
// being 1, and Half drops to 0 once below the slider's own resolution, which a
// logarithmic slider takes from the format's decimals - "%.7f" here; a %g or
// %e format would leave it with none.
const float SKIP_TOLERANCE_FIRST_STEP = 1.0f / 32768.0f;
const float SKIP_TOLERANCE_MIN = 1e-7f;

// One wheel notch, and one arrow keypress as a fraction of the window - the
// main window's steps, so both zoom alike.
const float ZOOM_WHEEL_STEP = 1.25f;
const float ZOOM_PAN_STEP = 0.10f;

// Gridlines are no closer than this, in pixels.
const float AXIS_MIN_STEP_PX = 70.0f;

// --- Input routing ---
// The GLFW backend hands every event to whichever ImGui context is current,
// and glfwPollEvents runs with the main window's current. So this window's
// events come through callbacks of its own, which make this window's context
// current for the one call into the backend and then hand the previous one back.

struct context_scope {
    ImGuiContext* previous;
    explicit context_scope(ImGuiContext* ctx) : previous(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(ctx);
    }
    ~context_scope() { ImGui::SetCurrentContext(previous); }
    context_scope(const context_scope&) = delete;
    context_scope& operator=(const context_scope&) = delete;
};

imgui_constellation_state_t* state_for(GLFWwindow* window) {
    return static_cast<imgui_constellation_state_t*>(glfwGetWindowUserPointer(window));
}

void on_focus(GLFWwindow* window, int focused) {
    if (imgui_constellation_state_t* ui = state_for(window)) {
        context_scope scope(ui->context);
        ImGui_ImplGlfw_WindowFocusCallback(window, focused);
    }
}

void on_cursor_enter(GLFWwindow* window, int entered) {
    if (imgui_constellation_state_t* ui = state_for(window)) {
        context_scope scope(ui->context);
        ImGui_ImplGlfw_CursorEnterCallback(window, entered);
    }
}

void on_cursor_pos(GLFWwindow* window, double x, double y) {
    if (imgui_constellation_state_t* ui = state_for(window)) {
        context_scope scope(ui->context);
        ImGui_ImplGlfw_CursorPosCallback(window, x, y);
    }
}

void on_mouse_button(GLFWwindow* window, int button, int action, int mods) {
    if (imgui_constellation_state_t* ui = state_for(window)) {
        context_scope scope(ui->context);
        ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
    }
}

void on_scroll(GLFWwindow* window, double x, double y) {
    if (imgui_constellation_state_t* ui = state_for(window)) {
        context_scope scope(ui->context);
        ImGui_ImplGlfw_ScrollCallback(window, x, y);
    }
}

void on_key(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (imgui_constellation_state_t* ui = state_for(window)) {
        context_scope scope(ui->context);
        ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
    }
}

void on_char(GLFWwindow* window, unsigned int c) {
    if (imgui_constellation_state_t* ui = state_for(window)) {
        context_scope scope(ui->context);
        ImGui_ImplGlfw_CharCallback(window, c);
    }
}

void set_callbacks(GLFWwindow* window, bool install) {
    glfwSetWindowFocusCallback(window, install ? on_focus : nullptr);
    glfwSetCursorEnterCallback(window, install ? on_cursor_enter : nullptr);
    glfwSetCursorPosCallback(window, install ? on_cursor_pos : nullptr);
    glfwSetMouseButtonCallback(window, install ? on_mouse_button : nullptr);
    glfwSetScrollCallback(window, install ? on_scroll : nullptr);
    glfwSetKeyCallback(window, install ? on_key : nullptr);
    glfwSetCharCallback(window, install ? on_char : nullptr);
}

// --- Plot ---

bool point_in_plot(const iq_constellation_view_t& view, ImVec2 p) {
    return p.x >= view.plot_x && p.x <= view.plot_x + view.plot_w &&
           p.y >= view.plot_y && p.y <= view.plot_y + view.plot_h;
}

// Wheel zooms about the pointer, a left-drag that started on the plot pans,
// arrow keys pan - the main window's controls. Ignored while the panel has the
// mouse or the keyboard, so scrolling the panel never moves the plot.
void handle_input(iq_constellation_t* constellation, const iq_constellation_view_t& view) {
    ImGuiIO& io = ImGui::GetIO();

    // Gated on where the drag started, so one that runs out over the panel or
    // off the plot keeps going rather than stopping mid-gesture.
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        point_in_plot(view, io.MouseClickedPos[ImGuiMouseButton_Left]) && !ImGui::IsAnyItemActive()) {
        iq_constellation_pan_px(constellation, io.MouseDelta.x, io.MouseDelta.y);
    }

    if (!io.WantCaptureMouse && io.MouseWheel != 0.0f && point_in_plot(view, io.MousePos)) {
        iq_constellation_zoom_by(constellation, powf(ZOOM_WHEEL_STEP, io.MouseWheel), io.MousePos.x, io.MousePos.y);
    }

    if (!io.WantCaptureKeyboard) {
        float pan_u = 0.0f;
        float pan_v = 0.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) { pan_u -= ZOOM_PAN_STEP; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) { pan_u += ZOOM_PAN_STEP; }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) { pan_v -= ZOOM_PAN_STEP; }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) { pan_v += ZOOM_PAN_STEP; }
        if (pan_u != 0.0f || pan_v != 0.0f) {
            iq_constellation_pan_by(constellation, pan_u, pan_v);
        }
    }
}

// Smallest 1-2-5 step of at least want.
double nice_step(double want) {
    if (!(want > 0.0 && want < 1e30)) {
        return 1.0;
    }
    const double decade = pow(10.0, floor(log10(want)));
    static const double multiples[] = { 1.0, 2.0, 5.0 };
    for (double m : multiples) {
        if (m * decade >= want * (1.0 - 1e-9)) {
            return m * decade;
        }
    }
    return 10.0 * decade;
}

// Decimals that tell neighbouring ticks of this step apart.
int step_decimals(double step) {
    if (step >= 1.0) {
        return 0;
    }
    return (int)ceil(-log10(step) - 1e-9);
}

void format_tick(char* out, size_t size, long long k, double step, int decimals) {
    if (k == 0) {
        snprintf(out, size, "0");
    } else {
        snprintf(out, size, "%.*f", decimals, (double)k * step);
    }
}

// Gridlines on a 1-2-5 ladder, the I = 0 and Q = 0 axes brighter, tick values
// in the margins. I and Q share one scale, so one step serves both.
void draw_axes(const iq_constellation_view_t& view) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float x0 = view.plot_x;
    const float y0 = view.plot_y;
    const float x1 = view.plot_x + view.plot_w;
    const float y1 = view.plot_y + view.plot_h;
    const float i_span = view.i_max - view.i_min;
    if (!(i_span > 0.0f) || view.plot_w <= 1.0f || view.plot_h <= 1.0f) {
        return;
    }
    const double px_per_unit = (double)view.plot_w / (double)i_span;
    const double step = nice_step(AXIS_MIN_STEP_PX / px_per_unit);
    const int decimals = step_decimals(step);

    const ImU32 grid_color = IM_COL32(60, 60, 68, 255);
    const ImU32 zero_color = IM_COL32(150, 150, 158, 255);
    const ImU32 label_color = IM_COL32(230, 230, 230, 255);
    const ImU32 frame_color = IM_COL32(220, 220, 220, 255);
    const float line_h = ImGui::GetTextLineHeight();

    // Bounded, so a degenerate view can never spin out thousands of lines.
    const long long i_first = (long long)ceil((double)view.i_min / step);
    const long long i_last = (long long)floor((double)view.i_max / step);
    const long long q_first = (long long)ceil((double)view.q_min / step);
    const long long q_last = (long long)floor((double)view.q_max / step);
    if (i_last - i_first > 400 || q_last - q_first > 400) {
        return;
    }

    draw->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    for (long long k = i_first; k <= i_last; k++) {
        const float x = x0 + (float)(((double)k * step - (double)view.i_min) * px_per_unit);
        draw->AddLine(ImVec2(x, y0), ImVec2(x, y1), k == 0 ? zero_color : grid_color);
    }
    for (long long k = q_first; k <= q_last; k++) {
        const float y = y0 + (float)(((double)view.q_max - (double)k * step) * px_per_unit);
        draw->AddLine(ImVec2(x0, y), ImVec2(x1, y), k == 0 ? zero_color : grid_color);
    }
    draw->PopClipRect();

    draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), frame_color);

    char text[32];
    for (long long k = i_first; k <= i_last; k++) {
        const float x = x0 + (float)(((double)k * step - (double)view.i_min) * px_per_unit);
        format_tick(text, sizeof(text), k, step, decimals);
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddLine(ImVec2(x, y1), ImVec2(x, y1 + 5.0f), frame_color);
        draw->AddText(ImVec2(x - size.x * 0.5f, y1 + 6.0f), label_color, text);
    }
    for (long long k = q_first; k <= q_last; k++) {
        const float y = y0 + (float)(((double)view.q_max - (double)k * step) * px_per_unit);
        format_tick(text, sizeof(text), k, step, decimals);
        const ImVec2 size = ImGui::CalcTextSize(text);
        float text_x = x0 - 8.0f - size.x;
        if (text_x < 2.0f) {
            text_x = 2.0f;
        }
        draw->AddLine(ImVec2(x0 - 5.0f, y), ImVec2(x0, y), frame_color);
        draw->AddText(ImVec2(text_x, y - line_h * 0.5f), label_color, text);
    }

    const ImVec2 i_size = ImGui::CalcTextSize("I");
    draw->AddText(ImVec2(x1 - i_size.x, y1 + 6.0f + line_h), label_color, "I");
    draw->AddText(ImVec2(4.0f, y0 - 2.0f), label_color, "Q");
}

// A notice centred on the plot, for when there is nothing to draw.
void draw_plot_notice(const iq_constellation_view_t& view, const char* text) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(view.plot_x + (view.plot_w - size.x) * 0.5f, view.plot_y + (view.plot_h - size.y) * 0.5f),
                  IM_COL32(200, 200, 200, 255), text);
}

// The main window's frozen badge, top right of the plot.
void draw_frozen_badge(const iq_constellation_view_t& view) {
    const char* text = "FROZEN";
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float pad = 4.0f;
    const float inset = 8.0f;
    const ImVec2 box_max(view.plot_x + view.plot_w - inset, view.plot_y + inset + size.y + pad * 2.0f);
    const ImVec2 box_min(box_max.x - size.x - pad * 2.0f, view.plot_y + inset);

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(box_min, box_max, IM_COL32(20, 28, 34, 235), 3.0f);
    draw->AddRect(box_min, box_max, IM_COL32(120, 230, 255, 255), 3.0f, 0, 1.5f);
    draw->AddText(ImVec2(box_min.x + pad, box_min.y + pad), IM_COL32(235, 250, 255, 255), text);
}

// Crosshair and the I/Q under the pointer, in the main window's cursor-readout
// style, pinned to the plot's bottom-left corner so it never covers what is
// being pointed at.
void draw_cursor_readout(const iq_constellation_t* constellation, const iq_constellation_view_t& view) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse || !point_in_plot(view, io.MousePos)) {
        return;
    }

    float i_val = 0.0f;
    float q_val = 0.0f;
    iq_constellation_screen_to_iq(constellation, io.MousePos.x, io.MousePos.y, &i_val, &q_val);
    const double magnitude = hypot((double)i_val, (double)q_val);
    const double phase_deg = atan2((double)q_val, (double)i_val) * 180.0 / 3.14159265358979323846;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 line_color = IM_COL32(120, 230, 255, 140);
    draw->AddLine(ImVec2(view.plot_x, io.MousePos.y), ImVec2(view.plot_x + view.plot_w, io.MousePos.y), line_color);
    draw->AddLine(ImVec2(io.MousePos.x, view.plot_y), ImVec2(io.MousePos.x, view.plot_y + view.plot_h), line_color);

    char text[128];
    snprintf(text, sizeof(text), "I %+.4g   Q %+.4g   |z| %.4g   %+.1f deg",
             (double)i_val, (double)q_val, magnitude, phase_deg);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float pad = 4.0f;
    const ImVec2 box_min(view.plot_x + 6.0f, view.plot_y + view.plot_h - 6.0f - size.y - pad * 2.0f);
    const ImVec2 box_max(box_min.x + size.x + pad * 2.0f, box_min.y + size.y + pad * 2.0f);
    draw->AddRectFilled(box_min, box_max, IM_COL32(20, 28, 34, 235), 3.0f);
    draw->AddRect(box_min, box_max, IM_COL32(120, 230, 255, 255), 3.0f, 0, 1.5f);
    draw->AddText(ImVec2(box_min.x + pad, box_min.y + pad), IM_COL32(235, 250, 255, 255), text);
}

// --- Panel ---

bool section_header(const char* label, bool open_at_start) {
    return ImGui::CollapsingHeader(label, open_at_start ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

// The overview map: the whole full view, with the part on screen marked in the
// waterfall zoom window's colours. A click or drag in it centres the view
// there.
void draw_minimap(iq_constellation_t* constellation, const iq_constellation_view_t& view) {
    const float size = (float)iq_constellation_map_size();
    // A deep zoom would otherwise shrink the marker to nothing.
    const float MINIMAP_MIN_PX = 3.0f;

    ImGui::InvisibleButton("##constellation_map", ImVec2(size, size));
    const bool dragging = ImGui::IsItemActive();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
    const ImTextureID texture = (ImTextureID)(intptr_t)iq_constellation_map_texture(constellation);
    dl->AddImage(texture, p0, p1);

    const float mid_x = (p0.x + p1.x) * 0.5f;
    const float mid_y = (p0.y + p1.y) * 0.5f;
    const ImU32 axis = IM_COL32(110, 110, 120, 110);
    dl->AddLine(ImVec2(mid_x, p0.y), ImVec2(mid_x, p1.y), axis);
    dl->AddLine(ImVec2(p0.x, mid_y), ImVec2(p1.x, mid_y), axis);

    // What the plot shows, which on a window that is not square is wider than
    // the zoom window itself along the longer side.
    if (view.extent > 0.0f) {
        const float to_u = 0.5f / view.extent;
        float wx0 = p0.x + (0.5f + view.i_min * to_u) * size;
        float wx1 = p0.x + (0.5f + view.i_max * to_u) * size;
        float wy0 = p0.y + (0.5f - view.q_max * to_u) * size;
        float wy1 = p0.y + (0.5f - view.q_min * to_u) * size;
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
        dl->PushClipRect(p0, p1, true);
        dl->AddRectFilled(ImVec2(wx0, wy0), ImVec2(wx1, wy1), IM_COL32(255, 170, 40, 60));
        dl->AddRect(ImVec2(wx0, wy0), ImVec2(wx1, wy1), IM_COL32(255, 190, 70, 255));
        dl->PopClipRect();
    }

    // Outer frame last so it stays crisp over the marker at full view.
    dl->AddRect(p0, p1, IM_COL32(150, 150, 158, 255));

    if (dragging) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        iq_constellation_set_center(constellation, (mouse.x - p0.x) / size, (mouse.y - p0.y) / size);
    }
}

// "3.41 M" rather than seven digits that change every frame.
void format_count(char* out, size_t size, long long n) {
    if (n >= 1000000LL) {
        snprintf(out, size, "%.2f M", (double)n / 1.0e6);
    } else if (n >= 10000LL) {
        snprintf(out, size, "%.1f k", (double)n / 1.0e3);
    } else {
        snprintf(out, size, "%lld", n);
    }
}

// Names what the window plots, for its title and the panel: the stream on
// screen, or in the Radar view H of the pair or one of its directions.
void describe_source(const iq_constellation_t* constellation,
                     const iq_constellation_radar_pair_t* radar,
                     const visualizer_complex_mean_t* mean,
                     const char* stream_label,
                     char* out,
                     size_t size) {
    // Says so when the points are averages rather than samples.
    const char* mean_suffix = (iq_constellation_get_use_mean(constellation) &&
                               iq_constellation_mean_available(constellation, mean, radar))
                                  ? " (rolling mean)"
                                  : "";
    if (radar == nullptr) {
        snprintf(out, size, "%s%s", stream_label != nullptr ? stream_label : "",
                 (stream_label != nullptr && stream_label[0] != '\0') ? mean_suffix : "");
        return;
    }
    const char* dl = (radar->dl != nullptr && radar->dl_label != nullptr) ? radar->dl_label : "not captured";
    const char* ul = (radar->ul != nullptr && radar->ul_label != nullptr) ? radar->ul_label : "not captured";
    if (iq_constellation_get_radar_data(constellation) == IQ_CONSTELLATION_RADAR_CFR) {
        snprintf(out, size, "%s, DL %s / UL %s%s",
                 radar->config.division == RADAR_DIV_UL_OVER_DL ? "H = UL/DL" : "1/H = DL/UL", dl, ul, mean_suffix);
    } else if (iq_constellation_get_radar_direction(constellation) == IQ_CONSTELLATION_UL) {
        snprintf(out, size, "%s UL", ul);
    } else {
        snprintf(out, size, "%s DL", dl);
    }
}

// The Radar view's own choices: H of the pair, or the IQ of one direction.
void draw_radar_choices(iq_constellation_t* constellation, const iq_constellation_radar_pair_t* radar) {
    int data = (int)iq_constellation_get_radar_data(constellation);
    ImGui::RadioButton("CFR H (complex)", &data, IQ_CONSTELLATION_RADAR_CFR);
    ImGui::SameLine();
    gui_hint(radar->config.division == RADAR_DIV_UL_OVER_DL ? "H = UL / DL" : "1/H = DL / UL",
             "Divides the pair the way the Radar view does - its lag, division and frequency range - and plots "
             "the complex result per resource element, unwindowed: the CFR view's |H| with its phase kept. Only "
             "symbols that pair and resource elements with a nonzero denominator give a value. With the right "
             "pairing the points gather around |H| = 1; a delay turns into a phase ramp across the band, which "
             "spreads them around a circle.");
    ImGui::RadioButton("IQ samples", &data, IQ_CONSTELLATION_RADAR_IQ);
    ImGui::SameLine();
    gui_hint("one direction", "The raw IQ of one direction of the radar pair, as the other views plot a stream.");
    if (data != (int)iq_constellation_get_radar_data(constellation)) {
        iq_constellation_set_radar_data(constellation, (iq_constellation_radar_data_t)data);
    }

    if (data == IQ_CONSTELLATION_RADAR_IQ) {
        ImGui::Indent();
        int direction = (int)iq_constellation_get_radar_direction(constellation);
        ImGui::RadioButton("DL##iq_direction", &direction, IQ_CONSTELLATION_DL);
        ImGui::SameLine();
        ImGui::RadioButton("UL##iq_direction", &direction, IQ_CONSTELLATION_UL);
        ImGui::SameLine();
        gui_hint("UL moved by the lag",
                 "The pair picked in Stream Selection, whose two directions may come from different eAxCs. With "
                 "Zoomed area, the Radar view's rows are DL symbols, so the UL shown is the one the radar pairs "
                 "with them: lag symbols later.");
        if (direction != (int)iq_constellation_get_radar_direction(constellation)) {
            iq_constellation_set_radar_direction(constellation, (iq_constellation_direction_t)direction);
        }
        ImGui::Unindent();
    }
}

// Rolling mean data, usable only while the main view shows a rolling mean of
// what the other settings ask for. While it is not, the box reads unticked and
// cannot be changed, but the choice is kept for when it is.
void draw_mean_choice(iq_constellation_t* constellation,
                      const iq_constellation_radar_pair_t* radar,
                      const visualizer_complex_mean_t* mean) {
    const bool available = iq_constellation_mean_available(constellation, mean, radar) != 0;
    bool use_mean = available && iq_constellation_get_use_mean(constellation) != 0;
    if (!available) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("Rolling mean data", &use_mean)) {
        iq_constellation_set_use_mean(constellation, use_mean ? 1 : 0);
    }
    if (!available) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const char* short_text = "complex mean per cell";
    if (!available) {
        short_text = (radar != nullptr && iq_constellation_get_radar_data(constellation) == IQ_CONSTELLATION_RADAR_IQ)
                         ? "H only in Radar view"
                         : "needs the main rolling mean on";
    }
    gui_hint(short_text,
             "Plots the main window's rolling mean instead of single symbols: one point per cell - offset in the "
             "period by resource element - holding the complex average of what was folded there. Same period, n "
             "and resets as the mean on screen, which averages magnitudes instead. A coherent mean of data REs "
             "tends to zero, since random QAM symbols cancel; only what repeats every period stays, such as "
             "reference signals. For H it is coherent integration: noise falls, a stable channel stays, and phase "
             "drift pulls the points in toward the origin. Available while the main window shows a rolling mean: "
             "Waterfall, Curve's mean curve, or in the Radar view the CFR mean with H selected here.");
}

// Skip empty REs and its tolerance, set the way the PCAP speed is: a
// logarithmic slider, Half and Double, and Ctrl+click to type a value.
void draw_skip_empty(iq_constellation_t* constellation) {
    bool skip_empty = iq_constellation_get_skip_empty(constellation) != 0;
    if (ImGui::Checkbox("Skip empty REs", &skip_empty)) {
        iq_constellation_set_skip_empty(constellation, skip_empty ? 1 : 0);
    }
    ImGui::SameLine();
    gui_hint("|I| and |Q| within tolerance",
             "Leaves out values whose |I| and |Q| are both at most the tolerance below; at 0 only exact zeros go. "
             "Resource elements nothing was scheduled on decode as 0+0j; left in, they pile into the one pixel at "
             "the origin and flatten the colour scale for everything else. A small tolerance also takes out "
             "near-empty ones, such as idle REs carrying only quantisation noise. For H, an RE empty in the "
             "dividing direction gives no estimate at all, and one empty in the other gives exactly 0.");

    if (!skip_empty) {
        ImGui::BeginDisabled();
    }
    ImGui::Indent();
    float tolerance = iq_constellation_get_skip_tolerance(constellation);
    bool changed = false;
    ImGui::SetNextItemWidth(200.0f);
    changed |= ImGui::SliderFloat("Tolerance", &tolerance, 0.0f, IQ_CONSTELLATION_SKIP_TOLERANCE_MAX, "%.7f",
                                  ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::Button("Half##skip_tolerance")) {
        tolerance *= 0.5f;
        if (tolerance < SKIP_TOLERANCE_MIN) {
            tolerance = 0.0f;
        }
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Double##skip_tolerance")) {
        tolerance = (tolerance > 0.0f) ? tolerance * 2.0f : SKIP_TOLERANCE_FIRST_STEP;
        changed = true;
    }
    ImGui::SameLine();
    gui_hint("Ctrl+click to type",
             "Ctrl+click the slider to type an exact value; Half and Double step it by factors of two, Double "
             "starting from one LSB of 16-bit IQ (2^-15 = 0.0000305 of full scale). One value for I and Q, in the "
             "units plotted: IQ full scale and |H| = 1 are both about 1.");
    if (changed) {
        iq_constellation_set_skip_tolerance(constellation, tolerance);
    }
    ImGui::Unindent();
    if (!skip_empty) {
        ImGui::EndDisabled();
    }
}

void draw_panel(imgui_constellation_state_t* ui,
                iq_constellation_t* constellation,
                const iq_constellation_view_t& view,
                const char* source_label,
                const iq_constellation_radar_pair_t* radar,
                const visualizer_complex_mean_t* mean) {
    const ImVec4 warn_color(1.0f, 0.45f, 0.35f, 1.0f);

    gui_push_theme();
    ImGui::SetNextWindowPos(ImVec2(view.plot_x + 10.0f, view.plot_y + 10.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Constellation", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    // The action and the readout stay above the sections, as in the main Zoom
    // window, so it says whether a zoom is active even with everything folded.
    if (ImGui::Button("Reset to full view")) {
        iq_constellation_reset_zoom(constellation);
    }
    ImGui::SameLine();
    if (iq_constellation_zoom_is_active(constellation)) {
        float span = 1.0f;
        iq_constellation_get_zoom(constellation, nullptr, nullptr, &span);
        ImGui::Text("%.1fx", (double)(1.0f / span));
    } else {
        ImGui::TextDisabled("full view");
    }

    // Opened and closed only as a zoom crosses full view - the wheel, or Reset
    // just above - never held to the zoom state, so it can be opened by hand
    // at full view and closed by hand while zoomed. Starts closed, at full view.
    const bool zoomed = iq_constellation_zoom_is_active(constellation) != 0;
    if (zoomed != ui->overview_zoomed) {
        ImGui::SetNextItemOpen(zoomed, ImGuiCond_Always);
        ui->overview_zoomed = zoomed;
    }
    if (section_header("Overview", false)) {
        draw_minimap(constellation, view);
        gui_hint("click or drag to move",
                 "The whole full view, with the part on screen marked. Click or drag in it to move there. The full "
                 "view is fitted to the data: the largest |I| or |Q| plotted plus a margin, fitted again when the "
                 "signal outgrows it or shrinks to under half of it, when the stream or the radar choice changes, "
                 "and on Reset.");
        if (view.extent_fitted) {
            ImGui::Text("Full view: +/- %.4g", (double)view.extent);
        } else {
            ImGui::Text("Full view: +/- 1 (no data yet)");
        }
    }

    iq_constellation_stats_t st;
    iq_constellation_get_stats(constellation, &st);

    if (section_header("Data", true)) {
        if (radar != nullptr) {
            draw_radar_choices(constellation, radar);
        }
        ImGui::Text("%s: %s", radar != nullptr ? "Plotting" : "Stream",
                    (source_label != nullptr && source_label[0] != '\0') ? source_label : "none yet");

        int source = (int)iq_constellation_get_source(constellation);
        ImGui::RadioButton("All data in memory", &source, IQ_CONSTELLATION_SOURCE_ALL);
        ImGui::SameLine();
        gui_hint("whole memory",
                 "Every symbol the stream's memory holds, across all 3276 resource elements. How far back that "
                 "reaches is the Memory size set at startup.");
        ImGui::RadioButton("Zoomed area of main view", &source, IQ_CONSTELLATION_SOURCE_ZOOMED);
        ImGui::SameLine();
        gui_hint("follows main zoom",
                 "Only what the main window's zoom covers. Resource elements narrow wherever its horizontal axis "
                 "spans them (Waterfall, Curve, Radar CFR). Symbols narrow where its vertical axis is time; with "
                 "the rolling mean the rows are offsets in the period, so every symbol at those offsets counts. A "
                 "triggered capture's symbols are read back from memory while they are still in it. Curve and "
                 "Range-Doppler have no time axis, so all symbols count there. In the Radar view the rows are DL "
                 "symbols; H takes them with their UL partners, and the UL alone is moved by the lag.");
        if (source != (int)iq_constellation_get_source(constellation)) {
            iq_constellation_set_source(constellation, (iq_constellation_source_t)source);
        }

        draw_mean_choice(constellation, radar, mean);

        if (st.missing == IQ_CONSTELLATION_MISSING_DL || st.missing == IQ_CONSTELLATION_MISSING_UL) {
            ImGui::TextColored(warn_color, "The pair's %s has not been captured%s",
                               st.missing == IQ_CONSTELLATION_MISSING_DL ? "DL" : "UL",
                               st.showing_cfr ? " - H needs both" : "");
        } else if (!st.has_memory) {
            ImGui::Text("No data on screen yet");
        } else {
            char samples_text[32];
            format_count(samples_text, sizeof(samples_text), st.samples_plotted);
            if (st.showing_mean) {
                ImGui::Text("%d period rows, %s cells (%s mean)", st.symbols_plotted, samples_text,
                            st.showing_cfr ? "H" : "IQ");
            } else if (st.showing_cfr) {
                ImGui::Text("%d paired symbols, %s estimates", st.symbols_plotted, samples_text);
            } else {
                ImGui::Text("%d symbols, %s samples", st.symbols_plotted, samples_text);
            }
            if (st.budget_hit) {
                ImGui::TextColored(warn_color, "Newest %d of %d symbols only (sample limit)",
                                   st.symbols_plotted, st.symbols_selected);
            }
        }

        if (source == IQ_CONSTELLATION_SOURCE_ZOOMED) {
            if (!st.main_view_valid) {
                ImGui::Text("Main view has nothing drawn yet - showing all");
            } else {
                ImGui::Text("RE %d-%d (PRB %d-%d)", st.re_first, st.re_last, st.re_first / 12, st.re_last / 12);
                switch (st.time) {
                case VISUALIZER_SELECTION_TIME_SYMBOLS:
                    if (st.symbol_shift != 0) {
                        ImGui::Text("UL symbols %d-%d (DL rows %+d)", st.symbol_first, st.symbol_last,
                                    st.symbol_shift);
                    } else {
                        ImGui::Text("Symbols %d-%d", st.symbol_first, st.symbol_last);
                    }
                    break;
                case VISUALIZER_SELECTION_TIME_PHASES:
                    ImGui::Text("Offsets %d-%d of period %d", st.phase_first,
                                (st.phase_first + st.phase_count - 1) % (st.period > 0 ? st.period : 1), st.period);
                    if (st.symbol_shift != 0) {
                        ImGui::SameLine();
                        ImGui::Text("(DL rows %+d)", st.symbol_shift);
                    }
                    break;
                case VISUALIZER_SELECTION_TIME_ALL:
                default:
                    ImGui::Text("All symbols (vertical axis is not time)");
                    break;
                }
                if (!st.main_view_zoomed) {
                    ImGui::Text("Main view not zoomed - its whole extent");
                }
            }
        }

        draw_skip_empty(constellation);
    }

    if (section_header("Display", true)) {
        bool density = iq_constellation_get_density(constellation) != 0;
        if (ImGui::Checkbox("Density colouring", &density)) {
            iq_constellation_set_density(constellation, density ? 1 : 0);
        }
        ImGui::SameLine();
        gui_hint("log hits per pixel",
                 "Colours each pixel by how many samples landed in it, on a log scale in the waterfall's palette, "
                 "so the spread inside a cluster shows. Off, every pixel hit at least once is the same green.");

        bool dot_size = iq_constellation_get_dot_size(constellation) != 0;
        if (ImGui::Checkbox("Dot size by density", &dot_size)) {
            iq_constellation_set_dot_size(constellation, dot_size ? 1 : 0);
        }
        ImGui::SameLine();
        gui_hint("denser = bigger",
                 "Draws each pixel's samples as one dot sized by how many landed there, on the same log scale as "
                 "the colour: up to 7 pixels across for the densest, a single pixel for sparse ones. Where dots "
                 "overlap the denser one is drawn, so dense clusters grow outward by a few pixels. Off, every "
                 "sample is one pixel, which resolves the finest detail.");
    }

    if (section_header("Mouse and keys", false)) {
        gui_hint("wheel zooms, drag moves",
                 "Wheel over the plot zooms about the pointer; drag the plot or use the arrow keys to move. I and Q "
                 "always share one scale, so circles stay circles whatever the window's shape.");
    }

    ImGui::End();
    gui_pop_theme();
}

}  // namespace

imgui_constellation_state_t* imgui_constellation_init(iq_constellation_t* constellation) {
    GLFWwindow* window = iq_constellation_window(constellation);
    if (window == nullptr) {
        return nullptr;
    }
    imgui_constellation_state_t* ui = new (std::nothrow) imgui_constellation_state();
    if (ui == nullptr) {
        return nullptr;
    }
    ui->window = window;
    ui->overview_zoomed = iq_constellation_zoom_is_active(constellation) != 0;

    ImGuiContext* previous = ImGui::GetCurrentContext();
    GLFWwindow* previous_gl = glfwGetCurrentContext();

    ui->context = ImGui::CreateContext();
    ImGui::SetCurrentContext(ui->context);
    ImGuiIO& io = ImGui::GetIO();
    // The main context already saves imgui.ini; two contexts writing one file
    // would keep overwriting each other's window positions.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    // The OpenGL backend creates its font texture in whichever GL context is
    // current, so this window's has to be.
    glfwMakeContextCurrent(window);
    // Callbacks off: see the input routing above.
    bool ok = ImGui_ImplGlfw_InitForOpenGL(window, false);
    if (ok && !ImGui_ImplOpenGL2_Init()) {
        ImGui_ImplGlfw_Shutdown();
        ok = false;
    }
    if (!ok) {
        ImGui::DestroyContext(ui->context);
        glfwMakeContextCurrent(previous_gl);
        ImGui::SetCurrentContext(previous);
        delete ui;
        return nullptr;
    }

    glfwSetWindowUserPointer(window, ui);
    set_callbacks(window, true);

    glfwMakeContextCurrent(previous_gl);
    ImGui::SetCurrentContext(previous);
    return ui;
}

void imgui_constellation_shutdown(imgui_constellation_state_t* ui) {
    if (ui == nullptr) {
        return;
    }
    // Detached first, so no event can reach a context that is going away.
    set_callbacks(ui->window, false);
    glfwSetWindowUserPointer(ui->window, nullptr);

    ImGuiContext* previous = ImGui::GetCurrentContext();
    GLFWwindow* previous_gl = glfwGetCurrentContext();
    ImGui::SetCurrentContext(ui->context);
    glfwMakeContextCurrent(ui->window);

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(ui->context);

    glfwMakeContextCurrent(previous_gl == ui->window ? nullptr : previous_gl);
    ImGui::SetCurrentContext(previous == ui->context ? nullptr : previous);
    delete ui;
}

void imgui_constellation_frame(imgui_constellation_state_t* ui,
                               iq_constellation_t* constellation,
                               const fh_oran_memory_t* memory,
                               const visualizer_zoom_selection_t* selection,
                               const iq_constellation_radar_pair_t* radar,
                               const visualizer_complex_mean_t* mean,
                               const char* stream_label,
                               bool frozen) {
    if (ui == nullptr || constellation == nullptr) {
        return;
    }

    // Described from the settings as they stand coming into the frame, like
    // the picture itself.
    char source_label[160];
    describe_source(constellation, radar, mean, stream_label, source_label, sizeof(source_label));
    iq_constellation_set_title(constellation, source_label);

    // Minimised: nothing to draw into, but the context still has to go back.
    if (!iq_constellation_begin_frame(constellation)) {
        iq_constellation_end_frame(constellation, 0);
        return;
    }

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ui->context);

    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplGlfw_NewFrame();

    // Framebuffer pixels throughout, the space the plot is laid out in - the
    // same choice the main window makes. Both are the same on X11.
    iq_constellation_view_t view;
    iq_constellation_get_view(constellation, &view);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)view.framebuffer_width, (float)view.framebuffer_height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    ImGui::NewFrame();

    // Before the update, so a zoom this frame is already in the picture drawn.
    handle_input(constellation, view);
    iq_constellation_update(constellation, memory, selection, radar, mean);

    // Read again: the update may have fitted the full view to new data.
    iq_constellation_get_view(constellation, &view);
    iq_constellation_stats_t st;
    iq_constellation_get_stats(constellation, &st);

    draw_axes(view);
    switch (st.missing) {
    case IQ_CONSTELLATION_MISSING_STREAM:
        draw_plot_notice(view, "No stream on screen yet");
        break;
    case IQ_CONSTELLATION_MISSING_DL:
        draw_plot_notice(view, st.showing_cfr ? "H needs both directions: the pair's DL has not been captured"
                                              : "The pair's DL has not been captured");
        break;
    case IQ_CONSTELLATION_MISSING_UL:
        draw_plot_notice(view, st.showing_cfr ? "H needs both directions: the pair's UL has not been captured"
                                              : "The pair's UL has not been captured");
        break;
    case IQ_CONSTELLATION_MISSING_NONE:
    default:
        if (st.has_memory && st.samples_plotted == 0) {
            draw_plot_notice(view, st.showing_mean  ? "The rolling mean holds nothing in this selection yet"
                                   : st.showing_cfr ? "No paired symbols in this selection"
                                                    : "No samples in this selection");
        }
        break;
    }
    if (frozen) {
        draw_frozen_badge(view);
    }
    draw_cursor_readout(constellation, view);
    draw_panel(ui, constellation, view, source_label, radar, mean);

    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

    ImGui::SetCurrentContext(previous);
    iq_constellation_end_frame(constellation, 1);
}
