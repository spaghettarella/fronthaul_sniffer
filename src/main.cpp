#include "fronthaul_sniffer/capture/capture_factory.hpp"
#include "fronthaul_sniffer/pipeline/packet_processor.hpp"
#include "fronthaul_sniffer/pipeline/packet_queue.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true);
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
    fronthaul::CaptureOptions capture_options;
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
            capture_options.mode = next_arg("--mode");
        } else if (arg == "--iface") {
            capture_options.interface_name = next_arg("--iface");
        } else if (arg == "--filter") {
            capture_options.bpf_filter = next_arg("--filter");
        } else if (arg == "--snaplen") {
            capture_options.snaplen = std::stoi(next_arg("--snaplen"));
        } else if (arg == "--promisc") {
            capture_options.promiscuous = (std::stoi(next_arg("--promisc")) != 0);
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
        auto backend = fronthaul::make_capture_backend(capture_options);
        fronthaul::PacketQueue queue(queue_packets, queue_bytes);
        fronthaul::PacketProcessor processor;

        std::thread capture_thread([&] {
            backend->run(
                [&](fronthaul::RawPacket&& pkt) {
                    queue.push(std::move(pkt));
                },
                g_stop);
            g_stop.store(true);
            queue.notify_all();
        });

        while (!g_stop.load()) {
            fronthaul::RawPacket pkt;
            if (queue.pop(pkt, g_stop)) {
                processor.consume(pkt);
            }
            processor.maybe_report();
        }

        queue.notify_all();
        if (capture_thread.joinable()) {
            capture_thread.join();
        }

        const auto stats = queue.stats();
        std::cout << "[queue] dropped_packets=" << stats.dropped_packets
                  << " dropped_bytes=" << stats.dropped_bytes << '\n';
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
