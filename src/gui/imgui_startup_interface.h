#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct fh_startup_config_t {
    std::string iface = "lo";
    std::string demo_pcap_path = "../src/capture/demo.pcap";
    int demo_pcap_speed = 25;
    bool demo_pcap_loop = true;
    int snaplen = 10000;
    int timeout_ms = 100;
    bool timeout_enabled = false;
    unsigned long long queue_packets = 4096;
    unsigned long long queue_bytes = 10000;
    unsigned long long decode_threads = 8;
    unsigned long long memory_symbols = 1120;
    bool ud_comp_hdr_present = false;
    int ud_iq_width = 9;
    int ud_iq_width_decompressed = 16;
    std::string ud_comp_method = "BFP";
    int scs_khz = 30;
    bool channel_has_eaxc = false;
    int channel_du_port_id = 0;
    int channel_band_sector_id = 0;
    int channel_cc_id = 0;
    int channel_ru_port_id = 0;
    uint16_t channel_eaxc = 0;
    bool channel_has_direction = false;
    int channel_direction = 0;
};

typedef struct fh_startup_interface_state fh_startup_interface_state_t;

fh_startup_interface_state_t* imgui_startup_init(const fh_startup_config_t* initial_config);
bool imgui_startup_render(
    fh_startup_interface_state_t* state,
    fh_startup_config_t* config,
    int window_width,
    int window_height);
void imgui_startup_shutdown(fh_startup_interface_state_t* state);
