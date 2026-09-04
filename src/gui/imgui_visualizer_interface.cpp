#include "imgui_visualizer_interface.h"
#include "core/c_oran_memory.h"
#include "pipeline/c_packet_queue.h"
#include "visualization/opengl_visualizer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl2.h>
#include <GLFW/glfw3.h>

#include <cstdlib>
#include <cstring>

struct imgui_visualizer_state {
    bool initialized;
    bool selection_initialized;
    int selected_eaxc_id_index;
    uint8_t selected_data_direction;
    uint8_t prev_data_direction;

    // Frame timing. The wire frame_id is only 8 bits, so it wraps every 256
    // frames; the memory pairs it with a wraparound count. Accumulating the
    // advance between samples here keeps a total that survives both that
    // wrap and the counter being reset (which pcap replay does on each loop).
    bool frame_tracking_initialized;
    const void* frame_tracking_memory;
    uint64_t last_absolute_frame;
    uint64_t frames_since_start;
};

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
