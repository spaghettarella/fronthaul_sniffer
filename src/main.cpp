#include "capture/c_capture_socket_mode.h"
#include "core/c_fronthaul_decoder.h"
#include "core/c_oran_memory.h"
#include "pipeline/c_packet_queue.h"

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
        << "  --iface <name>              Interface name (default: lo)\n"
        << "  --snaplen <bytes>           Max captured bytes per packet (default: 65535)\n"
        << "  --timeout-ms <ms>           Read timeout in milliseconds (default: 100)\n"
        << "  --queue-packets <count>     Max packets buffered before processing (default: 4096)\n"
        << "  --queue-bytes <bytes>       Max bytes buffered before processing (default: 32MiB)\n";
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

    std::size_t memory_symbols = 10000;

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
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage(argv[0]);
            return 1;
        }
    }

    // Register simple shutdown handlers; the worker loops watch this shared flag.
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
            throw std::runtime_error("failed to create ORAN memory");
        }

        // Run packet capture on a background thread so the main thread can decode and store.
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

        // Consume packets from the queue until shutdown is requested.
        while (g_stop == 0) {
            fh_packet_t pkt{};
            if (fh_packet_queue_pop(queue, &pkt, &g_stop) > 0) {
                fh_decoded_packet_t decoded{};
                if (fh_decode_packet(&pkt, &decoded, ud_comp_hdr_present, ud_iq_width, ud_iq_width_decompressed, ud_comp_method) == 0) {
                    fh_oran_memory_save(memory, &decoded);
                }
                fh_decoded_packet_free(&decoded);
                fh_packet_release(&pkt);
            }
        }

        // Drain any packets left in the queue after the capture thread stops.
        for (;;) {
            fh_packet_t pkt{};
            if (fh_packet_queue_pop(queue, &pkt, &g_stop) <= 0) {
                break;
            }
            fh_decoded_packet_t decoded{};
            if (fh_decode_packet(&pkt, &decoded, ud_comp_hdr_present, ud_iq_width, ud_iq_width_decompressed, ud_comp_method) == 0) {
                fh_oran_memory_save(memory, &decoded);
            }
            fh_decoded_packet_free(&decoded);
            fh_packet_release(&pkt);
        }

        // Wake any waiters, then join the capture thread before reading final stats.
        fh_packet_queue_notify_all(queue);
        if (capture_thread.joinable()) {
            capture_thread.join();
        }

        // Print the queue and memory counters so the run can be inspected from the terminal.
        const fh_queue_stats_t stats = fh_packet_queue_stats(queue);
        std::cout << "[queue] dropped_packets=" << stats.dropped_packets
                  << " dropped_bytes=" << stats.dropped_bytes << '\n';

        const fh_oran_memory_stats_t mem_stats = fh_oran_memory_stats(memory);
        std::cout << "[memory] stored_packets=" << mem_stats.stored_packets
                  << " max_packets=" << mem_stats.max_packets << '\n';

        // Release resources in reverse order of creation.
        fh_oran_memory_destroy(memory);
        fh_packet_queue_destroy(queue);
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
