#include "gui/imgui_startup_interface.h"
#include "gui/imgui_hint.h"

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
    ImGui::SameLine();
    gui_hint("merges duplicates",
             "Adding an eAxC that is already listed adds the direction to its existing entry instead of "
             "listing it twice.");
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
        (void)snprintf(state->demo_pcap_path_buffer.data(), state->demo_pcap_path_buffer.size(), "%s", "../src/capture/ofdm_radar_test.pcap");
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
        gui_hint("'pcap' replays a file",
                 "Where packets come from. A network interface is captured live ('lo' through the loopback "
                 "backend). 'pcap' replays a capture file instead, and 'ocudu phy tap' reads the OCUDU PHY "
                 "tap's shared-memory queue.");

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
            gui_hint("live-adjustable",
                     "Replay rate in packets per second, over every channel in the file. Real time is 28 000 "
                     "packets/s per stream at 30 kHz with one packet per symbol - 224 000 for four antennas "
                     "with DL and UL. Can be changed while running, in Live Statistics.");

            ImGui::Checkbox("Loop PCAP playback", &config->demo_pcap_loop);
            ImGui::SameLine();
            gui_hint("default on",
                     "Start the file again when it ends. Symbol numbers keep counting up across passes, "
                     "rounded to whole frames, so the views carry on instead of jumping back. Off ends the "
                     "run after one pass.");
        
        } else if (config->iface == "ocudu phy tap") {
            ImGui::SetNextItemWidth(448.0f);
            ImGui::InputText("PHY tap queue name", state->ocudu_phy_tap_queue_buffer.data(), state->ocudu_phy_tap_queue_buffer.size());
            config->ocudu_phy_tap_queue_name = state->ocudu_phy_tap_queue_buffer.data();
            ImGui::SameLine();
            gui_hint("shared-memory queue",
                     "Name of the shared-memory queue the OCUDU PHY tap writes into. Has to match the name the "
                     "gNB side was configured with.");
        }
    }

    if (section_header("Capture", true)) {
        ImGui::InputInt("Snaplen (bytes)", &config->snaplen, 1, 1000);
        clamp_int(config->snaplen, 1, 1000000);
        ImGui::SameLine();
        gui_hint("bytes per packet",
                 "Bytes reserved for each captured packet; anything longer is cut off. Needs more than 9000 "
                 "for jumbo frames and more than 20000 for the PHY tap. The queue's per-packet buffer "
                 "follows this value.");

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
        gui_hint("buffer before decode",
                 "How many packets the queue between capture and decode holds. Once the decoders fall "
                 "behind and it is full, new packets are dropped and counted in Live Statistics.");

        // GUI exposes only Snaplen; Queue bytes follows Snaplen automatically.

        const unsigned long long decode_threads_step = 1ULL;
        const unsigned long long decode_threads_step_fast = 4ULL;
        ImGui::InputScalar("Decode threads", ImGuiDataType_U64, &config->decode_threads, &decode_threads_step, &decode_threads_step_fast, "%llu");
        // Shares one budget with the FFT threads, which keep at least one. The
        // min keeps the subtraction from wrapping if the other count ever
        // arrives over the cap.
        const unsigned long long decode_threads_max =
            FH_MAX_DECODE_FFT_THREADS - std::min<unsigned long long>(config->fft_threads, FH_MAX_DECODE_FFT_THREADS - 1);
        if (config->decode_threads < 1ULL) {
            config->decode_threads = 1ULL;
        }
        if (config->decode_threads > decode_threads_max) {
            config->decode_threads = decode_threads_max;
        }
        ImGui::SameLine();
        // The FFT count lives in the folded Radar section, so the other half of
        // the budget is spelled out here.
        char decode_threads_help[256];
        snprintf(decode_threads_help, sizeof(decode_threads_help),
                 "Threads decoding packets in parallel; raise for high packet rates. Each handles about "
                 "20 000 full-symbol packets/s, and past about 16 more stop helping. Shares a budget of %d "
                 "with the radar's FFT threads (now %llu).",
                 FH_MAX_DECODE_FFT_THREADS, config->fft_threads);
        gui_hint("decode + FFT <= 20", decode_threads_help);

        const unsigned long long memory_size_step = 1ULL;
        const unsigned long long memory_size_step_fast = 100ULL;
        ImGui::InputScalar("Memory size (symbols)", ImGuiDataType_U64, &config->memory_symbols, &memory_size_step, &memory_size_step_fast, "%llu");
        if (config->memory_symbols < 1ULL) {
            config->memory_symbols = 1ULL;
        }
        ImGui::SameLine();
        gui_hint("symbols per stream",
                 "Symbols each stream keeps, which is also the waterfall's height. Costs about 26 KB per "
                 "symbol per stream. The radar's batch-aligned windows need at least twice their time "
                 "range plus 56.");

        // Place stream-stop control at the bottom of the Capture section
        ImGui::Spacing();
        ImGui::Checkbox("Exit when stream stops", &config->timeout_enabled);
        ImGui::SameLine();
        gui_hint("on silence",
                 "End the run by itself once the capture has been silent for the time below.");
        if (config->timeout_enabled) {
            ImGui::InputInt("Silence before exit (ms)", &config->timeout_ms, 1, 1000);
            clamp_int(config->timeout_ms, 1, 1000000);
            ImGui::SameLine();
            gui_hint("after first packet",
                     "Only armed once the first packet has arrived, so starting the sniffer before the gNB "
                     "or RU does not end the run early.");
        }
    }

    if (section_header("Decoder", true)) {
        ImGui::Checkbox("Force user data compression header", &config->ud_comp_hdr_present);
        ImGui::SameLine();
        gui_hint("off = auto-detect",
                 "When compression parameters travel in band, each U-Plane section header is followed by a "
                 "udCompHdr octet and a reserved octet, and the PRB data starts two bytes later.\n\n"
                 "Off detects this automatically, separately per eAxC and direction: the octets have to "
                 "match the settings below several times in a row before they are trusted, and a note is "
                 "printed when that happens.\n\n"
                 "On skips detection and always parses them, for a sender whose header does not match the "
                 "configured width or method.");

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
        gui_hint("BFP or NONE",
                 "Block floating point (BFP) or uncompressed (NONE); other O-RAN methods are not supported "
                 "yet. Switching to NONE sets the IQ width to 16.");

        ImGui::InputInt("UD IQ-Width (bits)", &config->ud_iq_width);
        ImGui::SameLine();
        if (config->ud_comp_method == "NONE") {
            gui_hint("usually 16",
                     "Width of the uncompressed samples, 1-16. 16 unless the sender says otherwise.");
        } else {
            gui_hint("default 9",
                     "Mantissa width of the BFP-compressed samples, 1-16. Default 9.");
        }
        clamp_int(config->ud_iq_width, 1, 16);

        ImGui::InputInt("UD IQ-Width Decompressed (bits)", &config->ud_iq_width_decompressed);
        ImGui::SameLine();
        gui_hint("default 16", "Width the samples are expanded to after decompression, 1-16. Default 16.");
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
        gui_hint("subcarrier spacing",
                 "Subcarrier spacing of the carrier: 15, 30, 60, 120 or 240 kHz, default 30. Sets how "
                 "symbol numbers map to slots and frames, and the radar's range and Doppler scales.");

        if (config->iface == "ocudu phy tap") {
            ImGui::Checkbox("Override SCS from PHY tap numerology", &config->phy_tap_scs_override);
            ImGui::SameLine();
            gui_hint("per-record numerology",
                     "Use the numerology each PHY tap record carries instead of the SCS above.");
        }
    }

    // Only the Radar view uses it, and no run starts in that view.
    if (section_header("Radar", false)) {
        const unsigned long long fft_threads_step = 1ULL;
        const unsigned long long fft_threads_step_fast = 4ULL;
        ImGui::InputScalar("FFT threads", ImGuiDataType_U64, &config->fft_threads, &fft_threads_step, &fft_threads_step_fast, "%llu");
        const unsigned long long fft_threads_max =
            FH_MAX_DECODE_FFT_THREADS - std::min<unsigned long long>(config->decode_threads, FH_MAX_DECODE_FFT_THREADS - 1);
        if (config->fft_threads < 1ULL) {
            config->fft_threads = 1ULL;
        }
        if (config->fft_threads > fft_threads_max) {
            config->fft_threads = fft_threads_max;
        }
        ImGui::SameLine();
        char fft_threads_help[320];
        snprintf(fft_threads_help, sizeof(fft_threads_help),
                 "Threads the radar's FFTs are split across; fixed for the run. Only the Radar view uses "
                 "them - everything else about the radar is set live, in the Radar Settings window. Shares "
                 "a budget of %d with the decode threads (now %llu).",
                 FH_MAX_DECODE_FFT_THREADS, config->decode_threads);
        gui_hint("decode + FFT <= 20", fft_threads_help);
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
        gui_hint(config->channel_filter_method == FH_CHANNEL_FILTER_WHITELIST ? "listed channels only"
                                                                              : "one eAxC and/or direction",
                 "Applied in the capture backend, before the queue, so filtered packets cost no decode "
                 "time. Single channel keeps one eAxC, one direction, or both. Whitelist keeps up to 64 "
                 "eAxCs, each with its own directions.");

        if (config->channel_filter_method == FH_CHANNEL_FILTER_WHITELIST) {
            draw_channel_whitelist(state, config);
        } else {
            ImGui::Checkbox("Filter by eAxC ID", &config->channel_has_eaxc);
            ImGui::SameLine();
            gui_hint("aka PC_ID",
                     "Keep only one eAxC, entered by its four fields below. The eAxC is the eCPRI PC_ID; "
                     "other ways of writing it are only accepted on the command line (--channel).");

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
            gui_hint("UL or DL", "Keep only uplink or only downlink packets.");
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
