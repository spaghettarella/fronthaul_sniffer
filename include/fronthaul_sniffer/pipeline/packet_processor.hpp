#pragma once

#include "fronthaul_sniffer/core/raw_packet.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>

namespace fronthaul {

class PacketProcessor {
public:
    void consume(const RawPacket& pkt);
    void maybe_report();

private:
    std::size_t packets_{0};
    std::size_t bytes_{0};
    std::chrono::steady_clock::time_point last_report_{std::chrono::steady_clock::now()};
};

}  // namespace fronthaul
