#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fronthaul {

struct RawPacket {
    std::chrono::system_clock::time_point timestamp;
    std::string ingress_if;
    std::vector<std::uint8_t> bytes;
    std::size_t original_len{0};
};

}  // namespace fronthaul
