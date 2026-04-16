#pragma once

#include "fronthaul_sniffer/capture/capture_backend.hpp"

#include <string>

namespace fronthaul {

struct SocketConfig {
    std::string interface_name{"lo"};
    int recv_timeout_ms{100};
    int snaplen{65535};
};

class SocketCapture final : public ICaptureBackend {
public:
    explicit SocketCapture(SocketConfig config);

    void run(const PacketHandler& on_packet, std::atomic<bool>& stop_flag) override;

private:
    SocketConfig config_;
};

}  // namespace fronthaul
