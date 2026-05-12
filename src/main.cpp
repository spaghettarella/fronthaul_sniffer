#include "capture/c_capture_socket_mode.h"
#include "capture/c_pcap_demo_stream.h"
#include "capture/c_capture_loopback_mode.h"
#include "core/c_fronthaul_decoder.h"
#include "core/c_oran_memory.h"
#include "pipeline/c_packet_queue.h"
#include "visualization/opengl_visualizer.h"
#include "gui/imgui_startup_interface.h"
#include "gui/imgui_visualizer_interface.h"

#include <GL/gl.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace {
struct startup_eaxc_fields_t {
    int du_port_id;
    int band_sector_id;
    int cc_id;
    int ru_port_id;
};

startup_eaxc_fields_t unpack_startup_eaxc(uint16_t eaxc_id) {
    return {
        static_cast<int>((eaxc_id >> 14) & 0x3u),
        static_cast<int>((eaxc_id >> 8) & 0x3Fu),
        static_cast<int>((eaxc_id >> 4) & 0xFu),
        static_cast<int>(eaxc_id & 0xFu),
    };
}
}  // namespace

// The memory key structs are defined in the GUI interface header
// and used here in the main program

namespace {
volatile sig_atomic_t g_stop = 0;
std::mutex g_memories_mutex;  // Protects memories map structure only
std::mutex g_memory_data_mutex;  // Global lock for per-packet save operations

void on_signal(int) {
    g_stop = 1;
}

void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "  --iface <name>                        Interface name (default: lo)\n"
        << "  --channel <eAxC_ID> <data_direction>  eAxC_ID (uint16) and data direction (UL/DL) to filter network capture before queue\n"
        << "  --snaplen <bytes>                     Max captured bytes per packet (default: 10000)\n"
        << "  --timeout-ms <ms>                     Read timeout in milliseconds (default: 100)\n"
        << "  --queue-packets <count>               Max packets buffered before processing (default: 4096)\n"
        << "  (NOTE) --snaplen will also set the queue-bytes value to the same number\n"
        << "  --decode-threads <count>              Number of parallel decode worker threads (default: 1)\n"
        << "  --memory-size <symbols>               Max symbols buffered per eAxC_ID and Direction (max: 573440; default: 2800)\n"
        << "  --ud-comp-hdr-present <bool>          Whether user data compression header is present (default: false)\n"
        << "  --ud-iq-width <bits>                  Width of user data IQ samples (default: 9)\n"
        << "  --ud-iq-width-decompressed <bits>     Width of user data IQ samples after decompression (default: 16)\n"
        << "  --ud-comp-method <method>             Compression method for user data (NONE or BFP; default: NONE)\n"
        << "  --scs-khz <value>                     Subcarrier spacing in kHz (default: 30)\n"
        << "  --demo-pcap <path> <speed>            Stream packets from a pcap file in <path> (instead of live capture) with <speed> packets per second\n"
        << "  --help, -h                            Show this help message\n";
}

int queue_callback(const fh_packet_t* pkt, void* user_ctx) {
    auto* queue = static_cast<fh_packet_queue_t*>(user_ctx);
    const int rc = fh_packet_queue_push_copy(queue, pkt);
    return (rc < 0) ? -1 : 0;
}

int fh_memory_manage(
    fh_packet_t* pkt,
    std::unordered_map<fh_memory_key_t, fh_oran_memory_t*>& memories,
    std::vector<fh_memory_key_t>& discovered_keys,
    fh_oran_memory_t*& display_memory,
    std::size_t memory_symbols,
    int scs_khz,
    bool ud_comp_hdr_present,
    int ud_iq_width,
    int ud_iq_width_decompressed,
    const char* ud_comp_method,
    std::mutex& memories_mutex,
    std::mutex& memory_data_mutex) {
    if (pkt == nullptr) {
        return -1;
    }

    // Decode once
    fh_decoded_packet_t decoded{};
    int rc = fh_decode_packet(pkt, &decoded, ud_comp_hdr_present, ud_iq_width, ud_iq_width_decompressed, ud_comp_method);
    if (rc == 0 && decoded.section_count > 0 && decoded.sections != nullptr) {
        const fh_memory_key_t key{decoded.sections[0].eaxc_id, decoded.sections[0].data_direction};
        
        fh_oran_memory_t* memory_buffer = nullptr;
        
        // Acquire map lock just for lookup/insert
        {
            std::lock_guard<std::mutex> lock(memories_mutex);
            auto it = memories.find(key);
            if (it == memories.end()) {
                fh_oran_memory_t* new_memory = fh_oran_memory_create(memory_symbols);
                if (new_memory != nullptr) {
                    memories[key] = new_memory;
                    discovered_keys.push_back(key);
                    if (display_memory == nullptr) {
                        display_memory = new_memory;
                    }
                    memory_buffer = new_memory;
                }
            } else {
                memory_buffer = it->second;
            }
        }
        
        // Save with global data lock
        if (memory_buffer != nullptr) {
            std::lock_guard<std::mutex> lock(memory_data_mutex);
            fh_oran_memory_save(memory_buffer, &decoded, scs_khz);
        }
    }

    fh_decoded_packet_free(&decoded);
    fh_packet_release(pkt);
    return rc;
}
}  // namespace closed

