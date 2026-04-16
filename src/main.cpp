#include "fronthaul_sniffer/capture/c_capture.h"
#include "fronthaul_sniffer/pipeline/packet_processor.hpp"
#include "fronthaul_sniffer/pipeline/c_packet_queue.h"

#include <csignal>
#include <cstdlib>
#include <exception>
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
        << "  --mode <pcap|socket>        Capture backend (default: pcap)\n"
        << "  --iface <name>              Interface name (default: lo)\n"
        << "  --filter <bpf>              BPF filter for pcap mode\n"
        << "  --snaplen <bytes>           Max captured bytes per packet\n"
        << "  --promisc <0|1>             Promiscuous mode for pcap\n"
        << "  --timeout-ms <ms>           Read timeout\n"
        << "  --queue-packets <count>     Max packets buffered before processing\n"
        << "  --queue-bytes <bytes>       Max bytes buffered before processing\n";
}
}  // namespace

int main(int argc, char** argv) {
    fh_capture_options_t capture_options{};
    std::string mode = "pcap";
    std::string iface = "lo";
    std::string filter;

    capture_options.mode = mode.c_str();
    capture_options.interface_name = iface.c_str();
    capture_options.bpf_filter = filter.c_str();
    capture_options.snaplen = 65535;
    capture_options.promiscuous = 0;
    capture_options.timeout_ms = 100;

    std::size_t queue_packets = 4096;
    std::size_t queue_bytes = 32 * 1024 * 1024;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_arg = [&](const char* opt_name) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("missing value for ") + opt_name);
            }
            return argv[++i];
        };

        if (arg == "--mode") {
            mode = next_arg("--mode");
        } else if (arg == "--iface") {
            iface = next_arg("--iface");
        } else if (arg == "--filter") {
            filter = next_arg("--filter");
        } else if (arg == "--snaplen") {
            capture_options.snaplen = std::stoi(next_arg("--snaplen"));
        } else if (arg == "--promisc") {
            capture_options.promiscuous = std::stoi(next_arg("--promisc")) != 0 ? 1 : 0;
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
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    try {
        capture_options.mode = mode.c_str();
        capture_options.interface_name = iface.c_str();
        capture_options.bpf_filter = filter.c_str();

        fh_packet_queue_t* queue = fh_packet_queue_create(queue_packets, queue_bytes);
        if (queue == nullptr) {
            throw std::runtime_error("failed to create packet queue");
        }

        fronthaul::PacketProcessor processor;

        std::thread capture_thread([&] {
            char errbuf[256] = {0};
            auto on_packet = [](const fh_packet_t* pkt, void* user_ctx) -> int {
                auto* q = static_cast<fh_packet_queue_t*>(user_ctx);
                const int rc = fh_packet_queue_push_copy(q, pkt);
                return (rc < 0) ? -1 : 0;
            };

            const int rc = fh_capture_run(&capture_options, on_packet, queue, &g_stop, errbuf, sizeof(errbuf));
            if (rc != 0) {
                std::cerr << "capture error: " << errbuf << '\n';
            }
            g_stop = 1;
            fh_packet_queue_notify_all(queue);
        });

        while (g_stop == 0) {
            fh_packet_t pkt{};
            if (fh_packet_queue_pop(queue, &pkt, &g_stop) > 0) {
                processor.consume(pkt);
                fh_packet_release(&pkt);
            }
            processor.maybe_report();
        }

        for (;;) {
            fh_packet_t pkt{};
            if (fh_packet_queue_pop(queue, &pkt, &g_stop) <= 0) {
                break;
            }
            processor.consume(pkt);
            fh_packet_release(&pkt);
        }

        fh_packet_queue_notify_all(queue);
        if (capture_thread.joinable()) {
            capture_thread.join();
        }

        const auto stats = fh_packet_queue_stats(queue);
        std::cout << "[queue] dropped_packets=" << stats.dropped_packets
                  << " dropped_bytes=" << stats.dropped_bytes << '\n';

        fh_packet_queue_destroy(queue);
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
