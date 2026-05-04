#include "capture/c_capture_socket_mode.h"
#include "core/c_fronthaul_decoder.h"
#include "core/c_oran_memory.h"
#include "pipeline/c_packet_queue.h"
#include "visualization/opengl_visualizer.h"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
volatile sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "  --iface <name>                        Interface name (default: lo)\n"
        << "  --snaplen <bytes>                     Max captured bytes per packet (default: 65535)\n"
        << "  --timeout-ms <ms>                     Read timeout in milliseconds (default: 100)\n"
        << "  --queue-packets <count>               Max packets buffered before processing (default: 4096)\n"
        << "  --queue-bytes <bytes>                 Max bytes buffered before processing (default: 32MiB)\n"
        << "  --memory-size <symbols>               Max symbols buffered (max: 573440; default: 2800)\n"
        << "  --ud-comp-hdr-present <bool>          Whether user data compression header is present (default: false)\n"
        << "  --ud-iq-width <bits>                  Width of user data IQ samples (default: 16)\n"
        << "  --ud-iq-width-decompressed <bits>     Width of user data IQ samples after decompression (default: 16)\n"
        << "  --ud-comp-method <method>             Compression method for user data (NONE or BFP; default: NONE)\n"
        << "  --scs_khz <value>                     Subcarrier spacing in kHz (default: 30)\n";
}

int queue_callback(const fh_packet_t* pkt, void* user_ctx) {
    auto* queue = static_cast<fh_packet_queue_t*>(user_ctx);
    const int rc = fh_packet_queue_push_copy(queue, pkt);
    return (rc < 0) ? -1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
    fh_capture_socket_options_t capture_options{};
    std::string iface = "lo";
    capture_options.interface_name = iface.c_str();
    capture_options.snaplen = 65535;
    capture_options.timeout_ms = 100;

    std::size_t queue_packets = 4096;
    std::size_t queue_bytes = 32u * 1024u * 1024u;

    bool ud_comp_hdr_present = false;
    int ud_iq_width = 16;
    int ud_iq_width_decompressed = 16;
    const char *ud_comp_method = "NONE";
    int scs_khz = 30;

    std::size_t memory_symbols = 2800;

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
        } else if (arg == "--snaplen") {
            capture_options.snaplen = std::stoi(next_arg("--snaplen"));
        } else if (arg == "--timeout-ms") {
            capture_options.timeout_ms = std::stoi(next_arg("--timeout-ms"));
        } else if (arg == "--queue-packets") {
            queue_packets = static_cast<std::size_t>(std::stoull(next_arg("--queue-packets")));
        } else if (arg == "--queue-bytes") {
            queue_bytes = static_cast<std::size_t>(std::stoull(next_arg("--queue-bytes")));
        } else if (arg == "--memory-size") {
            memory_symbols = static_cast<std::size_t>(std::stoull(next_arg("--memory-size")));
        } else if (arg == "--ud-comp-hdr-present") {
            ud_comp_hdr_present = (next_arg("--ud-comp-hdr-present") == "true");
        } else if (arg == "--ud-iq-width") {
            ud_iq_width = std::stoi(next_arg("--ud-iq-width"));
        } else if (arg == "--ud-iq-width-decompressed") {
            ud_iq_width_decompressed = std::stoi(next_arg("--ud-iq-width-decompressed"));
        } else if (arg == "--ud-comp-method") {
            ud_comp_method = next_arg("--ud-comp-method").c_str();
        } else if (arg == "--scs_khz") {
            scs_khz = std::stoi(next_arg("--scs_khz"));
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

    try {
        // Rebind the interface string after parsing so the capture options keep a valid pointer.
        capture_options.interface_name = iface.c_str();

        // Build the bounded packet queue and decoded-packet storage used by the pipeline.
        fh_packet_queue_t* queue = fh_packet_queue_create(queue_packets, queue_bytes);
        if (queue == nullptr) {
            throw std::runtime_error("failed to create packet queue");
        }

        fh_oran_memory_t* memory = fh_oran_memory_create(memory_symbols);
        if (memory == nullptr) {
            fh_packet_queue_destroy(queue);
            throw std::runtime_error("failed to create ORAN memory (Not enough physical memory?)");
        }

        // Create visualizer window for heatmap display (must be done on main thread)
        opengl_visualizer_t* visualizer = visualizer_create(800, 600);
        if (visualizer == nullptr) {
            std::cerr << "warning: failed to create visualizer, continuing without display\n";
        }

        // Run packet capture on a background thread (just network I/O, no OpenGL)
        std::thread capture_thread([&] {
            char errbuf[256] = {0};
            const int rc = fh_capture_socket_run(
                &capture_options,
                queue_callback,
                queue,
                &g_stop,
                errbuf,
                sizeof(errbuf));
            if (rc != 0) {
                std::cerr << "capture error: " << errbuf << '\n';
            }
            g_stop = 1;
            fh_packet_queue_notify_all(queue);
        });

        // Run packet decode on a background thread
        std::thread decode_thread([&] {
            while (g_stop == 0) {
                fh_packet_t pkt{};
                if (fh_packet_queue_pop(queue, &pkt, &g_stop) > 0) {
                    fh_decoded_packet_t decoded{};
                    if (fh_decode_packet(&pkt, &decoded, ud_comp_hdr_present, ud_iq_width, ud_iq_width_decompressed, ud_comp_method) == 0) {
                        fh_oran_memory_save(memory, &decoded, scs_khz);
                    }
                    fh_decoded_packet_free(&decoded);
                    fh_packet_release(&pkt);
                }
            }
            
            // Drain any remaining packets after shutdown
            for (;;) {
                fh_packet_t pkt{};
                if (fh_packet_queue_pop(queue, &pkt, &g_stop) <= 0) {
                    break;
                }
                fh_decoded_packet_t decoded{};
                if (fh_decode_packet(&pkt, &decoded, ud_comp_hdr_present, ud_iq_width, ud_iq_width_decompressed, ud_comp_method) == 0) {
                    fh_oran_memory_save(memory, &decoded, scs_khz);
                }
                fh_decoded_packet_free(&decoded);
                fh_packet_release(&pkt);
            }
        });

        // Main thread runs visualizer render loop with OpenGL calls
        int render_result = 0;
        if (visualizer != nullptr) {
            while (visualizer_is_running(visualizer) && g_stop == 0) {
                render_result = visualizer_update(visualizer, memory);
                if (render_result < 0) {
                    g_stop = 1;
                }
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
        if (decode_thread.joinable()) {
            decode_thread.join();
        }

        // Print the queue and memory counters so the run can be inspected from the terminal.
        const fh_queue_stats_t stats = fh_packet_queue_stats(queue);
        std::cout << "[queue] dropped_packets=" << stats.dropped_packets
                  << " dropped_bytes=" << stats.dropped_bytes << '\n';

        const fh_oran_memory_stats_t mem_stats = fh_oran_memory_stats(memory);
        int occupancy = (mem_stats.symbol_highest >= mem_stats.symbol_lowest) ? 
                        (mem_stats.symbol_highest - mem_stats.symbol_lowest + 1) : 0;
        std::cout << "[memory] occupancy=" << occupancy
                  << " max_capacity=" << mem_stats.max_symbols 
                  << " highest=" << mem_stats.symbol_highest 
                  << " lowest=" << mem_stats.symbol_lowest << '\n';

        // Release resources in reverse order of creation.
        if (visualizer != nullptr) {
            visualizer_destroy(visualizer);
        }
        fh_oran_memory_destroy(memory);
        fh_packet_queue_destroy(queue);
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
