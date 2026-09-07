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

        ImGui::End();
    }

    // Axes and markers. Drawn on the background list so they sit over the
    // heatmap but under every panel, and entirely within the margins the
    // visualizer reserves, so they never cover data.
    if (visualizer != nullptr) {
        draw_plot_axes(visualizer, current_display_memory);
        draw_markers(visualizer);
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
