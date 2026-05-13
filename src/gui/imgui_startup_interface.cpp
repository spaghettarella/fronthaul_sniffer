#include "gui/imgui_startup_interface.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl2.h>
#include <pcap/pcap.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct fh_startup_interface_state {
    std::vector<std::string> interfaces;
    int selected_interface_index;
    std::array<char, 512> demo_pcap_path_buffer;
};

namespace {

constexpr int kScsValues[] = {15, 30, 60, 120, 240};
constexpr const char* kCompMethods[] = {"NONE", "BFP"};
constexpr const char* kDirections[] = {"UL", "DL"};

uint16_t pack_channel_eaxc(int du_port_id, int band_sector_id, int cc_id, int ru_port_id) {
    const uint16_t du = static_cast<uint16_t>(du_port_id) & 0x3u;
    const uint16_t band = static_cast<uint16_t>(band_sector_id) & 0x3Fu;
    const uint16_t cc = static_cast<uint16_t>(cc_id) & 0xFu;
    const uint16_t ru = static_cast<uint16_t>(ru_port_id) & 0xFu;
    return static_cast<uint16_t>((du << 14) | (band << 8) | (cc << 4) | ru);
}

void clamp_int(int& value, int min_value, int max_value) {
    value = std::max(min_value, std::min(max_value, value));
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

bool open_file_dialog(std::string& selected_path, const std::string& initial_path) {
    std::string command = "zenity --file-selection";
    if (!initial_path.empty()) {
        command += " --filename=" + shell_quote(initial_path);
    }
    command += " 2>/dev/null";

    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }

    std::array<char, 4096> buffer{};
    std::string output;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

    const int status = pclose(pipe);
    if (status != 0) {
        return false;
    }

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }

    if (output.empty()) {
        return false;
    }

    selected_path = output;
    return true;
}

std::vector<std::string> scan_interfaces() {
    std::vector<std::string> interfaces;
    interfaces.emplace_back("pcap");

    char errbuf[PCAP_ERRBUF_SIZE] = {0};
    pcap_if_t* alldevs = nullptr;
    if (pcap_findalldevs(&alldevs, errbuf) != 0) {
        return interfaces;
    }

    for (pcap_if_t* dev = alldevs; dev != nullptr; dev = dev->next) {
        if (dev->name == nullptr || dev->name[0] == '\0') {
            continue;
        }

        if (std::find(interfaces.begin(), interfaces.end(), dev->name) == interfaces.end()) {
            interfaces.emplace_back(dev->name);
        }
    }

    pcap_freealldevs(alldevs);
    return interfaces;
}