int main(int argc, char** argv) {
    fh_capture_socket_options_t capture_options{};
    std::string iface = "lo";
    capture_options.interface_name = iface.c_str();
    capture_options.snaplen = 10000;
    capture_options.timeout_ms = 100;

    std::size_t queue_packets = 4096;
    std::size_t queue_bytes = static_cast<std::size_t>(capture_options.snaplen);
    std::size_t texture_height = 200;

    bool timeout_specified = false;

    bool ud_comp_hdr_present = false;
    int ud_iq_width = 9;
    int ud_iq_width_decompressed = 16;
    std::string ud_comp_method = "BFP";
    int scs_khz = 30;
    bool demo_pcap_enabled = false;
    std::string demo_pcap_path = "../src/capture/demo.pcap";
    int demo_pcap_speed = 25;

    std::size_t memory_symbols = 1120;
    std::size_t decode_threads = 8;
    bool channel_has_eaxc = false;
    uint16_t channel_eaxc = 0;
    bool channel_has_direction = false;
    int channel_direction = -1; // 0 = UL, 1 = DL

    // Parse CLI options so the capture backend and queue can be tuned without recompiling.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_arg = [&](const char* opt_name) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("missing value for ") + opt_name);
            }
            return argv[++i];
        };

        if (arg == "--iface") {
            iface = next_arg("--iface");
        } else if (arg == "--channel") {
            // Accept up to two tokens after --channel in any order: <eAxC_ID> <UL|DL>
            std::string token1, token2;
            if (i + 1 < argc) {
                std::string peek = argv[i + 1];
                if (!peek.empty() && peek.rfind("--", 0) != 0) token1 = argv[++i];
            }
            if (i + 1 < argc) {
                std::string peek = argv[i + 1];
                if (!peek.empty() && peek.rfind("--", 0) != 0) token2 = argv[++i];
            }
            if (token1.empty() && token2.empty()) {
                throw std::invalid_argument("--channel requires at least one argument");
            }

            auto up = [](std::string s){ std::transform(s.begin(), s.end(), s.begin(), ::toupper); return s; };
            auto parse_eaxc = [&](const std::string &s)->uint16_t {
                // parse as hex (allows strings like 0001 or 0x1)
                unsigned long v = std::stoul(s, nullptr, 16);
                return static_cast<uint16_t>(v & 0xFFFFu);
            };

            // Determine which token is direction vs eaxc
            if (!token1.empty()) {
                std::string u1 = up(token1);
                if (u1 == "UL" || u1 == "DL") {
                    channel_has_direction = true;
                    channel_direction = (u1 == "DL") ? 1 : 0;
                    if (!token2.empty()) {
                        channel_has_eaxc = true;
                        channel_eaxc = parse_eaxc(token2);
                    }
                } else {
                    channel_has_eaxc = true;
                    channel_eaxc = parse_eaxc(token1);
                    if (!token2.empty()) {
                        std::string u2 = up(token2);
                        if (u2 == "UL" || u2 == "DL") {
                            channel_has_direction = true;
                            channel_direction = (u2 == "DL") ? 1 : 0;
                        } else {
                            throw std::invalid_argument("invalid --channel second argument: expected UL or DL");
                        }
                    }
                }
            }
        } else if (arg == "--snaplen") {
            capture_options.snaplen = std::stoi(next_arg("--snaplen"));
            // Combine: set queue_bytes to the same value as snaplen
            queue_bytes = static_cast<std::size_t>(capture_options.snaplen);
        } else if (arg == "--timeout-ms") {
            capture_options.timeout_ms = std::stoi(next_arg("--timeout-ms"));
            timeout_specified = true;
        } else if (arg == "--queue-packets") {
            queue_packets = static_cast<std::size_t>(std::stoull(next_arg("--queue-packets")));
        } else if (arg == "--decode-threads") {
            decode_threads = static_cast<std::size_t>(std::stoull(next_arg("--decode-threads")));
            if (decode_threads < 1) {
                throw std::invalid_argument("--decode-threads must be >= 1");
            }
        } else if (arg == "--memory-size") {
            memory_symbols = static_cast<std::size_t>(std::stoull(next_arg("--memory-size")));
        } else if (arg == "--ud-comp-hdr-present") {
            ud_comp_hdr_present = (next_arg("--ud-comp-hdr-present") == "true");
        } else if (arg == "--ud-iq-width") {
            ud_iq_width = std::stoi(next_arg("--ud-iq-width"));
        } else if (arg == "--ud-iq-width-decompressed") {
            ud_iq_width_decompressed = std::stoi(next_arg("--ud-iq-width-decompressed"));
        } else if (arg == "--ud-comp-method") {
            ud_comp_method = next_arg("--ud-comp-method");
        } else if (arg == "--scs-khz") {
            scs_khz = std::stoi(next_arg("--scs-khz"));
        } else if (arg == "--demo-pcap") {
            // Support `--demo-pcap` with optional <path> and optional <speed>.
            // With no arguments, use the default path relative to `build/`.
            if (i + 1 >= argc) {
                demo_pcap_enabled = true;
            } else {
                std::string peek = argv[i + 1];
                if (!peek.empty() && peek.rfind("--", 0) == 0) {
                    demo_pcap_enabled = true;
                } else {
                    demo_pcap_path = next_arg("--demo-pcap");
                    if (i + 1 < argc) {
                        std::string peek2 = argv[i + 1];
                        if (!peek2.empty() && peek2.rfind("--", 0) != 0) {
                            demo_pcap_speed = std::stoi(next_arg("--demo-pcap"));
                            if (demo_pcap_speed <= 0) {
                                throw std::invalid_argument("--demo-pcap speed must be > 0");
                            }
                        }
                    }
                    demo_pcap_enabled = true;
                }
            }
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage(argv[0]);
            return 1;
        }
    }

    // Register shutdown handlers; the worker loops watch this shared flag.
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Keep separate variables for potential debugging, but tie both behaviors to memory-size.
    texture_height = memory_symbols;

    try {
        // Rebind the interface string after parsing so the capture options keep a valid pointer.
        capture_options.interface_name = iface.c_str();

        fh_startup_config_t startup_config{};
        startup_config.iface = iface;
        startup_config.demo_pcap_path = demo_pcap_path;
        startup_config.demo_pcap_speed = demo_pcap_speed;
        startup_config.snaplen = capture_options.snaplen;
        startup_config.timeout_ms = capture_options.timeout_ms;
        startup_config.timeout_enabled = timeout_specified;
        startup_config.queue_packets = queue_packets;
        startup_config.queue_bytes = queue_bytes;
        startup_config.decode_threads = decode_threads;
        startup_config.memory_symbols = memory_symbols;
        startup_config.ud_comp_hdr_present = ud_comp_hdr_present;
        startup_config.ud_iq_width = ud_iq_width;
        startup_config.ud_iq_width_decompressed = ud_iq_width_decompressed;
        startup_config.ud_comp_method = ud_comp_method;
        startup_config.scs_khz = scs_khz;
        startup_config.channel_has_eaxc = channel_has_eaxc;
        if (channel_has_eaxc) {
            const startup_eaxc_fields_t channel_fields = unpack_startup_eaxc(channel_eaxc);
            startup_config.channel_eaxc = channel_eaxc;
            startup_config.channel_du_port_id = channel_fields.du_port_id;
            startup_config.channel_band_sector_id = channel_fields.band_sector_id;
            startup_config.channel_cc_id = channel_fields.cc_id;
            startup_config.channel_ru_port_id = channel_fields.ru_port_id;
        }
        startup_config.channel_direction = (channel_direction >= 0) ? channel_direction : 0;

        // Per-stream memory buffers keyed by (eAxC_ID, data_direction).
        std::unordered_map<fh_memory_key_t, fh_oran_memory_t*> memories;
        // Keep discovery order so we can print the list of active streams later.
        std::vector<fh_memory_key_t> discovered_keys;
        // The visualizer renders one memory instance; by default we show the first one seen.
        fh_oran_memory_t* display_memory = nullptr;

        // Create visualizer window for heatmap display (must be done on main thread)
        opengl_visualizer_t* visualizer = visualizer_create(800, 600, static_cast<int>(texture_height));
        if (visualizer == nullptr) {
            std::cerr << "warning: failed to create visualizer, continuing without display\n";
        }

        // Initialize ImGui for stream selection interface
        imgui_visualizer_state_t* imgui_state = nullptr;
        if (visualizer != nullptr) {
            imgui_state = imgui_visualizer_init(800, 600);
            if (imgui_state == nullptr) {
                std::cerr << "warning: failed to initialize ImGui, continuing without stream selector\n";
            }
        }

        fh_startup_interface_state_t* startup_state = nullptr;
        if (argc == 1) {
            startup_state = imgui_startup_init(&startup_config);
            if (startup_state == nullptr) {
                std::cerr << "warning: failed to initialize startup configuration UI, continuing with defaults\n";
            }

            if (visualizer != nullptr && imgui_state != nullptr && startup_state != nullptr) {
                bool startup_complete = false;
                while (visualizer_is_running(visualizer) && g_stop == 0 && !startup_complete) {
                    if (visualizer_update(visualizer, nullptr) < 0) {
                        g_stop = 1;
                        break;
                    }

                    // Draw orange background behind the ImGui startup window
                    glMatrixMode(GL_PROJECTION);
                    glPushMatrix();
                    glLoadIdentity();
                    glOrtho(0, 800, 600, 0, -1, 1);
                    glMatrixMode(GL_MODELVIEW);
                    glPushMatrix();
                    glLoadIdentity();
                    
                    glDisable(GL_TEXTURE_2D);
                    glBegin(GL_QUADS);
                    glColor3f(1.0f, 0.5f, 0.0f);
                    glVertex2f(0, 0);
                    glVertex2f(800, 0);
                    glVertex2f(800, 600);
                    glVertex2f(0, 600);
                    glEnd();
                    
                    glMatrixMode(GL_PROJECTION);
                    glPopMatrix();
                    glMatrixMode(GL_MODELVIEW);
                    glPopMatrix();
                    glColor3f(1.0f, 1.0f, 1.0f);

                    startup_complete = imgui_startup_render(startup_state, &startup_config, 800, 600);
                    imgui_visualizer_present();
                    visualizer_present(visualizer);
                }

                if (!startup_complete && g_stop == 0) {
                    throw std::runtime_error("startup window closed before configuration was confirmed");
                }
                
                // Reset to default black background for main visualization
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            }

            imgui_startup_shutdown(startup_state);

            iface = startup_config.iface;
            demo_pcap_path = startup_config.demo_pcap_path;
            demo_pcap_speed = startup_config.demo_pcap_speed;
            capture_options.snaplen = startup_config.snaplen;
            if (startup_config.timeout_enabled) {
                capture_options.timeout_ms = startup_config.timeout_ms;
            } else {
                capture_options.timeout_ms = 0; // disable timeout
            }
            queue_packets = static_cast<std::size_t>(startup_config.queue_packets);
            queue_bytes = static_cast<std::size_t>(startup_config.queue_bytes);
            decode_threads = static_cast<std::size_t>(startup_config.decode_threads);
            if (decode_threads < 1) {
                decode_threads = 1;
            }
            memory_symbols = static_cast<std::size_t>(startup_config.memory_symbols);
            ud_comp_hdr_present = startup_config.ud_comp_hdr_present;
            ud_iq_width = startup_config.ud_iq_width;
            ud_iq_width_decompressed = startup_config.ud_iq_width_decompressed;
            ud_comp_method = startup_config.ud_comp_method;
            scs_khz = startup_config.scs_khz;
            channel_has_eaxc = startup_config.channel_has_eaxc;
            channel_has_direction = startup_config.channel_has_direction;
            if (channel_has_eaxc) {
                channel_eaxc = startup_config.channel_eaxc;
            } else {
                channel_eaxc = 0;
            }
            if (channel_has_direction) {
                channel_direction = startup_config.channel_direction;
            } else {
                channel_direction = -1;
            }
            texture_height = memory_symbols;
            capture_options.interface_name = iface.c_str();
        }

        // Build the bounded packet queue and decoded-packet storage used by the pipeline.
        fh_packet_queue_t* queue = fh_packet_queue_create(queue_packets, queue_bytes);
        if (queue == nullptr) {
            throw std::runtime_error("failed to create packet queue");
        }

        // Run packet capture on a background thread (just network I/O/file replay, no OpenGL)
        std::thread capture_thread([&] {
            char errbuf[256] = {0};
            int rc = 0;
            if (demo_pcap_enabled || iface == "pcap") {
                fh_pcap_stream_options_t demo_options{};
                demo_options.pcap_path = demo_pcap_path.c_str();
                demo_options.demo_pcap_timeout = 1.0 / static_cast<double>(demo_pcap_speed);
                // If timeout is disabled (capture_options.timeout_ms == 0), keep replaying the pcap
                // instead of exiting when the file ends.
                if (capture_options.timeout_ms == 0) {
                    rc = 0;
                    while (rc == 0 && *(&g_stop) == 0) {
                        rc = fh_pcap_stream_run(
                            &demo_options,
                            queue_callback,
                            queue,
                            &g_stop,
                            errbuf,
                            sizeof(errbuf));
                        if (rc != 0) break; // error - stop replaying
                        // otherwise EOF reached; loop to restart playback
                    }
                } else {
                    rc = fh_pcap_stream_run(
                        &demo_options,
                        queue_callback,
                        queue,
                        &g_stop,
                        errbuf,
                        sizeof(errbuf));
                }
            } else if (iface == "lo") {
                // Use tcpdump backend for loopback interface
                fh_capture_loopback_options_t loopback_options{};
                loopback_options.interface_name = iface.c_str();
                loopback_options.snaplen = capture_options.snaplen;
                loopback_options.timeout_ms = capture_options.timeout_ms;
                // Apply CLI channel filters if provided
                loopback_options.filter_has_eaxc = channel_has_eaxc ? 1 : 0;
                loopback_options.filter_eaxc = channel_eaxc;
                loopback_options.filter_has_direction = channel_has_direction ? 1 : 0;
                loopback_options.filter_direction = channel_direction;
                rc = fh_capture_loopback_run(
                    &loopback_options,
                    queue_callback,
                    queue,
                    &g_stop,
                    errbuf,
                    sizeof(errbuf));
            } else {
                rc = fh_capture_socket_run(
                    &capture_options,
                    queue_callback,
                    queue,
                    &g_stop,
                    errbuf,
                    sizeof(errbuf));
            }
            if (rc != 0) {
                std::cerr << "capture error: " << errbuf << '\n';
                g_stop = 1;
            } else if (capture_options.timeout_ms != 0) {
                // If not in looping mode for pcap replay, stop when capture finishes cleanly.
                g_stop = 1;
            }
            fh_packet_queue_notify_all(queue);
        });

        // Run packet decode on background threads (multiple workers for throughput).
        // The same helper is used both during normal operation and during shutdown draining
        // so that packet handling stays in one place.
        std::vector<std::thread> decode_workers;
        for (std::size_t w = 0; w < decode_threads; ++w) {
            decode_workers.emplace_back([&] {
                while (g_stop == 0) {
                    fh_packet_t pkt{};
                    if (fh_packet_queue_pop(queue, &pkt, &g_stop) > 0) {
                        fh_memory_manage(
                            &pkt,
                            memories,
                            discovered_keys,
                            display_memory,
                            memory_symbols,
                            scs_khz,
                            ud_comp_hdr_present,
                            ud_iq_width,
                            ud_iq_width_decompressed,
                            ud_comp_method.c_str(),
                            g_memories_mutex,
                            g_memory_data_mutex);
                    }
                }
                
                // Drain any remaining packets after shutdown
                for (;;) {
                    fh_packet_t pkt{};
                    if (fh_packet_queue_pop(queue, &pkt, &g_stop) <= 0) {
                        break;
                    }
                    fh_memory_manage(
                        &pkt,
                        memories,
                        discovered_keys,
                        display_memory,
                        memory_symbols,
                        scs_khz,
                        ud_comp_hdr_present,
                        ud_iq_width,
                        ud_iq_width_decompressed,
                        ud_comp_method.c_str(),
                        g_memories_mutex,
                        g_memory_data_mutex);
                }
            });
        }

        // Main thread runs visualizer render loop with OpenGL calls
        int render_result = 0;
        if (visualizer != nullptr) {
            while (visualizer_is_running(visualizer) && g_stop == 0) {
                // Build array of memory pointers for ImGui interface (protected by mutex)
                std::vector<fh_oran_memory_t*> memory_ptrs;
                std::vector<fh_memory_key_t> keys_snapshot;
                {
                    std::lock_guard<std::mutex> lock(g_memories_mutex);
                    keys_snapshot = discovered_keys;
                    for (const auto& key : keys_snapshot) {
                        auto it = memories.find(key);
                        if (it != memories.end()) {
                            memory_ptrs.push_back(it->second);
                        } else {
                            memory_ptrs.push_back(nullptr);
                        }
                    }
                }

                // Update and obtain live queue stats for UI
                fh_queue_stats_t stats = fh_packet_queue_stats(queue);

                // Render ImGui stream selection interface
                if (imgui_state != nullptr && !keys_snapshot.empty()) {
                    display_memory = imgui_visualizer_render(
                        imgui_state,
                        keys_snapshot.data(),
                        keys_snapshot.size(),
                        memory_ptrs.data(),
                        display_memory,
                        800,
                        600,
                        stats);
                }

                // Render heatmap visualization
                // No lock needed here since display_memory pointer doesn't change and renderer reads are safe
                render_result = visualizer_update(visualizer, display_memory);
                if (render_result < 0) {
                    g_stop = 1;
                    continue;
                }

                if (imgui_state != nullptr && !discovered_keys.empty()) {
                    imgui_visualizer_present();
                }

                visualizer_present(visualizer);
            }
        } else {
            // If no visualizer, block on g_stop
            while (g_stop == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // Wake any waiters, then join the threads
        fh_packet_queue_notify_all(queue);
        if (capture_thread.joinable()) {
            capture_thread.join();
        }
        for (auto& worker : decode_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        // Print the queue and memory counters so the run can be inspected from the terminal.
        const fh_queue_stats_t stats = fh_packet_queue_stats(queue);
        const size_t attempted_queue_packets = stats.total_packets;
        const double dropped_pct = (attempted_queue_packets > 0)
            ? (100.0 * static_cast<double>(stats.dropped_packets) / static_cast<double>(attempted_queue_packets))
            : 0.0;
        std::cout << "[queue] attempted_queue_packets=" << attempted_queue_packets
                  << " processed_packets=" << stats.processed_packets
                  << " queued_packets=" << stats.queued_packets
                  << " dropped_packets=" << stats.dropped_packets
                  << " dropped_pct=" << dropped_pct << "%\n";
        std::cout << "[queue] dropped_packets=" << stats.dropped_packets
                  << " dropped_bytes=" << stats.dropped_bytes << '\n';

        std::cout << "[memory] tracked_streams=" << discovered_keys.size() << '\n';
        for (const auto& key : discovered_keys) {
            auto it = memories.find(key);
            if (it == memories.end() || it->second == nullptr) {
                continue;
            }
            const fh_oran_memory_stats_t mem_stats = fh_oran_memory_stats(it->second);
            int occupancy = (mem_stats.symbol_highest >= mem_stats.symbol_lowest)
                                ? (mem_stats.symbol_highest - mem_stats.symbol_lowest + 1)
                                : 0;
            std::cout << "[memory] eaxc_id=" << key.eaxc_id
                      << " data_direction=" << static_cast<int>(key.data_direction)
                      << " occupancy=" << occupancy
                      << " max_capacity=" << mem_stats.max_symbols
                      << " highest=" << mem_stats.symbol_highest
                      << " lowest=" << mem_stats.symbol_lowest << '\n';
        }

        // Release resources in reverse order of creation.
        if (imgui_state != nullptr) {
            imgui_visualizer_shutdown(imgui_state);
        }
        if (visualizer != nullptr) {
            visualizer_destroy(visualizer);
        }
        for (auto& entry : memories) {
            if (entry.second != nullptr) {
                fh_oran_memory_destroy(entry.second);
                entry.second = nullptr;
            }
        }
        fh_packet_queue_destroy(queue);
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
