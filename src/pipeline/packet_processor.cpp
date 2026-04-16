#include "fronthaul_sniffer/pipeline/packet_processor.hpp"

#include <chrono>
#include <iostream>

namespace fronthaul {

void PacketProcessor::consume(const RawPacket& pkt) {
    ++packets_;
    bytes_ += pkt.bytes.size();
}

void PacketProcessor::maybe_report() {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_report_).count();
    if (elapsed < 1) {
        return;
    }

    const double mbps = (bytes_ * 8.0) / (1000.0 * 1000.0 * static_cast<double>(elapsed));
    std::cout << "[processor] packets=" << packets_ << " bytes=" << bytes_ << " throughput_mbps=" << mbps << '\n';

    packets_ = 0;
    bytes_ = 0;
    last_report_ = now;
}

}  // namespace fronthaul