int find_interface_index(const std::vector<std::string>& interfaces, const std::string& iface) {
    for (std::size_t i = 0; i < interfaces.size(); ++i) {
        if (interfaces[i] == iface) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

}  // namespace

fh_startup_interface_state_t* imgui_startup_init(const fh_startup_config_t* initial_config) {
    auto* state = new fh_startup_interface_state{};
    state->interfaces = scan_interfaces();
    if (state->interfaces.empty()) {
        state->interfaces.emplace_back("pcap");
    }

    if (initial_config != nullptr) {
        state->selected_interface_index = find_interface_index(state->interfaces, initial_config->iface);
        (void)snprintf(state->demo_pcap_path_buffer.data(), state->demo_pcap_path_buffer.size(), "%s", initial_config->demo_pcap_path.c_str());
    } else {
        state->selected_interface_index = 0;
        (void)snprintf(state->demo_pcap_path_buffer.data(), state->demo_pcap_path_buffer.size(), "%s", "../src/capture/demo.pcap");
    }

    return state;
}

bool imgui_startup_render(
    fh_startup_interface_state_t* state,
    fh_startup_config_t* config,
    int window_width,
    int window_height) {
    if (state == nullptr || config == nullptr) {
        return false;
    }

    if (state->selected_interface_index < 0 || state->selected_interface_index >= static_cast<int>(state->interfaces.size())) {
        state->selected_interface_index = 0;
    }

    const char* selected_iface = state->interfaces[state->selected_interface_index].c_str();
    if (config->iface != selected_iface) {
        const int synced_index = find_interface_index(state->interfaces, config->iface);
        state->selected_interface_index = synced_index;
        selected_iface = state->interfaces[state->selected_interface_index].c_str();
    }

    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(window_width), static_cast<float>(window_height));

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 viewport_center = ImVec2(
        viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);

    ImGui::SetNextWindowPos(
        viewport_center,
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(900.0f, 0.0f), ImGuiCond_Always);

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

    bool start_requested = false;
    ImGui::Begin("Startup Configuration", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::TextUnformatted("Configure capture settings before starting.");
    ImGui::Spacing();

    if (ImGui::BeginCombo("Interface", selected_iface)) {
        for (int i = 0; i < static_cast<int>(state->interfaces.size()); ++i) {
            const bool is_selected = (i == state->selected_interface_index);
            if (ImGui::Selectable(state->interfaces[i].c_str(), is_selected)) {
                state->selected_interface_index = i;
                config->iface = state->interfaces[i];
            }

            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("select 'pcap' to replay from a pcap file");

    config->iface = state->interfaces[state->selected_interface_index];

    if (config->iface == "pcap") {
        ImGui::PushID("pcap_path_row");
        ImGui::SetNextItemWidth(448.0f);
        ImGui::InputText("##pcap_path", state->demo_pcap_path_buffer.data(), state->demo_pcap_path_buffer.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            std::string picked_path;
            if (open_file_dialog(picked_path, state->demo_pcap_path_buffer.data())) {
                (void)snprintf(state->demo_pcap_path_buffer.data(), state->demo_pcap_path_buffer.size(), "%s", picked_path.c_str());
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("PCAP path");
        ImGui::PopID();

        config->demo_pcap_path = state->demo_pcap_path_buffer.data();
        ImGui::InputInt("PCAP speed (packets/sec)", &config->demo_pcap_speed);
        clamp_int(config->demo_pcap_speed, 1, 1000000);
        ImGui::SameLine();
        ImGui::TextDisabled("replay speed for pcap interface");

        ImGui::Checkbox("Loop PCAP playback", &config->demo_pcap_loop);
        ImGui::SameLine();
        ImGui::TextDisabled("enabled by default");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Capture");
    ImGui::InputInt("Snaplen (bytes)", &config->snaplen, 1, 1000);
    clamp_int(config->snaplen, 1, 1000000);
    ImGui::SameLine();
    ImGui::TextDisabled("bytes reserved for each packet (>9000 for Jumbo Frames)");

    // Keep queue_bytes in code but combine in the GUI: set queue_bytes to snaplen
    config->queue_bytes = static_cast<unsigned long long>(config->snaplen);

    // GUI exposes only Snaplen; Queue bytes follows Snaplen automatically.

    const unsigned long long queue_packets_step = 1ULL;
    const unsigned long long queue_packets_step_fast = 100ULL;
    ImGui::InputScalar("Queue packets", ImGuiDataType_U64, &config->queue_packets, &queue_packets_step, &queue_packets_step_fast, "%llu");
    if (config->queue_packets < 1ULL) {
        config->queue_packets = 1ULL;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("capture buffer before processing");

    // GUI exposes only Snaplen; Queue bytes follows Snaplen automatically.

    const unsigned long long decode_threads_step = 1ULL;
    const unsigned long long decode_threads_step_fast = 4ULL;
    ImGui::InputScalar("Decode threads", ImGuiDataType_U64, &config->decode_threads, &decode_threads_step, &decode_threads_step_fast, "%llu");
    if (config->decode_threads < 1ULL) {
        config->decode_threads = 1ULL;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("number of threads for packet decoding (increase for high packet rates)");

    const unsigned long long memory_size_step = 1ULL;
    const unsigned long long memory_size_step_fast = 100ULL;
    ImGui::InputScalar("Memory size (symbols)", ImGuiDataType_U64, &config->memory_symbols, &memory_size_step, &memory_size_step_fast, "%llu");
    if (config->memory_symbols < 1ULL) {
        config->memory_symbols = 1ULL;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("number of symbols to keep in memory (vertical resolution)");

    // Place timeout control at the bottom of the Capture section
    ImGui::Spacing();
    ImGui::Checkbox("Enable timeout", &config->timeout_enabled);
    if (config->timeout_enabled) {
        ImGui::InputInt("Timeout (ms)", &config->timeout_ms, 1, 1000);
        clamp_int(config->timeout_ms, 1, 1000000);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("timeout for packet capture (shut down if stream stops)");

    ImGui::Spacing();
    ImGui::SeparatorText("Decoder");
    ImGui::Checkbox("User Data Compression Header present", &config->ud_comp_hdr_present);
    // TODO: implement compression header parsing
    ImGui::SameLine();
    ImGui::TextDisabled("ud_comp_hdr parsing not supported yet");

    if (ImGui::BeginCombo("Compression Method", config->ud_comp_method.c_str())) {
        for (const char* method : kCompMethods) {
            const bool is_selected = (config->ud_comp_method == method);
            if (ImGui::Selectable(method, is_selected)) {
                config->ud_comp_method = method;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("for now only 'BFP' or 'NONE' (uncompressed) supported");

    ImGui::InputInt("UD IQ-Width (bits)", &config->ud_iq_width);
    ImGui::SameLine();
    ImGui::TextDisabled("default 9, range 1-16");
    clamp_int(config->ud_iq_width, 1, 16);

    ImGui::InputInt("UD IQ-Width Decompressed (bits)", &config->ud_iq_width_decompressed);
    ImGui::SameLine();
    ImGui::TextDisabled("default 16, range 1-16");
    clamp_int(config->ud_iq_width_decompressed, 1, 16);

    if (ImGui::BeginCombo("SCS (kHz)", std::to_string(config->scs_khz).c_str())) {
        for (int scs : kScsValues) {
            const bool is_selected = (config->scs_khz == scs);
            const std::string label = std::to_string(scs);
            if (ImGui::Selectable(label.c_str(), is_selected)) {
                config->scs_khz = scs;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("subcarrier spacing (default 30, values 15/30/60/120/240)");

    ImGui::Spacing();
    ImGui::SeparatorText("Channel filter");
    ImGui::Checkbox("Filter by eAxC ID", &config->channel_has_eaxc);
    ImGui::SameLine();
    ImGui::TextDisabled("aka PC_ID (other formats only in CLI supported)");

    if (config->channel_has_eaxc) {
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("DU Port ID (2 bits)", &config->channel_du_port_id, 1, 1);
        clamp_int(config->channel_du_port_id, 0, 3);

        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("Band Sector ID (6 bits)", &config->channel_band_sector_id, 1, 1);
        clamp_int(config->channel_band_sector_id, 0, 63);

        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("CC ID (4 bits)", &config->channel_cc_id, 1, 1);
        clamp_int(config->channel_cc_id, 0, 15);

        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("RU Port ID (4 bits)", &config->channel_ru_port_id, 1, 1);
        clamp_int(config->channel_ru_port_id, 0, 15);

        config->channel_eaxc = pack_channel_eaxc(
            config->channel_du_port_id,
            config->channel_band_sector_id,
            config->channel_cc_id,
            config->channel_ru_port_id);

        ImGui::TextDisabled("eAxC_ID preview: 0x%04X", config->channel_eaxc);
    }

    ImGui::Checkbox("Filter by Data Direction", &config->channel_has_direction);
    ImGui::SameLine();
    ImGui::TextDisabled("'UL' (uplink) / 'DL' (downlink)");
    if (config->channel_has_direction) {
        int direction_index = config->channel_direction;
        if (ImGui::BeginCombo("##direction_combo", kDirections[direction_index])) {
            for (int i = 0; i < 2; ++i) {
                const bool is_selected = (config->channel_direction == i);
                if (ImGui::Selectable(kDirections[i], is_selected)) {
                    config->channel_direction = i;
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Start", ImVec2(120.0f, 0.0f))) {
        start_requested = true;
    }

    ImGui::End();

    // Pop the startup theme colors
    ImGui::PopStyleColor(9);

    // Match the normal ImGui frame lifecycle so the next NewFrame() starts cleanly.
    ImGui::Render();

    return start_requested;
}

void imgui_startup_shutdown(fh_startup_interface_state_t* state) {
    delete state;
}
