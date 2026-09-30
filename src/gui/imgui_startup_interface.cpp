#include "gui/imgui_startup_interface.h"

#include "capture/c_pcap_demo_stream.h"

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
    std::array<char, 256> ocudu_phy_tap_queue_buffer;

    // The channel whitelist's row for adding the next entry. Kept here rather
    // than in the config, since it is only an editor, not a setting.
    int whitelist_du_port_id;
    int whitelist_band_sector_id;
    int whitelist_cc_id;
    int whitelist_ru_port_id;
    int whitelist_direction_index;  // into kWhitelistDirections
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

// A collapsible section, matching the running view's settings windows. Open
// at first only when open_at_start says the section is in use, so the screen
// leads with what matters for this run and folds the rest away.
bool section_header(const char* label, bool open_at_start) {
    return ImGui::CollapsingHeader(label, open_at_start ? ImGuiTreeNodeFlags_DefaultOpen : 0);
}

void clamp_int(int& value, int min_value, int max_value) {
    value = std::max(min_value, std::min(max_value, value));
}

// Index + 1 is the entry's FH_CHANNEL_DIR_* bits.
constexpr const char* kWhitelistDirections[] = {"UL", "DL", "Both"};

// The whitelist half of the Channel filter section: the list, with each
// entry's directions editable in place, and a row for adding the next eAxC -
// entered by its fields, the same way the single-channel filter takes it.
void draw_channel_whitelist(fh_startup_interface_state_t* state, fh_startup_config_t* config) {
    std::vector<fh_channel_whitelist_entry_t>& list = config->channel_whitelist;

    if (list.empty()) {
        ImGui::TextDisabled("Empty - every channel passes until an entry is added.");
    } else if (ImGui::BeginTable("channel_whitelist", 4,
                                 ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("eAxC_ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("DU / Band sector / CC / RU", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Direction", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        int remove_index = -1;
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            fh_channel_whitelist_entry_t& entry = list[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("0x%04X", entry.eaxc);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%u / %u / %u / %u",
                                static_cast<unsigned>((entry.eaxc >> 14) & 0x3u),
                                static_cast<unsigned>((entry.eaxc >> 8) & 0x3Fu),
                                static_cast<unsigned>((entry.eaxc >> 4) & 0xFu),
                                static_cast<unsigned>(entry.eaxc & 0xFu));

            ImGui::TableSetColumnIndex(2);
            int direction_index = static_cast<int>(entry.directions & FH_CHANNEL_DIR_BOTH) - 1;
            if (direction_index < 0) {
                direction_index = 2;
            }
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("##direction", &direction_index, kWhitelistDirections,
                             IM_ARRAYSIZE(kWhitelistDirections))) {
                entry.directions = static_cast<uint8_t>(direction_index + 1);
            }

            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("Remove")) {
                remove_index = i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();

        if (remove_index >= 0) {
            list.erase(list.begin() + remove_index);
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Add channel");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputInt("DU Port ID (2 bits)##whitelist", &state->whitelist_du_port_id, 1, 1);
    clamp_int(state->whitelist_du_port_id, 0, 3);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputInt("Band Sector ID (6 bits)##whitelist", &state->whitelist_band_sector_id, 1, 1);
    clamp_int(state->whitelist_band_sector_id, 0, 63);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputInt("CC ID (4 bits)##whitelist", &state->whitelist_cc_id, 1, 1);
    clamp_int(state->whitelist_cc_id, 0, 15);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputInt("RU Port ID (4 bits)##whitelist", &state->whitelist_ru_port_id, 1, 1);
    clamp_int(state->whitelist_ru_port_id, 0, 15);

    const uint16_t eaxc = pack_channel_eaxc(state->whitelist_du_port_id, state->whitelist_band_sector_id,
                                            state->whitelist_cc_id, state->whitelist_ru_port_id);
    ImGui::SetNextItemWidth(80.0f);
    ImGui::Combo("Direction##whitelist_add", &state->whitelist_direction_index, kWhitelistDirections,
                 IM_ARRAYSIZE(kWhitelistDirections));
    ImGui::SameLine();

    const bool full = list.size() >= static_cast<std::size_t>(FH_CHANNEL_WHITELIST_MAX);
    if (full) {
        ImGui::BeginDisabled();
    }
    char add_label[32];
    snprintf(add_label, sizeof(add_label), "Add 0x%04X", eaxc);
    if (ImGui::Button(add_label)) {
        const uint8_t bits = static_cast<uint8_t>(state->whitelist_direction_index + 1);
        // One entry per eAxC: adding one that is already listed widens its
        // directions instead of listing it twice.
        auto existing = std::find_if(list.begin(), list.end(),
                                     [eaxc](const fh_channel_whitelist_entry_t& e) { return e.eaxc == eaxc; });
        if (existing != list.end()) {
            existing->directions = static_cast<uint8_t>(existing->directions | bits);
        } else {
            list.push_back(fh_channel_whitelist_entry_t{eaxc, bits});
        }
    }
    if (full) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("list full (%d)", FH_CHANNEL_WHITELIST_MAX);
    }
    ImGui::TextDisabled("Adding an eAxC that is already listed adds the direction to its entry.");
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
    interfaces.emplace_back("ocudu phy tap");

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
    state->whitelist_direction_index = 2;  // Both: the usual reason to list a channel
    state->interfaces = scan_interfaces();
    if (state->interfaces.empty()) {
        state->interfaces.emplace_back("pcap");
    }

    if (initial_config != nullptr) {
        state->selected_interface_index = find_interface_index(state->interfaces, initial_config->iface);
        (void)snprintf(state->demo_pcap_path_buffer.data(), state->demo_pcap_path_buffer.size(), "%s", initial_config->demo_pcap_path.c_str());
        (void)snprintf(state->ocudu_phy_tap_queue_buffer.data(), state->ocudu_phy_tap_queue_buffer.size(), "%s", initial_config->ocudu_phy_tap_queue_name.c_str());
    } else {
        state->selected_interface_index = 0;
        (void)snprintf(state->demo_pcap_path_buffer.data(), state->demo_pcap_path_buffer.size(), "%s", "../src/capture/demo.pcap");
        (void)snprintf(state->ocudu_phy_tap_queue_buffer.data(), state->ocudu_phy_tap_queue_buffer.size(), "%s", "/ocudu_phy_tap_queue");
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

    if (section_header("Source", true)) {
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
            clamp_int(config->demo_pcap_speed, 1, FH_PCAP_MAX_SPEED_PPS);
            ImGui::SameLine();
            ImGui::TextDisabled("replay speed; also adjustable while running, in Live Statistics");

            ImGui::Checkbox("Loop PCAP playback", &config->demo_pcap_loop);
            ImGui::SameLine();
            ImGui::TextDisabled("enabled by default");
        
        } else if (config->iface == "ocudu phy tap") {
            ImGui::SetNextItemWidth(448.0f);
            ImGui::InputText("PHY tap queue name", state->ocudu_phy_tap_queue_buffer.data(), state->ocudu_phy_tap_queue_buffer.size());
            config->ocudu_phy_tap_queue_name = state->ocudu_phy_tap_queue_buffer.data();
            ImGui::SameLine();
            ImGui::TextDisabled("shared-memory queue name");
        }
    }

    if (section_header("Capture", true)) {
        ImGui::InputInt("Snaplen (bytes)", &config->snaplen, 1, 1000);
        clamp_int(config->snaplen, 1, 1000000);
        ImGui::SameLine();
        ImGui::TextDisabled("bytes reserved for each packet (>9000 for Jumbo Frames, >20000 for PHY tap)");

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

        // Place stream-stop control at the bottom of the Capture section
        ImGui::Spacing();
        ImGui::Checkbox("Exit when stream stops", &config->timeout_enabled);
        ImGui::SameLine();
        ImGui::TextDisabled("end the run once the capture goes silent");
        if (config->timeout_enabled) {
            ImGui::InputInt("Silence before exit (ms)", &config->timeout_ms, 1, 1000);
            clamp_int(config->timeout_ms, 1, 1000000);
            ImGui::SameLine();
            ImGui::TextDisabled("armed only after the first packet arrives");
        }
    }

    if (section_header("Decoder", true)) {
        ImGui::Checkbox("Force user data compression header", &config->ud_comp_hdr_present);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                "When compression parameters travel in band, each U-Plane section header is\n"
                "followed by a udCompHdr octet and a reserved octet, and the PRB data starts\n"
                "two bytes later.\n\n"
                "Leave this off to detect it automatically, separately per eAxC and direction:\n"
                "the octets have to match the settings below several times in a row before\n"
                "they are trusted, and a note is printed when that happens.\n\n"
                "Tick it to skip detection and always parse them, for a sender whose header\n"
                "does not match the configured width or method.");
            ImGui::EndTooltip();
        }
        ImGui::TextDisabled("Off = detect automatically per stream");

        if (ImGui::BeginCombo("Compression Method", config->ud_comp_method.c_str())) {
            for (const char* method : kCompMethods) {
                const bool is_selected = (config->ud_comp_method == method);
                if (ImGui::Selectable(method, is_selected)) {
                    const bool method_changed = (config->ud_comp_method != method);
                    config->ud_comp_method = method;
                    // Uncompressed samples are carried at their full width, which is
                    // 16 bits in practice; the 9-bit default only makes sense for
                    // BFP. Applied on the change rather than every frame, so it
                    // stays a starting point the field below can still override.
                    if (method_changed && config->ud_comp_method == "NONE") {
                        config->ud_iq_width = 16;
                    }
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
        if (config->ud_comp_method == "NONE") {
            ImGui::TextDisabled("uncompressed: 16 unless the sender says otherwise, range 1-16");
        } else {
            ImGui::TextDisabled("default 9, range 1-16");
        }
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

        if (config->iface == "ocudu phy tap") {
            ImGui::Checkbox("Override SCS from PHY tap numerology", &config->phy_tap_scs_override);
            ImGui::SameLine();
            ImGui::TextDisabled("use each record's own numerology instead of the SCS above");
        }
    }

    // Only the Radar view uses it, and no run starts in that view.
    if (section_header("Radar", false)) {
        const unsigned long long fft_threads_step = 1ULL;
        const unsigned long long fft_threads_step_fast = 4ULL;
        ImGui::InputScalar("FFT threads", ImGuiDataType_U64, &config->fft_threads, &fft_threads_step, &fft_threads_step_fast, "%llu");
        if (config->fft_threads < 1ULL) {
            config->fft_threads = 1ULL;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("threads for the radar transforms; fixed for the run");
        ImGui::TextDisabled("Only used by the Radar view. Everything else about the radar");
        ImGui::TextDisabled("is configured live, in the Radar Settings window.");
    }

    // Open when a filter is already set, e.g. passed on the command line,
    // so an active filter is never hidden behind a folded header.
    const bool whitelist_selected = (config->channel_filter_method == FH_CHANNEL_FILTER_WHITELIST);
    const bool filter_set = whitelist_selected ? !config->channel_whitelist.empty()
                                               : (config->channel_has_eaxc || config->channel_has_direction);
    if (section_header("Channel filter", filter_set)) {
        int method = static_cast<int>(config->channel_filter_method);
        ImGui::RadioButton("Single channel", &method, FH_CHANNEL_FILTER_SINGLE);
        ImGui::SameLine();
        ImGui::RadioButton("Whitelist", &method, FH_CHANNEL_FILTER_WHITELIST);
        config->channel_filter_method = static_cast<fh_channel_filter_method_t>(method);
        ImGui::SameLine();
        if (config->channel_filter_method == FH_CHANNEL_FILTER_WHITELIST) {
            ImGui::TextDisabled("keep only the listed channels, each with its own direction");
        } else {
            ImGui::TextDisabled("one eAxC and/or one direction");
        }

        if (config->channel_filter_method == FH_CHANNEL_FILTER_WHITELIST) {
            draw_channel_whitelist(state, config);
        } else {
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
