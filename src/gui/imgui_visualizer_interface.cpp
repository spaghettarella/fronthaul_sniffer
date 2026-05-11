#include "imgui_visualizer_interface.h"
#include "core/c_oran_memory.h"

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
    int window_height) {
    
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
