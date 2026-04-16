#pragma once

#include "fronthaul_sniffer/core/raw_packet.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace fronthaul {

using PacketHandler = std::function<void(RawPacket&&)>;

class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    virtual void run(const PacketHandler& on_packet, std::atomic<bool>& stop_flag) = 0;
};

using CapturePtr = std::unique_ptr<ICaptureBackend>;

}  // namespace fronthaul
