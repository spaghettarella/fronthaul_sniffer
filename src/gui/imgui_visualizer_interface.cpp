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
    state->selected_eaxc_id_index = 0;
    state->selected_data_direction = 0;  // Default to UL
    state->prev_data_direction = 0;

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

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

    // Create stream selection window
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
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

    // Display stream list
    ImGui::BeginChild("StreamList", ImVec2(280, 120), true, ImGuiWindowFlags_AlwaysAutoResize);

    for (int i = 0; i < num_unique_streams; i++) {
        uint16_t eaxc_id = streams[i].eaxc_id;
        ImGui::PushID(i);

        // Format eAxC_ID for display
        char eaxc_label[16];
        snprintf(eaxc_label, sizeof(eaxc_label), "0x%04X", eaxc_id);

        // Radio button for stream selection
        bool is_selected = (state->selected_eaxc_id_index == i);
        if (ImGui::Selectable(eaxc_label, is_selected)) {
            state->selected_eaxc_id_index = i;
        }

        ImGui::SameLine(120);

        // Direction selector (combo for available directions)
        int available_directions = 0;
        int direction_indices[2];
        const char* available_labels[2];

        if (streams[i].has_ul) {
            direction_indices[available_directions] = 0;
            available_labels[available_directions] = "UL";
            available_directions++;
        }
        if (streams[i].has_dl) {
            direction_indices[available_directions] = 1;
            available_labels[available_directions] = "DL";
            available_directions++;
        }

        // Find current selection index in available directions
        int current_direction_idx = 0;
        for (int j = 0; j < available_directions; j++) {
            if (direction_indices[j] == state->selected_data_direction) {
                current_direction_idx = j;
                break;
            }
        }

        if (ImGui::Combo("##direction", &current_direction_idx, available_labels, available_directions)) {
            state->selected_data_direction = direction_indices[current_direction_idx];
            state->prev_data_direction = state->selected_data_direction;
        }

        ImGui::PopID();
    }

    ImGui::EndChild();

    // Status display
    if (state->selected_eaxc_id_index < num_unique_streams) {
        uint16_t selected_eaxc_id = streams[state->selected_eaxc_id_index].eaxc_id;
        const char* direction_str = data_direction_to_string(state->selected_data_direction);
        ImGui::Text("Selected: 0x%04X / %s", selected_eaxc_id, direction_str);
    }

    ImGui::End();

    // Render ImGui
    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

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

void imgui_visualizer_shutdown(imgui_visualizer_state_t* state) {
    if (!state || !state->initialized) {
        return;
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    free(state);
}
